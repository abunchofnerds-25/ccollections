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
#include <tau/tau.h>

TAU_MAIN()

/* ========================================================================== */
/*   DESTROYING chttp_default_client()'S HANDLE (dedicated binary)            */
/*                                                                            */
/* chttp_default_client() builds a process-wide singleton on first use, with  */
/* _ccol_call_once, and hands out its raw handle. Its own doc comment tells   */
/* the caller to pass that handle to a chttpclient_set_* function, and        */
/* nothing stops the caller from passing it to chttpclient_destroy as well.   */
/* Such a call must not leave the static default_client_bundler.client        */
/* pointer dangling, because the destroy macro only sets the caller's local   */
/* variable to NULL.                                                          */
/*                                                                            */
/* A dangling singleton pointer hurts twice. Every later                      */
/* chttp_default_client(), chttp_do() or chttp_get() call in the process      */
/* uses it after the free, since _ccol_call_once never fires again and        */
/* nothing rebuilds it. And the __attribute__((destructor)) cleanup of this   */
/* file destroys it a second time at process exit, whether anything used it   */
/* in between or not.                                                         */
/*                                                                            */
/* So __chttpclient_destroy clears default_client_bundler.client, with a      */
/* defensive compare-and-swap, whenever the handle it gets is the singleton.  */
/* The header documents that a default client somebody destroyed is never     */
/* rebuilt; from then on every convenience function fails cleanly and none    */
/* of them crashes.                                                           */
/*                                                                            */
/* Destroying default_client_bundler.client is a one-shot thing for the       */
/* whole process and can't be undone, so following the project's convention   */
/* it gets a binary of its own in the same test directory, like tests_tls.c   */
/* and tests_mem_mgmt.c. Inside tests.c it would break chttp_do and           */
/* chttp_get for every other test for the rest of that process.               */
/* ========================================================================== */

TEST(default_client, destroying_it_directly_does_not_crash_or_double_free) {
  chttpcli cli = chttp_default_client();
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  /* The misuse this test makes safe: the caller destroys the handle that
     chttp_default_client() owns, although that function is the one in
     charge of tearing it down at process exit. */
  chttpclient_destroy(cli);
  REQUIRE_EQ(cli, CHTTPCLI_NULL); /* the macro clears the local, as it
                                        always does */

  /* The destroy has to clear the singleton. If it doesn't, this call gives back
   * the same stale handle, _ccol_call_once never fires again and every use of
   * it below is a use after the destroy. Once the singleton is cleared this
   * call gives back CHTTPCLI_NULL for good, because nothing can rebuild the
   * default client after such a destroy (see the doc comment of
   * chttp_default_client). */
  chttpcli cli2 = chttp_default_client();
  REQUIRE_EQ(cli2, CHTTPCLI_NULL);

  /* Every convenience function on top of the default client must fail cleanly,
   * with no crash and no dereference of a dangling pointer. No real request is
   * ever attempted here since chttp_do stops on a NULL default client before
   * doing any network I/O. */
  chttp_request_t *req =
      chttp_request_create(CHTTP_GET, "http://127.0.0.1:1/x", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_unexpected_failure);
  REQUIRE_EQ((void *)resp, NULL);
  chttp_request_free(req);

  /* The __attribute__((destructor)) cleanup runs when this test process exits.
   * It must find default_client_bundler.client already NULL, since the code
   * above cleared it, and then do nothing instead of destroying the same freed
   * memory again. No hook inside a test can watch the destructor, so what
   * checks this is a clean valgrind run of the binary under `make memtest`. */
}
