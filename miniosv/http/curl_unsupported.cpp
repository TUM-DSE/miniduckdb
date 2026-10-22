/*
 * The HTTPFSCurlUtil members that live in httpfs_curl_client.cpp, which is not
 * compiled: curl wants a socket layer that does not exist here.
 *
 * httpfs installs HTTPFSCurlUtil as every database's HTTP backend when it loads
 * (HTTPSettings::Initialize), so the class has to work, not just link. Its
 * clients are the mininet one, as HTTPFSUtil's are (mininet_client.cpp); the
 * rest is what the base class does.
 */

#include "http/httpfs_client.hpp"

namespace duckdb {

HTTPFSCurlUtil::HTTPFSCurlUtil(bool connection_caching_enabled_p, optional_ptr<DatabaseInstance> db)
    : HTTPFSUtil(db), connection_caching_enabled(connection_caching_enabled_p) {
}

unique_ptr<HTTPClient> HTTPFSCurlUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	return HTTPFSUtil::InitializeClient(http_params, proto_host_port);
}

HTTPTransportReusePolicy HTTPFSCurlUtil::GetTransportReusePolicy() const {
	return HTTPFSUtil::GetTransportReusePolicy();
}

string HTTPFSCurlUtil::GetName() const {
	// httpfs_connection_caching recognises its backend by this name.
	return "HTTPFS-Curl";
}

bool HTTPFSCurlUtil::GetDefaultVerifySSL(const HTTPFSParams &params) const {
	// mininet verifies the server itself, against its own roots.
	return params.enable_server_cert_verification;
}

} // namespace duckdb
