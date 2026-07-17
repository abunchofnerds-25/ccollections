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

#include <chttpclient.h>
#include <common.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*   DESTROYING chttp_default_client()'S HANDLE (dedicated binary)            */
/*                                                                            */
/* Regression test for a real bug: chttp_default_client() lazily creates a   */
/* process-wide singleton via call_once and hands its raw handle out; its    */
/* own doc comment invites passing that handle to chttpclient_set_*, and     */
/* nothing stops a caller from also passing it to chttpclient_destroy. Doing */
/* so used to leave the static default_client_bundler.client pointer         */
/* dangling (only the caller's own local variable was NULLed by the destroy  */
/* macro): every later chttp_default_client()/chttp_do()/chttp_get() call in */
/* the process would then use-after-free that pointer directly (call_once   */
/* never re-fires, so nothing rebuilds it), and this file's own              */
/* __attribute__((destructor)) cleanup would double-destroy it a second time */
/* at process exit regardless of whether anything used it in between.       */
/*                                                                           */
/* Fixed with a defensive compare-and-swap in __chttpclient_destroy: it now  */
/* clears default_client_bundler.client if the handle passed to it happens  */
/* to be the singleton, and the header now documents that a destroyed       */
/* default client is never rebuilt (every convenience function fails        */
/* cleanly afterward instead of crashing).                                  */
/*                                                                           */
/* This is exactly the kind of one-shot, process-global, unrecoverable       */
/* action (destroying default_client_bundler.client is permanent for the    */
/* life of the process) that this project's own established convention      */
/* isolates into its own binary within the same test directory, mirroring   */
/* tests_tls.c/tests_mem_mgmt.c's precedent, rather than running inside     */
/* tests.c where it would permanently break every other test's use of       */
/* chttp_do/chttp_get for the rest of that process.                        */
/* ========================================================================== */

TEST(default_client, destroying_it_directly_does_not_crash_or_double_free) {
  chttpcli cli = chttp_default_client();
  REQUIRE_NE((void *)cli, NULL);

  /* The actual misuse this test exists to make safe: the caller destroys
   * the handle chttp_default_client() itself owns and is responsible for
   * tearing down at process exit. */
  chttpclient_destroy(cli);
  REQUIRE_EQ((void *)cli, NULL); /* the macro NULLs the local as usual */

  /* Before the fix: this would return the same, now-dangling pointer
   * (call_once never re-fires), and any use of it below would be a real
   * use-after-free. After the fix: the singleton was cleared, so this
   * consistently and permanently returns NULL instead -- there is no way
   * to rebuild the default client once it has been destroyed this way (see
   * chttp_default_client's own doc comment). */
  chttpcli cli2 = chttp_default_client();
  REQUIRE_EQ((void *)cli2, NULL);

  /* Every convenience function built on the default client must fail
   * cleanly (no crash) rather than dereference the dangling pointer the
   * bug used to leave behind. No real request is ever attempted: chttp_do
   * short-circuits on a NULL default client before any network I/O. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://127.0.0.1:1/x", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_unexpected_failure);
  REQUIRE_EQ((void *)resp, NULL);
  chttp_request_free(req);

  /* The process-exit __attribute__((destructor)) cleanup runs after this
   * test process exits; it must find default_client_bundler.client already
   * NULL (cleared above) and be a no-op, not a second destroy of the same
   * freed memory. That is exactly what a clean valgrind run under `make
   * memtest` for this binary verifies -- there is no in-test hook to
   * observe the destructor directly. */
}
