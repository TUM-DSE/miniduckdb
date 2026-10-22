/*
 * Just enough of <openssl/rand.h> for httpfs's crypto.hpp to parse.
 *
 * s3fs.cpp includes crypto.hpp for the SigV4 hashing next door to it, and
 * crypto.hpp includes this. The AES code that actually wants OpenSSL is only
 * reachable through OVERRIDE_ENCRYPTION_UTILS, which this build leaves
 * undefined -- so EncryptionUtil stays core's mbedtls implementation, already
 * compiled in, and crypto.cpp is never built.
 *
 * A declaration, not a definition: linking something that silently returns
 * bad randomness would be far worse than failing to link at all.
 */

#ifndef MINIOSV_OPENSSL_RAND_STUB_H
#define MINIOSV_OPENSSL_RAND_STUB_H

#ifdef __cplusplus
extern "C" {
#endif

int RAND_bytes(unsigned char *buf, int num);

#ifdef __cplusplus
}
#endif

#endif /* MINIOSV_OPENSSL_RAND_STUB_H */
