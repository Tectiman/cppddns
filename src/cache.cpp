#include "cache.hpp"
#include <fstream>
#include <string>

namespace cache {

std::string read_last_ip(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string ip;
    std::getline(f, ip);
    while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' '))
        ip.pop_back();
    return ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << ip;
    return f.good();
}

} // namespace cache
