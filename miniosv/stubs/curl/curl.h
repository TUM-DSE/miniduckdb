/*
 * Just enough of <curl/curl.h> for httpfs's headers to parse.
 *
 * src/include/http/httpfs_curl_client.hpp names these types in inline members,
 * and src/http/http_settings.cpp includes it unconditionally -- so the header
 * has to resolve even though no curl backend exists here. Nothing declared
 * below is ever called: miniosv/http/curl_unsupported.cpp defines the one class
 * that would have used them, without curl.
 *
 * Declarations only, deliberately. A definition would make it possible to link
 * something that then does nothing at run time, which is exactly the failure
 * mode a stub should not have.
 */

#ifndef MINIOSV_CURL_STUB_H
#define MINIOSV_CURL_STUB_H

typedef void CURL;
typedef struct Curl_URL CURLU;
typedef int CURLcode;
typedef int CURLoption;

enum { CURLE_OK = 0 };

struct curl_slist {
	char *data;
	struct curl_slist *next;
};

#ifdef __cplusplus
extern "C" {
#endif

CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
const char *curl_easy_strerror(CURLcode code);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data);
void curl_slist_free_all(struct curl_slist *list);

#ifdef __cplusplus
}
#endif

#endif /* MINIOSV_CURL_STUB_H */
