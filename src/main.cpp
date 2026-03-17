#include "log.hpp"
#include "config.hpp"
#include "cache.hpp"
#include "ip_getter.hpp"
#include "provider/cloudflare.hpp"
#include "provider/aliyun.hpp"

#include <argparse/argparse.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─── Version (injected by CMake via -D flags) ─────────────────────────────────
#ifndef APP_VERSION
#  define APP_VERSION "dev"
#endif
#ifndef APP_COMMIT
#  define APP_COMMIT ""
#endif
#ifndef APP_BUILD_DATE
#  define APP_BUILD_DATE ""
#endif

// ─── Signal handling ──────────────────────────────────────────────────────────
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int) {
    g_shutdown.store(true);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void print_version() {
    std::cout << "cppddns " APP_VERSION "\n";
    if (std::string(APP_COMMIT).size() > 0)
        std::cout << "commit: " APP_COMMIT "\n";
    if (std::string(APP_BUILD_DATE).size() > 0)
        std::cout << "built:  " APP_BUILD_DATE "\n";
}

// ─── Update a single DNS record ───────────────────────────────────────────────

struct UpdateResult {
    std::string record_name;
    bool        success = false;
    std::string error;
};

static UpdateResult update_single_record(const config::Config&       cfg,
                                          const config::RecordConfig& rec,
                                          const std::string&          current_ip,
                                          const std::string&          zone_id_cache_file) {
    UpdateResult result;
    result.record_name = rec.record + "." + rec.zone;

    if (g_shutdown.load()) {
        result.error = "shutdown requested";
        return result;
    }

    logger::info("Processing record: %s (%s)", result.record_name.c_str(), rec.provider.c_str());

    std::string proxy_url = config::get_record_proxy(cfg, rec);
    int         ttl       = config::get_record_ttl(rec);

    if (rec.provider == "cloudflare") {
        if (!rec.cloudflare) {
            result.error = "missing cloudflare config";
            logger::error("Record %s: %s", result.record_name.c_str(), result.error.c_str());
            return result;
        }

        auto provider = provider::CloudflareProvider(rec.cloudflare->api_token, proxy_url);

        std::string zone_id = rec.cloudflare->zone_id;

        // Apply provider-level TTL and proxied overrides
        if (rec.cloudflare->ttl > 0) ttl = rec.cloudflare->ttl;
        bool proxied = rec.proxied || rec.cloudflare->proxied;

        // Auto-fetch zone_id if not set (try cache first)
        if (zone_id.empty()) {
            // Try to read from ZoneID cache
            auto cached_zone_ids = config::read_zone_id_cache(zone_id_cache_file);
            auto it = cached_zone_ids.find(rec.zone);
            if (it != cached_zone_ids.end() && !it->second.empty()) {
                zone_id = it->second;
                logger::debug("Zone ID loaded from cache for %s: %s", rec.zone.c_str(), zone_id.c_str());
            }
        }

        if (zone_id.empty()) {
            logger::info("Zone ID not configured, fetching for zone: %s", rec.zone.c_str());
            std::string err;
            zone_id = provider.get_zone_id(rec.zone, "", err);
            if (zone_id.empty()) {
                result.error = "Failed to get Zone ID: " + err;
                logger::error("Record %s: %s", result.record_name.c_str(), result.error.c_str());
                return result;
            }
            logger::info("Zone ID fetched: %s", zone_id.c_str());

            // Save to cache
            if (!config::update_zone_id_cache(zone_id_cache_file, rec.zone, zone_id)) {
                logger::warning("Warning: failed to save Zone ID cache");
            }
        }

        bool ok = provider.upsert_record_with_zone_id(
            rec.zone, rec.record, current_ip, zone_id, ttl, proxied);

        if (!ok) {
            result.error = "Cloudflare upsert failed";
            logger::error("Failed to update %s", result.record_name.c_str());
            return result;
        }

    } else if (rec.provider == "aliyun") {
        if (!rec.aliyun) {
            result.error = "missing aliyun config";
            logger::error("Record %s: %s", result.record_name.c_str(), result.error.c_str());
            return result;
        }

        if (!proxy_url.empty()) {
            logger::warning("Aliyun provider does not support proxy, ignoring use_proxy setting");
        }

        if (rec.aliyun->ttl > 0) ttl = rec.aliyun->ttl;

        auto provider = provider::AliyunProvider(
            rec.aliyun->access_key_id, rec.aliyun->access_key_secret);

        std::map<std::string, std::string> extra;
        bool ok = provider.upsert_record(rec.zone, rec.record, current_ip, ttl, extra);
        if (!ok) {
            result.error = "Aliyun upsert failed";
            logger::error("Failed to update %s", result.record_name.c_str());
            return result;
        }

    } else {
        result.error = "unsupported provider: " + rec.provider;
        logger::error("Record %s: %s", result.record_name.c_str(), result.error.c_str());
        return result;
    }

    logger::success("Record %s updated successfully", result.record_name.c_str());
    result.success = true;

    return result;
}

// ─── Run command ──────────────────────────────────────────────────────────────

static int run_cmd(const std::string& config_path, bool ignore_cache, int timeout_sec) {
    // Register signal handlers
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Resolve absolute config path
    std::error_code ec;
    auto abs_config = std::filesystem::absolute(config_path, ec);
    if (ec) {
        std::cerr << "Failed to resolve config path: " << ec.message() << "\n";
        return 1;
    }

    // Load config
    auto cfg_opt = config::read_config(abs_config.string());
    if (!cfg_opt) {
        std::cerr << "Failed to load configuration\n";
        return 1;
    }
    config::Config cfg = std::move(*cfg_opt);

    // Init logging
    if (!logger::init(cfg.general.log_output)) {
        std::cerr << "Failed to initialize logging\n";
        return 1;
    }

    logger::info("cppddns starting with %zu record(s)", cfg.records.size());

    // ── Get current IPv6 ──────────────────────────────────────────────────────
    std::vector<ip_getter::IPv6Info> infos;
    std::string ip_err;

    if (!cfg.general.get_ip.interface_name.empty()) {
        infos = ip_getter::get_from_interface(cfg.general.get_ip.interface_name, ip_err);
        if (infos.empty()) {
            logger::info("Interface %s failed: %s. Trying API fallback...",
                      cfg.general.get_ip.interface_name.c_str(), ip_err.c_str());
            infos = ip_getter::get_from_apis(cfg.general.get_ip.urls, ip_err);
        }
    } else {
        infos = ip_getter::get_from_apis(cfg.general.get_ip.urls, ip_err);
    }

    if (infos.empty()) {
        logger::error("Failed to get current IP: %s", ip_err.c_str());
        return 1;
    }

    std::string current_ip = ip_getter::select_best(infos, ip_err);
    if (current_ip.empty()) {
        logger::error("Failed to select best IPv6: %s", ip_err.c_str());
        return 1;
    }

    logger::info("Current IPv6 address: %s", current_ip.c_str());

    // ── Cache check ────────────────────────────────────────────────────────────
    std::string cache_file = config::get_cache_file_path(
        abs_config.string(), cfg.general.work_dir);
    std::string last_ip = cache::read_last_ip(cache_file);

    if (!ignore_cache && !last_ip.empty()) {
        if (last_ip == current_ip) {
            logger::info("IP has not changed since last run: %s", current_ip.c_str());
        } else {
            logger::info("IP changed from %s to %s", last_ip.c_str(), current_ip.c_str());
        }
    }

    // ── Update all records in parallel ────────────────────────────────────────
    std::string              zone_id_cache_file = config::get_zone_id_cache_path(abs_config.string());
    std::vector<UpdateResult> results(cfg.records.size());
    std::vector<std::thread>  threads;
    threads.reserve(cfg.records.size());

    for (size_t i = 0; i < cfg.records.size(); ++i) {
        threads.emplace_back([&, i]() {
            if (g_shutdown.load()) return;
            results[i] = update_single_record(cfg, cfg.records[i], current_ip, zone_id_cache_file);
        });
    }

    // Wait for threads with timeout
    auto start = std::chrono::steady_clock::now();
    for (auto& t : threads) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed >= timeout_sec) {
            logger::warning("Timeout reached (%d seconds), forcing shutdown", timeout_sec);
            g_shutdown.store(true);
            break;
        }
        if (t.joinable()) {
            t.join();
        }
    }

    // ── Summarize ─────────────────────────────────────────────────────────────
    int success_count = 0, fail_count = 0;
    for (const auto& r : results) {
        if (r.success) ++success_count; else ++fail_count;
    }

    logger::info("Update completed: %d succeeded, %d failed", success_count, fail_count);

    // Update cache only if IP changed and at least one succeeded
    bool any_success = (success_count > 0);
    if (any_success && last_ip != current_ip) {
        if (!cache::write_last_ip(cache_file, current_ip)) {
            logger::warning("Warning: failed to write cache file");
        }
    }

    return fail_count > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("cppddns", APP_VERSION);
    program.add_description("强大的动态 DNS 客户端 - 支持多域名多服务商");

    // ── Sub-command: run ──────────────────────────────────────────────────────
    argparse::ArgumentParser run_cmd_parser("run");
    run_cmd_parser.add_description("运行 DDNS 更新");
    run_cmd_parser.add_argument("-f", "--config")
        .help("配置文件路径 (JSON 格式)")
        .default_value(std::string(""));
    run_cmd_parser.add_argument("-i", "--ignore-cache")
        .help("忽略缓存 IP，强制更新")
        .default_value(false)
        .implicit_value(true);
    run_cmd_parser.add_argument("-t", "--timeout")
        .help("超时时间（秒），默认 300 秒")
        .default_value(300);

    // ── Sub-command: version ──────────────────────────────────────────────────
    argparse::ArgumentParser version_cmd("version");
    version_cmd.add_description("显示版本信息");

    program.add_subparser(run_cmd_parser);
    program.add_subparser(version_cmd);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::cerr << program;
        return 1;
    }

    if (program.is_subcommand_used("version")) {
        print_version();
        return 0;
    }

    if (program.is_subcommand_used("run")) {
        std::string config_path = run_cmd_parser.get<std::string>("--config");
        if (config_path.empty()) {
            std::cerr << "错误: 缺少配置文件参数 --config / -f\n";
            std::cerr << run_cmd_parser;
            return 1;
        }
        bool ignore_cache = run_cmd_parser.get<bool>("--ignore-cache");
        int timeout = run_cmd_parser.get<int>("--timeout");
        return run_cmd(config_path, ignore_cache, timeout);
    }

    std::cerr << program;
    return 1;
}
