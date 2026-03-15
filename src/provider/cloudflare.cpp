#include "cloudflare.hpp"
#include "../log.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace provider {

namespace {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    reinterpret_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

} // anonymous namespace

CloudflareProvider::CloudflareProvider(std::string api_token, std::string proxy_url)
    : api_token_(std::move(api_token)), proxy_url_(std::move(proxy_url)) {}

CloudflareProvider::HttpResponse
CloudflareProvider::cf_request(const std::string& method,
                                const std::string& url,
                                const std::string& body_json) {
    constexpr int MAX_RETRIES = 3;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) return {-1, "curl_easy_init failed"};

        std::string response_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        // Auth & content-type headers
        struct curl_slist* headers = nullptr;
        std::string auth_hdr = "Authorization: Bearer " + api_token_;
        headers = curl_slist_append(headers, auth_hdr.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Method / body
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_json.size());
        } else if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_json.size());
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        // GET is the default

        // Proxy
        if (!proxy_url_.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url_.c_str());
        }

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
                continue;
            }
            return {-1, std::string("curl error: ") + curl_easy_strerror(res)};
        }

        if (http_code >= 500 && attempt < MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }

        return {http_code, response_body};
    }
    return {-1, "max retries exceeded"};
}

// ─── Get Zone ID ──────────────────────────────────────────────────────────────

std::string CloudflareProvider::get_zone_id(const std::string& zone_name,
                                             const std::string& zone_id_hint,
                                             std::string&       error_out) {
    if (!zone_id_hint.empty()) return zone_id_hint;

    std::string url = "https://api.cloudflare.com/client/v4/zones?name=" + zone_name;
    auto resp = cf_request("GET", url);
    if (resp.code != 200) {
        error_out = "GET zones returned HTTP " + std::to_string(resp.code);
        return "";
    }

    try {
        auto j = json::parse(resp.body);
        if (!j.value("success", false) || !j.contains("result") || j["result"].empty()) {
            error_out = "Zone not found for: " + zone_name;
            if (j.contains("errors") && !j["errors"].empty())
                error_out += ". API error: " + j["errors"][0].value("message", "");
            return "";
        }
        return j["result"][0]["id"].get<std::string>();
    } catch (const std::exception& e) {
        error_out = std::string("JSON parse error: ") + e.what();
        return "";
    }
}

// ─── Upsert record ────────────────────────────────────────────────────────────

bool CloudflareProvider::upsert_record(const std::string& zone,
                                        const std::string& record_name,
                                        const std::string& ip,
                                        int                ttl,
                                        const std::map<std::string, std::string>& extra) {
    bool proxied = false;
    auto it = extra.find("proxied");
    if (it != extra.end()) proxied = (it->second == "true" || it->second == "1");

    // We need zone_id; resolve it now (callers from main pass it via extra too)
    std::string zone_id;
    auto zit = extra.find("zone_id");
    if (zit != extra.end()) zone_id = zit->second;

    if (zone_id.empty()) {
        std::string err;
        zone_id = get_zone_id(zone, "", err);
        if (zone_id.empty()) {
            logger::error("Cloudflare: failed to get zone_id for %s: %s", zone.c_str(), err.c_str());
            return false;
        }
    }

    return upsert_record_with_zone_id(zone, record_name, ip, zone_id, ttl, proxied);
}

bool CloudflareProvider::upsert_record_with_zone_id(const std::string& zone,
                                                     const std::string& record_name,
                                                     const std::string& ip,
                                                     const std::string& zone_id,
                                                     int                ttl,
                                                     bool               proxied) {
    std::string fqdn = (record_name == "@") ? zone : (record_name + "." + zone);
    std::string search_url = "https://api.cloudflare.com/client/v4/zones/"
                           + zone_id + "/dns_records?type=AAAA&name=" + fqdn;

    auto search_resp = cf_request("GET", search_url);
    if (search_resp.code != 200) {
        logger::error("Cloudflare: search DNS record returned HTTP %ld", search_resp.code);
        return false;
    }

    json new_record = {
        {"type",    "AAAA"},
        {"name",    fqdn},
        {"content", ip},
        {"ttl",     ttl},
        {"proxied", proxied}
    };

    try {
        auto sj = json::parse(search_resp.body);
        if (!sj.value("success", false)) {
            std::string msg;
            if (sj.contains("errors") && !sj["errors"].empty())
                msg = sj["errors"][0].value("message", "unknown");
            logger::error("Cloudflare: DNS search failed: %s", msg.c_str());
            return false;
        }

        std::string method, endpoint;

        if (!sj["result"].empty()) {
            auto& existing = sj["result"][0];
            std::string existing_ip      = existing.value("content", "");
            bool        existing_proxied = existing.value("proxied", false);
            int         existing_ttl     = existing.value("ttl", 0);

            if (existing_ip == ip && existing_proxied == proxied && existing_ttl == ttl) {
                logger::info("Cloudflare: record %s already up-to-date", fqdn.c_str());
                return true;
            }
            std::string record_id = existing["id"].get<std::string>();
            method   = "PUT";
            endpoint = "https://api.cloudflare.com/client/v4/zones/"
                     + zone_id + "/dns_records/" + record_id;
        } else {
            method   = "POST";
            endpoint = "https://api.cloudflare.com/client/v4/zones/"
                     + zone_id + "/dns_records";
        }

        std::string body = new_record.dump();
        auto put_resp = cf_request(method, endpoint, body);

        auto rj = json::parse(put_resp.body);
        if (!rj.value("success", false)) {
            std::string msg;
            if (rj.contains("errors") && !rj["errors"].empty())
                msg = rj["errors"][0].value("message", "unknown");
            logger::error("Cloudflare: %s failed: %s", method.c_str(), msg.c_str());
            return false;
        }
        return true;

    } catch (const std::exception& e) {
        logger::error("Cloudflare: JSON error: %s", e.what());
        return false;
    }
}

} // namespace provider
