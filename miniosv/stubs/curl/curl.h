/*
 * Just enough of <curl/curl.h> for httpfs's headers to parse.
 *
 * src/include/httpfs_curl_client.hpp names these types in inline members, and
 * src/httpfs_extension.cpp includes it unconditionally -- so the header has to
 * resolve even though no curl backend exists here. Nothing declared below is
 * ever called: miniosv/http/curl_unsupported.cpp defines the one class that
 * would have used them, and every method of it throws.
 *
 * Declarations only, deliberately. A definition would make it possible to link
 * something that then does nothing at run time, which is exactly the failure
 * mode a stub should not have.
 */

#ifndef MINIOSV_CURL_STUB_H
#define MINIOSV_CURL_STUB_H

typedef void CURL;
typedef int CURLcode;

struct curl_slist {
	char *data;
	struct curl_slist *next;
};

#ifdef __cplusplus
extern "C" {
#endif

CURLcode curl_easy_perform(CURL *curl);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data);
void curl_slist_free_all(struct curl_slist *list);

#ifdef __cplusplus
}
#endif

#endif /* MINIOSV_CURL_STUB_H */
