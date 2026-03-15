#include "config.hpp"
#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace config {

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static std::string jstr(const json& j, const char* key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static bool jbool(const json& j, const char* key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

static int jint(const json& j, const char* key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<int>();
    return def;
}

// ─── Validation helpers ───────────────────────────────────────────────────────

static bool validate_proxy_url(const std::string& proxy) {
    if (proxy.empty()) return true;
    std::string lower = proxy;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.rfind("http://", 0) == 0) return true;
    if (lower.rfind("https://", 0) == 0) return true;
    if (lower.rfind("socks5://", 0) == 0) return true;
    if (lower.rfind("socks5h://", 0) == 0) return true;
    return false;
}

static bool validate_config(const Config& cfg) {
    if (cfg.records.empty()) {
        logger::error("Config: at least one record must be configured");
        return false;
    }

    bool has_iface = !cfg.general.get_ip.interface_name.empty();
    bool has_urls  = !cfg.general.get_ip.urls.empty();
    if (!has_iface && !has_urls) {
        logger::error("Config: either 'get_ip.interface' or 'get_ip.urls' must be set");
        return false;
    }

    if (!cfg.general.proxy.empty() && !validate_proxy_url(cfg.general.proxy)) {
        logger::error("Config: invalid global proxy format '%s'", cfg.general.proxy.c_str());
        return false;
    }

    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const auto& r = cfg.records[i];
        if (r.provider.empty()) {
            logger::error("Config: record[%zu]: provider is required", i);
            return false;
        }
        if (r.zone.empty()) {
            logger::error("Config: record[%zu]: zone is required", i);
            return false;
        }
        if (r.record.empty()) {
            logger::error("Config: record[%zu]: record name is required", i);
            return false;
        }
        if (r.use_proxy && cfg.general.proxy.empty()) {
            logger::error("Config: record[%zu]: use_proxy=true but no global proxy set", i);
            return false;
        }

        if (r.provider == "cloudflare") {
            if (!r.cloudflare || r.cloudflare->api_token.empty()) {
                logger::error("Config: record[%zu]: cloudflare.api_token is required", i);
                return false;
            }
        } else if (r.provider == "aliyun") {
            if (!r.aliyun ||
                r.aliyun->access_key_id.empty() ||
                r.aliyun->access_key_secret.empty()) {
                logger::error("Config: record[%zu]: aliyun access_key_id and access_key_secret are required", i);
                return false;
            }
        } else {
            logger::error("Config: record[%zu]: unsupported provider '%s'", i, r.provider.c_str());
            return false;
        }
    }
    return true;
}

// ─── Parse ────────────────────────────────────────────────────────────────────

std::optional<Config> read_config(const std::string& path) {
    std::error_code ec;
    fs::path abs_path = fs::absolute(path, ec);
    if (ec) {
        logger::error("Failed to resolve config path '%s': %s", path.c_str(), ec.message().c_str());
        return std::nullopt;
    }

    std::ifstream f(abs_path);
    if (!f.is_open()) {
        logger::error("Failed to open config file: %s", abs_path.c_str());
        return std::nullopt;
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        logger::error("Failed to parse config JSON: %s", e.what());
        return std::nullopt;
    }

    Config cfg;

    // general
    if (root.contains("general")) {
        const auto& g = root["general"];
        cfg.general.log_output = jstr(g, "log_output", "shell");
        cfg.general.work_dir   = jstr(g, "work_dir");
        cfg.general.proxy      = jstr(g, "proxy");

        if (g.contains("get_ip")) {
            const auto& gi = g["get_ip"];
            cfg.general.get_ip.interface_name = jstr(gi, "interface");
            if (gi.contains("urls") && gi["urls"].is_array()) {
                for (const auto& u : gi["urls"]) {
                    if (u.is_string()) cfg.general.get_ip.urls.push_back(u.get<std::string>());
                }
            }
        }
    }

    // records
    if (root.contains("records") && root["records"].is_array()) {
        for (const auto& rj : root["records"]) {
            RecordConfig r;
            r.provider  = jstr(rj, "provider");
            r.zone      = jstr(rj, "zone");
            r.record    = jstr(rj, "record");
            r.ttl       = jint(rj, "ttl");
            r.proxied   = jbool(rj, "proxied");
            r.use_proxy = jbool(rj, "use_proxy");

            if (rj.contains("cloudflare") && rj["cloudflare"].is_object()) {
                const auto& cj = rj["cloudflare"];
                CloudflareRecord cr;
                cr.api_token = jstr(cj, "api_token");
                cr.zone_id   = jstr(cj, "zone_id");
                cr.proxied   = jbool(cj, "proxied");
                cr.ttl       = jint(cj, "ttl");
                r.cloudflare = cr;
            }

            if (rj.contains("aliyun") && rj["aliyun"].is_object()) {
                const auto& aj = rj["aliyun"];
                AliyunRecord ar;
                ar.access_key_id     = jstr(aj, "access_key_id");
                ar.access_key_secret = jstr(aj, "access_key_secret");
                ar.ttl               = jint(aj, "ttl");
                r.aliyun = ar;
            }

            cfg.records.push_back(std::move(r));
        }
    }

    if (!validate_config(cfg)) return std::nullopt;

    return cfg;
}

// ─── Write ────────────────────────────────────────────────────────────────────

bool write_config(const std::string& path, const Config& cfg) {
    json root;
    root["general"]["log_output"]       = cfg.general.log_output;
    root["general"]["work_dir"]         = cfg.general.work_dir;
    root["general"]["proxy"]            = cfg.general.proxy;
    root["general"]["get_ip"]["interface"] = cfg.general.get_ip.interface_name;
    root["general"]["get_ip"]["urls"]   = cfg.general.get_ip.urls;

    root["records"] = json::array();
    for (const auto& r : cfg.records) {
        json rj;
        rj["provider"]  = r.provider;
        rj["zone"]      = r.zone;
        rj["record"]    = r.record;
        rj["ttl"]       = r.ttl;
        rj["proxied"]   = r.proxied;
        rj["use_proxy"] = r.use_proxy;

        if (r.cloudflare) {
            rj["cloudflare"]["api_token"] = r.cloudflare->api_token;
            rj["cloudflare"]["zone_id"]   = r.cloudflare->zone_id;
            rj["cloudflare"]["proxied"]   = r.cloudflare->proxied;
            rj["cloudflare"]["ttl"]       = r.cloudflare->ttl;
        }
        if (r.aliyun) {
            rj["aliyun"]["access_key_id"]     = r.aliyun->access_key_id;
            rj["aliyun"]["access_key_secret"] = r.aliyun->access_key_secret;
            rj["aliyun"]["ttl"]               = r.aliyun->ttl;
        }
        root["records"].push_back(std::move(rj));
    }

    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << root.dump(4) << "\n";
    return f.good();
}

// ─── Cache helpers ────────────────────────────────────────────────────────────

std::string get_cache_file_path(const std::string& config_abs_path,
                                 const std::string& work_dir) {
    if (!work_dir.empty()) {
        std::error_code ec;
        fs::create_directories(work_dir, ec);
        if (!ec) {
            return (fs::path(work_dir) / "cache.lastip").string();
        }
        logger::error("Failed to create work_dir '%s': %s", work_dir.c_str(), ec.message().c_str());
    }
    return (fs::path(config_abs_path).parent_path() / "cache.lastip").string();
}

std::string read_last_ip(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string ip;
    std::getline(f, ip);
    // trim whitespace
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    };
    trim(ip);
    return ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << ip;
    return f.good();
}

// ─── Record helpers ───────────────────────────────────────────────────────────

std::string get_record_proxy(const Config& cfg, const RecordConfig& record) {
    if (!record.use_proxy) return "";
    return cfg.general.proxy;
}

int get_record_ttl(const RecordConfig& record) {
    if (record.ttl > 0) return record.ttl;
    if (record.provider == "cloudflare") return 180;
    return 600;
}

} // namespace config
