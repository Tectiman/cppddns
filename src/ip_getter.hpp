#pragma once
#include <string>
#include <vector>

namespace ip_getter {

/// IPv6Info contains information about an IPv6 address
/// (matching Go version's ifaddr.IPv6Info)
struct IPv6Info {
    std::string ip;
    std::string scope;           ///< "Link Local", "Unique Local (ULA)", "Global Unicast"
    std::string address_state;   ///< "Expired", "Deprecated", "Preferred/Dynamic", "Preferred/Static"
    long        preferred_lft = 0; ///< seconds
    long        valid_lft     = 0;
    bool        is_deprecated = false;
    bool        is_unique_local = false;
    bool        is_candidate  = false; ///< Whether it is a DDNS candidate
};

/// Get IPv6 addresses from interface using Linux netlink.
/// Returns empty vector + sets error_out on failure.
std::vector<IPv6Info> get_from_interface(const std::string& iface_name,
                                          std::string&       error_out);

/// Query multiple HTTP APIs (tries each, returns first success).
std::vector<IPv6Info> get_from_apis(const std::vector<std::string>& urls,
                                     std::string&                    error_out);

/// Select the best (longest PreferredLft) global unicast candidate.
/// Returns empty string on failure.
std::string select_best(const std::vector<IPv6Info>& infos, std::string& error_out);

} // namespace ip_getter
