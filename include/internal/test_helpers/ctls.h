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

// Test-only hooks of ctls, defined in src/ctls.c. They read the OpenSSL
// objects behind a connection or a context, count references and parses, and
// drive the record tracker. They exist only when RUNNING_UNIT_TESTS is
// defined, which the shipped library never does.

#include <internal/ctls.h>
#include <openssl/ssl.h>

#ifdef RUNNING_UNIT_TESTS
// Tells whether the process still holds the root key of the self-signed
// certificates, so the suite's destructor can fail a run that leaks it.
bool ctls__root_key_held_for_tests(void);

// Counts the lookups of the connection ex_data index that took its mutex, so a
// test can check that a registered index is read without the lock.
long ctls__ex_idx_locked_calls_for_tests(void);

// Feeds count chunks (with their lens) in order to a fresh record tracker and
// gives what ctls_conn_has_pending_input() would answer from the framing
// alone, so a test can drive record boundaries without a real connection.
bool ctls__rec_track_pending_after_for_tests(const char *const *chunks,
                                             const int *lens, int count);

// Gives the SSL_CTX references this module holds for live connections, so a
// test can check that it comes back to its start once every connection is
// destroyed.
long ctls__live_conn_ctx_pin_count_for_tests(void);

// Gives how many socket BIOs of this module are alive, so a test can check
// that every connection gives its BIO back.
size_t ctls__live_sock_bio_count_for_tests(void);

// Give how many times the default CA file was parsed, so a test can check it
// happens once per process, and drop the parsed file, so the next
// system-trust context reads it again (after a test changed SSL_CERT_FILE,
// for example).
long ctls__system_ca_parses_for_tests(void);
void ctls__system_ca_forget_for_tests(void);

// Gives the certificates in the trust store of ctx's default context (not
// counting what a hashed directory loads during a handshake), or -1 when ctx
// has no built context yet, so a test can check that a bundle went in whole.
int ctls__trusted_cert_count_for_tests(ctls_ctx *ctx);

// Gives the SSL object of conn, so a test can inspect the agreed peer
// certificate.
SSL *ctls__conn_ssl_for_tests(ctls_conn *conn);

// Gives the SSL_CTX a servername dispatch put on conn (this module holds a
// reference on it), or NULL when it never dispatched, so a test can check the
// pin.
SSL_CTX *ctls__conn_pinned_sni_ctx_for_tests(ctls_conn *conn);

// Gives the SSL_CTX that serves a ClientHello naming server_name (the default
// context for NULL or a name nothing matches), so a test can check the SNI
// matching rules without a handshake.
SSL_CTX *ctls__ctx_served_ctx_for_tests(ctls_ctx *tls, const char *server_name);
#endif
