# cppddns — 动态 DNS 客户端 (C++17)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD-blue)](README.md)

[cppddns](./cppddns) 是 [goddns](../goddns) 的 C++17 重写版本，功能完全对等，配置文件格式兼容。

## 特性

- **多域名支持**：单次运行可并发更新多个 DNS 记录
- **Cloudflare 集成**：AAAA 记录自动创建/更新，Zone ID 自动获取
- **阿里云 DNS**：HMAC-SHA1 签名，AAAA 记录自动创建/更新
- **IPv6 支持**：
  - Linux：netlink 接口
  - FreeBSD/OpenBSD：ioctl 接口
  - 所有平台：HTTP API 降级
- **代理支持**：HTTP/HTTPS/SOCKS5（仅 Cloudflare）
- **IP 缓存**：避免重复 API 调用
- **彩色日志**：终端下分级彩色显示，支持文件输出

## 系统依赖

### Linux (Ubuntu / Debian)
```bash
sudo apt install cmake build-essential libcurl4-openssl-dev libssl-dev
```

### FreeBSD
```bash
pkg install cmake gcc curl openssl
```

### OpenBSD
```bash
pkg_add cmake curl openssl
```

> `nlohmann/json` 和 `argparse` 由 CMake FetchContent 自动拉取，无需手动安装。

## 构建

### 使用 g++ 编译（默认）

```bash
# 直接构建（dev 版本）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 或指定使用 g++
CXX=g++ cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 使用 clang++ 编译

```bash
CXX=clang++ cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 带版本信息构建

```bash
# 使用构建脚本
./build.sh v1.0.0

# 或手动设置编译参数
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DAPP_VERSION=v1.0.0 \
  -DAPP_COMMIT=$(git rev-parse HEAD) \
  -DAPP_BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cmake --build build -j$(nproc)
```

### 编译器支持

✅ **g++** (GCC 7+ with C++17 support)
✅ **clang++** (Clang 5+ with C++17 support)

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

### 安全性要求

**重要**：出于安全考虑，cppddns **禁止在配置文件中明文存储密钥信息**。
所有敏感信息（API Token、AccessKey 等）必须使用环境变量引用。

❌ 错误示例（明文密钥，会被拒绝执行）：
```json
{
    "cloudflare": {
        "api_token": "your_actual_token_here"
    }
}
```

✅ 正确示例（使用环境变量）：
```json
{
    "cloudflare": {
        "api_token": "${CLOUDFLARE_API_TOKEN}",
        "zone_id": "${CLOUDFLARE_ZONE_ID:-}"
    }
}
```

运行前设置环境变量：
```bash
export CLOUDFLARE_API_TOKEN="your_token_here"
export ALIYUN_ACCESS_KEY_ID="LTAI1234567890"
export ALIYUN_ACCESS_KEY_SECRET="your_secret_here"
./build/cppddns run -f config.json
```

### 配置示例

```json
{
    "general": {
        "get_ip": {
            "interface": "eth0",
            "urls": [
                "https://ipv6.icanhazip.com",
                "https://6.ipw.cn"
            ]
        },
        "work_dir": "/opt/cppddns",
        "log_output": "shell",
        "proxy": ""
    },
    "records": [
        {
            "provider": "cloudflare",
            "zone": "example.com",
            "record": "dev",
            "ttl": 300,
            "proxied": false,
            "use_proxy": false,
            "cloudflare": {
                "api_token": "${CLOUDFLARE_API_TOKEN}",
                "zone_id": "${CLOUDFLARE_ZONE_ID:-}"
            }
        },
        {
            "provider": "aliyun",
            "zone": "example.cn",
            "record": "www",
            "ttl": 600,
            "use_proxy": false,
            "aliyun": {
                "access_key_id": "${ALIYUN_ACCESS_KEY_ID}",
                "access_key_secret": "${ALIYUN_ACCESS_KEY_SECRET}"
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
├── LICENSE
├── README.md
└── src/
    ├── main.cpp
    ├── log.hpp / log.cpp
    ├── config.hpp / config.cpp
    ├── cache.hpp / cache.cpp
    ├── ip_getter.hpp / ip_getter.cpp
    ├── ip_getter_freebsd.cpp  # FreeBSD/OpenBSD ioctl 实现
    └── provider/
        ├── provider.hpp
        ├── cloudflare.hpp / cloudflare.cpp
        └── aliyun.hpp / aliyun.cpp
```

## 平台支持

| 平台 | IPv6 获取方式 | 状态 |
|------|--------------|------|
| Linux | netlink (`RTM_GETADDR`) | ✅ 完整支持 |
| FreeBSD | ioctl (`SIOCGIFALIFETIME_IN6`) | ✅ 支持 |
| OpenBSD | ioctl (`getifaddrs`) | ✅ 支持 |
| macOS | - | ⚠️ 暂无支持 |

所有平台均支持 HTTP API 降级方式获取 IPv6 地址。

## 许可证

采用 **MIT License** - 详见 [LICENSE](LICENSE) 文件。
