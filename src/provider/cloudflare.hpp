#pragma once
#include "provider.hpp"
#include <string>

namespace provider {

class CloudflareProvider : public DNSProvider {
public:
    CloudflareProvider(std::string api_token, std::string proxy_url = "");

    std::string name() const override { return "cloudflare"; }

    /// Returns zone_id. If zone_id_hint is non-empty it is returned as-is;
    /// otherwise the Cloudflare API is queried.
    std::string get_zone_id(const std::string& zone_name,
                             const std::string& zone_id_hint,
                             std::string&       error_out);

    bool upsert_record(const std::string& zone,
                       const std::string& record_name,
                       const std::string& ip,
                       int                ttl,
                       const std::map<std::string, std::string>& extra) override;

    /// Overload that accepts a pre-resolved zone_id (avoids extra API call).
    bool upsert_record_with_zone_id(const std::string& zone,
                                    const std::string& record_name,
                                    const std::string& ip,
                                    const std::string& zone_id,
                                    int                ttl,
                                    bool               proxied);

private:
    std::string api_token_;
    std::string proxy_url_;

    struct HttpResponse { long code; std::string body; };
    HttpResponse cf_request(const std::string& method,
                             const std::string& url,
                             const std::string& body_json = "");
};

} // namespace provider
