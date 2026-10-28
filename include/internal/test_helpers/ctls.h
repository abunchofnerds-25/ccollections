/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

/**
 * @file test_helpers/ctls.h
 * @brief INTERNAL ONLY. The test-only hooks of ctls.
 *
 * src/ctls.c defines these hooks and the tests call them. They read the
 * OpenSSL objects behind a connection or a context, count references and
 * parses, and drive the record tracker directly.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <internal/ctls.h>
#include <openssl/ssl.h>

#ifdef RUNNING_UNIT_TESTS
/* Whether the process still holds the root key of the self-signed
 * certificates. */
bool _ctls_root_key_held_for_tests(void);

/* Counts the lookups of the connection ex_data index that take its mutex. */
long _ctls_ex_idx_locked_calls_for_tests(void);

/* Feeds the given chunks in order to a fresh record tracker and gives what
 * ctls_conn_has_pending_input() would answer from the framing alone. */
bool _ctls_rec_track_pending_after_for_tests(const char *const *chunks,
                                             const int *lens, int count);

/* How many SSL_CTX references this module holds for live connections, so a
 * test can check that the total comes back to its starting value once every
 * connection is destroyed. */
long _ctls_live_conn_ctx_pin_count_for_tests(void);

/* How many socket BIOs of this module are alive, so a test can check that
 * every connection gives its BIO back. */
size_t _ctls_live_sock_bio_count_for_tests(void);

/* How many times the process parsed the default CA file, and a call that
 * drops the parsed file so the next context that trusts the system store
 * reads it again, for example after a test changed SSL_CERT_FILE. */
long _ctls_system_ca_parses_for_tests(void);
void _ctls_system_ca_forget_for_tests(void);

/* How many certificates the trust store of ctx's default context holds
 * right now, without the ones a hashed directory would load during a
 * handshake, or -1 when ctx has no built context yet. */
int _ctls_trusted_cert_count_for_tests(ctls_ctx_t *ctx);

/* The raw SSL object of a connection, so a test can inspect the peer
 * certificate that the two sides agreed on. */
SSL *_ctls_conn_ssl_for_tests(ctls_conn_t *conn);

/* The SSL_CTX that a servername dispatch put on this connection, which this
 * module holds a reference on, or NULL when the connection never
 * dispatched. */
SSL_CTX *_ctls_conn_pinned_sni_ctx_for_tests(ctls_conn_t *conn);

/* The SSL_CTX that serves a ClientHello naming server_name, or the default
 * context for NULL or a name that nothing matches. */
SSL_CTX *_ctls_ctx_served_ctx_for_tests(ctls_ctx_t *tls,
                                        const char *server_name);
#endif /* RUNNING_UNIT_TESTS */
