#include "ip_getter.hpp"
#include "log.hpp"

// Linux netlink headers
#include <arpa/inet.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

// CURL for HTTP API fallback
#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ip_getter {

// ─── Utilities ────────────────────────────────────────────────────────────────

static bool is_link_local(const uint8_t* addr16) {
    return addr16[0] == 0xfe && (addr16[1] & 0xc0) == 0x80;
}

static bool is_loopback(const uint8_t* addr16) {
    for (int i = 0; i < 15; ++i) if (addr16[i] != 0) return false;
    return addr16[15] == 1;
}

static bool is_ula(const uint8_t* addr16) {
    return (addr16[0] == 0xfc || addr16[0] == 0xfd);
}

static std::string format_ipv6(const uint8_t* addr16) {
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, addr16, buf, sizeof(buf));
    return buf;
}

/// Populate IPv6Info fields (matching Go version's PopulateInfo)
static void populate_info(IPv6Info* info) {
    if (info->ip.empty()) return;

    // Convert IP to bytes for checking
    uint8_t addr[16];
    if (inet_pton(AF_INET6, info->ip.c_str(), addr) != 1) return;

    info->is_unique_local = is_ula(addr);

    if (is_link_local(addr)) {
        info->scope = "Link Local";
    } else if (info->is_unique_local) {
        info->scope = "Unique Local (ULA)";
    } else {
        info->scope = "Global Unicast";
    }

    info->is_deprecated = (info->preferred_lft <= 0 && info->valid_lft > 0);

    if (info->valid_lft == 0) {
        info->address_state = "Expired";
    } else if (info->is_deprecated) {
        info->address_state = "Deprecated";
    } else if (info->preferred_lft < info->valid_lft) {
        info->address_state = "Preferred/Dynamic";
    } else {
        info->address_state = "Preferred/Static";
    }

    info->is_candidate = (info->scope == "Global Unicast" && 
                          !info->is_deprecated && 
                          !info->is_unique_local && 
                          info->valid_lft > 0);
}

// ─── Netlink IPv6 getter ──────────────────────────────────────────────────────

std::vector<IPv6Info> get_from_interface(const std::string& iface_name,
                                          std::string&       error_out) {
    unsigned int iface_idx = if_nametoindex(iface_name.c_str());
    if (iface_idx == 0) {
        error_out = "Interface not found: " + iface_name;
        return {};
    }

    // Open netlink socket
    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (sock < 0) {
        error_out = std::string("socket() failed: ") + strerror(errno);
        return {};
    }

    // Send RTM_GETADDR request
    struct {
        nlmsghdr  nlh;
        ifaddrmsg ifa;
    } req{};
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(ifaddrmsg));
    req.nlh.nlmsg_type  = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = 1;
    req.ifa.ifa_family  = AF_INET6;

    if (send(sock, &req, req.nlh.nlmsg_len, 0) < 0) {
        error_out = std::string("send() failed: ") + strerror(errno);
        close(sock);
        return {};
    }

    // Read response
    std::vector<IPv6Info> result;
    char buf[8192];

    while (true) {
        ssize_t len = recv(sock, buf, sizeof(buf), 0);
        if (len < 0) {
            error_out = std::string("recv() failed: ") + strerror(errno);
            close(sock);
            return {};
        }

        const nlmsghdr* nlh = reinterpret_cast<const nlmsghdr*>(buf);
        for (; NLMSG_OK(nlh, (unsigned)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE)  { goto done; }
            if (nlh->nlmsg_type == NLMSG_ERROR) { error_out = "netlink error"; close(sock); return {}; }
            if (nlh->nlmsg_type != RTM_NEWADDR)  continue;

            const ifaddrmsg* ifa = reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(nlh));
            if (ifa->ifa_index != iface_idx) continue;
            if (ifa->ifa_family != AF_INET6) continue;

            // Parse attributes
            const rtattr* rta = IFA_RTA(ifa);
            int rta_len = IFA_PAYLOAD(nlh);

            const uint8_t* addr = nullptr;
            uint32_t preferred_lft = 0, valid_lft = 0;
            bool deprecated = (ifa->ifa_flags & IFA_F_DEPRECATED) != 0;

            for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
                if (rta->rta_type == IFA_ADDRESS) {
                    addr = reinterpret_cast<const uint8_t*>(RTA_DATA(rta));
                }
                if (rta->rta_type == IFA_CACHEINFO) {
                    const ifa_cacheinfo* ci = reinterpret_cast<const ifa_cacheinfo*>(RTA_DATA(rta));
                    preferred_lft = ci->ifa_prefered;
                    valid_lft     = ci->ifa_valid;
                }
            }

            if (!addr) continue;
            if (is_link_local(addr) || is_loopback(addr)) continue;
            if (valid_lft == 0) continue; // expired

            IPv6Info info;
            info.ip            = format_ipv6(addr);
            info.preferred_lft = (preferred_lft == 0xFFFFFFFF) ? (long)1e12 : (long)preferred_lft;
            info.valid_lft     = (valid_lft     == 0xFFFFFFFF) ? (long)1e12 : (long)valid_lft;
            info.is_deprecated = deprecated;
            populate_info(&info);

            if (info.is_candidate) result.push_back(info);
        }
    }
done:
    close(sock);
    if (result.empty()) error_out = "No suitable IPv6 address on interface " + iface_name;
    return result;
}

// ─── HTTP API getter ──────────────────────────────────────────────────────────

namespace {

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = reinterpret_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string fetch_ip_from_url(const std::string& url, std::string& err) {
    constexpr int MAX_RETRIES = 2;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) { err = "curl_easy_init failed"; return ""; }

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6); // force IPv6

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            err = curl_easy_strerror(res);
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            return "";
        }

        if (http_code != 200) {
            err = "HTTP " + std::to_string(http_code);
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            return "";
        }

        // Take first line and trim
        std::string line = trim(body.substr(0, body.find('\n')));
        if (line.empty()) {
            err = "Empty response";
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            return "";
        }

        // Validate as IPv6
        uint8_t addr[16];
        if (inet_pton(AF_INET6, line.c_str(), addr) != 1) {
            err = "Not a valid IPv6: " + line;
            return "";
        }
        if (is_link_local(addr) || is_loopback(addr) || is_ula(addr)) {
            err = "Private/local address not suitable: " + line;
            return "";
        }
        return line;
    }
    return "";
}

} // anonymous namespace

std::vector<IPv6Info> get_from_apis(const std::vector<std::string>& urls,
                                     std::string&                    error_out) {
    if (urls.empty()) { error_out = "No API URLs configured"; return {}; }

    for (const auto& url : urls) {
        logger::info("Querying API: %s", url.c_str());
        std::string err;
        std::string ip = fetch_ip_from_url(url, err);
        if (!ip.empty()) {
            logger::info("API %s succeeded: %s", url.c_str(), ip.c_str());
            IPv6Info info;
            info.ip            = ip;
            info.preferred_lft = (long)1e12; // treat as permanent
            info.valid_lft     = (long)1e12;
            populate_info(&info);
            return {info};
        }
        logger::error("API %s failed: %s", url.c_str(), err.c_str());
        error_out = err;
    }
    return {};
}

// ─── Select best ─────────────────────────────────────────────────────────────

std::string select_best(const std::vector<IPv6Info>& infos, std::string& error_out) {
    const IPv6Info* best = nullptr;
    for (const auto& info : infos) {
        if (!info.is_candidate) continue;
        if (!best || info.preferred_lft > best->preferred_lft) {
            best = &info;
        }
    }
    if (!best) {
        error_out = "No suitable global unicast IPv6 candidate found";
        return "";
    }
    return best->ip;
}

} // namespace ip_getter
