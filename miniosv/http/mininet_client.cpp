/*
 * duckdb::HTTPClient, implemented against modules/mininet.
 *
 * This *replaces* httpfs's src/httpfs_httplib_client.cpp, which is excluded
 * from the build (see miniosv/sources.mk). Nothing in the extension is
 * modified to make that work: upstream already chooses its HTTP client at
 * build time -- httplib normally, a stub under Emscripten -- and both of those
 * files define HTTPFSUtil::InitializeClient. This is a third choice, selected
 * the same way, so the whole of httpfs above the client boundary compiles
 * unmodified.
 *
 * mininet carries HTTP but does not build it, so this renders the request head
 * itself. That is the right split: the range arithmetic, the header policy and
 * (later) SigV4 all belong to httpfs, which already does them.
 *
 * Two limits, both deliberate and both loud rather than silent:
 *
 *   - One host per image. A worker's usable source ports are derived from the
 *     peer's address, so mininet is brought up for one endpoint; a request for
 *     any other host is refused here rather than fetched from the wrong one.
 *   - A response body has to fit a buffer sized in advance, because that is
 *     what mininet::get takes. Every read httpfs makes on the hot path is
 *     ranged, so the size comes from the Range header this function just
 *     wrote. An unranged GET -- `SET force_download=true` -- has no such size
 *     and is refused.
 */

#include "httpfs_client.hpp"
#include "http_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include "modules/mininet/mininet.hh"

namespace duckdb {

namespace {

//! "https://bucket.s3.eu-north-1.amazonaws.com" -> "bucket.s3...amazonaws.com".
//! mininet dials an address it was given at startup; the name is only ever the
//! Host header and the TLS server name.
string HostOf(const string &proto_host_port) {
	string s = proto_host_port;
	auto scheme = s.find("://");
	if (scheme != string::npos) {
		s = s.substr(scheme + 3);
	}
	auto slash = s.find('/');
	if (slash != string::npos) {
		s = s.substr(0, slash);
	}
	auto colon = s.find(':');
	if (colon != string::npos) {
		s = s.substr(0, colon);
	}
	return s;
}

//! Bytes a `Range: bytes=A-B` header asks for, or 0 if there is no such
//! header. httpfs writes one for every read on the hot path.
idx_t RangeLength(const HTTPHeaders &headers) {
	if (!headers.HasHeader("Range")) {
		return 0;
	}
	const string v = headers.GetHeaderValue("Range");
	auto eq = v.find('=');
	auto dash = v.find('-', eq == string::npos ? 0 : eq + 1);
	if (eq == string::npos || dash == string::npos) {
		return 0;
	}
	try {
		auto first = std::stoull(v.substr(eq + 1, dash - eq - 1));
		auto last = std::stoull(v.substr(dash + 1));
		if (last < first) {
			return 0;
		}
		return static_cast<idx_t>(last - first + 1);
	} catch (...) {
		return 0;
	}
}

//! Request line, headers, blank line. No explicit `Connection:` header --
//! HTTP/1.1 defaults to keep-alive, and mininet reuses the socket for the
//! next request on this slot as long as the peer leaves it Established.
string RenderHead(const char *method, const string &path, const string &host, const HTTPHeaders &headers,
                  const HTTPParams &params) {
	string out = string(method) + " " + path + " HTTP/1.1\r\n";
	bool have_host = false;
	for (auto &h : headers) {
		if (StringUtil::CIEquals(h.first, "Host")) {
			have_host = true;
		}
		out += h.first + ": " + h.second + "\r\n";
	}
	if (!have_host) {
		out += "Host: " + host + "\r\n";
	}
	for (auto &h : params.extra_headers) {
		out += h.first + ": " + h.second + "\r\n";
	}
	out += "\r\n";
	return out;
}

unique_ptr<HTTPResponse> Failed(int rc, const string &what) {
	auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
	result->request_error = what + ": " + mininet::strerror(rc);
	result->success = false;
	return result;
}

//! Turn what mininet reports back into the response httpfs expects. Only the
//! headers something above actually reads are reconstructed -- httpfs asks by
//! name, and asking for one that was never sent is already handled.
unique_ptr<HTTPResponse> ToResponse(const mininet::response &r) {
	auto result = make_uniq<HTTPResponse>(HTTPUtil::ToStatusCode(static_cast<int32_t>(r.status)));
	if (r.content_length > 0 || r.status == 204) {
		result->headers.Insert("Content-Length", to_string(r.content_length));
	}
	if (r.has_range) {
		string v = "bytes " + to_string(r.range_first) + "-" + to_string(r.range_last) + "/";
		v += r.range_total ? to_string(r.range_total) : string("*");
		result->headers.Insert("Content-Range", v);
	}
	if (r.etag[0] != '\0') {
		result->headers.Insert("ETag", string(r.etag));
	}
	if (r.last_modified[0] != '\0') {
		result->headers.Insert("Last-Modified", string(r.last_modified));
	}
	return result;
}

class MininetHTTPClient : public HTTPClient {
public:
	explicit MininetHTTPClient(const string &proto_host_port)
	    : HTTPClient(proto_host_port), host(HostOf(proto_host_port)) {
	}

	void Initialize(HTTPParams &) override {
	}

	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override {
		if (auto refusal = CheckHost()) {
			return refusal;
		}
		const idx_t want = RangeLength(info.headers);
		if (want == 0) {
			// No Range, so nothing tells us how large the body will be, and
			// mininet needs the buffer up front. force_download is the setting
			// that gets here; say so rather than return an empty body.
			auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			result->request_error =
			    "mininet: unranged GET is not supported (a body needs a size before it arrives); "
			    "leave force_download off";
			result->success = false;
			return result;
		}

		const string head = RenderHead("GET", info.path, host, info.headers, info.params);
		string body;
		body.resize(want);

		mininet::response r {};
		int rc = mininet::get(head.c_str(), head.size(), &body[0], body.size(), &r);
		if (rc != mininet::OK) {
			return Failed(rc, "mininet: GET " + info.path);
		}

		auto response = ToResponse(r);
		if (static_cast<int>(response->status) >= 400) {
			// An error body is small and is the useful part; hand it over as
			// the body rather than through the content handler.
			response->body = body.substr(0, static_cast<size_t>(r.bytes));
			if (info.response_handler) {
				info.response_handler(*response);
			}
			return response;
		}
		if (info.response_handler && !info.response_handler(*response)) {
			return response;
		}
		if (info.content_handler && r.bytes > 0) {
			info.content_handler(const_data_ptr_cast(body.data()), static_cast<idx_t>(r.bytes));
		} else {
			response->body = body.substr(0, static_cast<size_t>(r.bytes));
		}
		return response;
	}

	unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) override {
		if (auto refusal = CheckHost()) {
			return refusal;
		}
		const string head = RenderHead("HEAD", info.path, host, info.headers, info.params);
		mininet::response r {};
		// A HEAD has no body, so there is nowhere for one to go.
		int rc = mininet::get(head.c_str(), head.size(), nullptr, 0, &r);
		if (rc != mininet::OK) {
			return Failed(rc, "mininet: HEAD " + info.path);
		}
		return ToResponse(r);
	}

	unique_ptr<HTTPResponse> Put(PutRequestInfo &info) override {
		return NotImplemented("PUT");
	}
	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) override {
		return NotImplemented("DELETE");
	}
	unique_ptr<HTTPResponse> Post(PostRequestInfo &info) override {
		return NotImplemented("POST");
	}

private:
	//! mininet serves one endpoint. Fetching from the wrong host would look
	//! like data corruption much later, so refuse here.
	unique_ptr<HTTPResponse> CheckHost() const {
		const char *up = mininet::host();
		if (!up) {
			auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			result->request_error = "mininet is not up";
			result->success = false;
			return result;
		}
		if (host != up) {
			auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			result->request_error = StringUtil::Format(
			    "mininet serves \"%s\" and cannot reach \"%s\": one endpoint per image", up, host);
			result->success = false;
			return result;
		}
		return nullptr;
	}

	static unique_ptr<HTTPResponse> NotImplemented(const char *method) {
		auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
		result->request_error = string("mininet: ") + method + " is not implemented";
		result->success = false;
		return result;
	}

	string host;
};

} // namespace

//! Also normally httplib's, which parses the query string with its own
//! helper. s3fs uses it to pull the parameters out of a pre-signed URL, so it
//! has to work rather than return nothing -- the WASM backend stubs it out and
//! silently loses every parameter.
//!
//! `a=b&c=d`, percent-decoded, with `+` meaning space as in a form body.
unordered_map<string, string> HTTPFSUtil::ParseGetParameters(const string &text) {
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	};
	auto decode = [&hex](const string &in) {
		string out;
		out.reserve(in.size());
		for (idx_t i = 0; i < in.size(); i++) {
			int hi = i + 2 < in.size() ? hex(in[i + 1]) : -1;
			int lo = i + 2 < in.size() ? hex(in[i + 2]) : -1;
			if (in[i] == '+') {
				out += ' ';
			} else if (in[i] == '%' && hi >= 0 && lo >= 0) {
				out += static_cast<char>(hi * 16 + lo);
				i += 2;
			} else {
				out += in[i];
			}
		}
		return out;
	};

	unordered_map<string, string> result;
	idx_t pos = 0;
	while (pos < text.size()) {
		auto amp = text.find('&', pos);
		const string field = text.substr(pos, amp == string::npos ? string::npos : amp - pos);
		if (!field.empty()) {
			auto eq = field.find('=');
			if (eq == string::npos) {
				result.emplace(decode(field), string());
			} else {
				result.emplace(decode(field.substr(0, eq)), decode(field.substr(eq + 1)));
			}
		}
		if (amp == string::npos) {
			break;
		}
		pos = amp + 1;
	}
	return result;
}

//! The symbol httpfs_httplib_client.cpp would have defined. Selecting a client
//! by which file is compiled is upstream's own mechanism; this is a third
//! choice alongside httplib and the WASM stub.
unique_ptr<HTTPClient> HTTPFSUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	return make_uniq<MininetHTTPClient>(proto_host_port);
}

} // namespace duckdb
