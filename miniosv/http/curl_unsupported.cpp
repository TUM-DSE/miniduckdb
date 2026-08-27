/*
 * The three HTTPFSCurlUtil methods that live in httpfs_curl_client.cpp,
 * defined here to refuse.
 *
 * httpfs offers several HTTP backends and lets a query pick between them with
 * `SET httpfs_client_implementation`. curl and httplib both want a socket
 * layer that does not exist here, so the only working one is
 * mininet_client.cpp -- but src/httpfs_extension.cpp constructs an
 * HTTPFSCurlUtil in a settings callback, which needs the class to link whether
 * or not anything calls it.
 *
 * Only these three: the rest of HTTPFSCurlUtil is in
 * src/httpfs_connection_caching.cpp, which does compile here because the
 * connection pool is transport-agnostic. Defining those again would be a
 * duplicate symbol, which is how this file was first written and what the
 * linker had to say about it.
 *
 * Same shape as upstream's own httpfs_client_wasm.cpp, which defines
 * InitializeClient to throw for a build where no client is reachable.
 */

#include "httpfs_client.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

unique_ptr<HTTPClient> HTTPFSCurlUtil::InitializeClient(HTTPParams &, const string &) {
	throw NotImplementedException(
	    "httpfs: the curl backend is not available on miniOSv. There is no socket layer for it to "
	    "use; requests go through modules/mininet instead, which is the default.");
}

unordered_map<string, string> HTTPFSCurlUtil::ParseGetParameters(const string &text) {
	return HTTPFSUtil::ParseGetParameters(text);
}

string HTTPFSCurlUtil::GetName() const {
	return "HTTPFS-Curl-unavailable";
}

} // namespace duckdb
