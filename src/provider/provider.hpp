#pragma once
#include <map>
#include <string>

namespace provider {

/// Abstract DNS provider.
class DNSProvider {
public:
    virtual ~DNSProvider() = default;

    virtual std::string name() const = 0;

    /// Create or update a DNS AAAA record.
    /// extra: provider-specific key-value pairs (e.g. "proxied" → "true")
    /// Returns true on success.
    virtual bool upsert_record(const std::string& zone,
                               const std::string& record_name,
                               const std::string& ip,
                               int                ttl,
                               const std::map<std::string, std::string>& extra) = 0;
};

} // namespace provider
