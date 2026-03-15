#pragma once
#include <optional>
#include <string>
#include <vector>

namespace config {

struct IPSource {
    std::string              interface_name; ///< network interface (optional)
    std::vector<std::string> urls;           ///< fallback HTTP API URLs
};

struct GeneralConfig {
    IPSource    get_ip;
    std::string work_dir;
    std::string log_output;  ///< "shell" or file path
    std::string proxy;       ///< global proxy (optional)
};

struct CloudflareRecord {
    std::string api_token;
    std::string zone_id;  ///< auto-fetched if empty
    bool        proxied = false;
    int         ttl     = 0; ///< 0 = use parent record's ttl
};

struct AliyunRecord {
    std::string access_key_id;
    std::string access_key_secret;
    int         ttl = 0;
};

struct RecordConfig {
    std::string                      provider;
    std::string                      zone;
    std::string                      record;
    int                              ttl       = 0;
    bool                             proxied   = false;
    bool                             use_proxy = false;
    std::optional<CloudflareRecord>  cloudflare;
    std::optional<AliyunRecord>      aliyun;
};

struct Config {
    GeneralConfig           general;
    std::vector<RecordConfig> records;
};

/// Read and validate config JSON. Returns nullopt on any error.
std::optional<Config> read_config(const std::string& path);

/// Write config to file (used to persist auto-fetched zone_id, etc.)
bool write_config(const std::string& path, const Config& cfg);

/// Returns the cache file path next to the config file (or in work_dir)
std::string get_cache_file_path(const std::string& config_abs_path,
                                 const std::string& work_dir);

/// Read last IP from cache file (empty string on miss)
std::string read_last_ip(const std::string& path);

/// Write IP to cache file
bool write_last_ip(const std::string& path, const std::string& ip);

/// Effective proxy URL for a record (empty = none)
std::string get_record_proxy(const Config& cfg, const RecordConfig& record);

/// Effective TTL for a record
int get_record_ttl(const RecordConfig& record);

} // namespace config
