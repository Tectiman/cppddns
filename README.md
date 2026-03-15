# cppddns — 动态 DNS 客户端 (C++17)

[cppddns](./cppddns) 是 [goddns](../goddns) 的 C++17 重写版本，功能完全对等，配置文件格式兼容。

## 特性

- **多域名支持**：单次运行可并发更新多个 DNS 记录
- **Cloudflare 集成**：AAAA 记录自动创建/更新，Zone ID 自动获取
- **阿里云 DNS**：HMAC-SHA1 签名，AAAA 记录自动创建/更新
- **IPv6 支持**：Linux netlink 接口获取 + HTTP API 降级
- **代理支持**：HTTP/HTTPS/SOCKS5（仅 Cloudflare）
- **IP 缓存**：避免重复 API 调用
- **彩色日志**：终端下分级彩色显示，支持文件输出

## 系统依赖

```bash
# Ubuntu / Debian
sudo apt install cmake build-essential libcurl4-openssl-dev libssl-dev
```

> `nlohmann/json` 和 `argparse` 由 CMake FetchContent 自动拉取，无需手动安装。

## 构建

```bash
# 直接构建（dev 版本）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 带版本信息构建
./build.sh v1.0.0
```

## 运行

```bash
# 更新 DNS 记录（使用缓存，IP 未变则跳过）
./build/cppddns run -f config.json

# 强制更新（忽略缓存）
./build/cppddns run -f config.json -i

# 查看版本
./build/cppddns version
```

## 配置文件

与 Go 版本 **完全兼容**，可直接复用：

```json
{
    "general": {
        "get_ip": {
            "interface": "enp6s18",
            "urls": [
                "https://ipv6.icanhazip.com",
                "https://6.ipw.cn"
            ]
        },
        "work_dir": "",
        "log_output": "shell",
        "proxy": ""
    },
    "records": [
        {
            "provider": "cloudflare",
            "zone": "example.com",
            "record": "dev",
            "ttl": 180,
            "proxied": false,
            "use_proxy": false,
            "cloudflare": {
                "api_token": "YOUR_CLOUDFLARE_API_TOKEN",
                "zone_id": ""
            }
        },
        {
            "provider": "aliyun",
            "zone": "example.cn",
            "record": "www",
            "ttl": 600,
            "aliyun": {
                "access_key_id": "YOUR_ACCESS_KEY_ID",
                "access_key_secret": "YOUR_ACCESS_KEY_SECRET"
            }
        }
    ]
}
```

## 配置字段说明

### general

| 字段 | 说明 |
|------|------|
| `get_ip.interface` | 本地网卡名（优先） |
| `get_ip.urls` | 外部 IPv6 检测 API 列表（降级） |
| `work_dir` | 缓存文件目录（空则与配置文件同目录） |
| `log_output` | `shell` 输出到终端，或日志文件路径 |
| `proxy` | 全局代理（socks5://... 或 http://...） |

### records

| 字段 | 说明 |
|------|------|
| `provider` | `cloudflare` 或 `aliyun` |
| `zone` | 主域名 |
| `record` | 子域名（`@` 表示根域） |
| `ttl` | TTL（秒） |
| `use_proxy` | 是否使用全局代理（仅 Cloudflare） |

## 自动运行（systemd）

```ini
# /etc/systemd/system/cppddns.service
[Unit]
Description=DDNS client (cppddns)
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/cppddns run -f /etc/cppddns/config.json
```

```ini
# /etc/systemd/system/cppddns.timer
[Unit]
Description=Run cppddns every 5 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min
Persistent=true

[Install]
WantedBy=timers.target
```

## 目录结构

```
cppddns/
├── CMakeLists.txt
├── build.sh
├── config.example.json
├── README.md
└── src/
    ├── main.cpp
    ├── log.hpp / log.cpp
    ├── config.hpp / config.cpp
    ├── cache.hpp / cache.cpp
    ├── ip_getter.hpp / ip_getter.cpp
    └── provider/
        ├── provider.hpp
        ├── cloudflare.hpp / cloudflare.cpp
        └── aliyun.hpp / aliyun.cpp
```
