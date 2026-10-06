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
/* chttp_default_client() creates a process-wide singleton on its first use, */
/* with ccol_call_once. It then hands out the raw handle of that singleton.  */
/* Its own doc comment invites the caller to pass that handle to a           */
/* chttpclient_set_* function, and nothing stops the caller from passing it  */
/* to chttpclient_destroy too. Such a call must not leave the static         */
/* default_client_bundler.client pointer dangling. The destroy macro sets    */
/* only the local variable of the caller to NULL.                            */
/*                                                                            */
/* A dangling singleton pointer does two things. Every later call to         */
/* chttp_default_client(), chttp_do() or chttp_get() in the process then     */
/* uses that pointer after the free. ccol_call_once never fires again, so    */
/* nothing rebuilds it. And the __attribute__((destructor)) cleanup of this  */
/* file destroys it a second time at the exit of the process. That happens   */
/* whether or not anything used it in between.                               */
/*                                                                            */
/* __chttpclient_destroy therefore clears default_client_bundler.client. It  */
/* uses a defensive compare-and-swap whenever the handle that it gets is the */
/* singleton. The header documents that nothing ever rebuilds a default      */
/* client that somebody destroyed. Every convenience function then fails     */
/* cleanly, and none of them crashes.                                        */
/*                                                                            */
/* To destroy default_client_bundler.client is a one-shot action. It is      */
/* process-global, and nothing can undo it for the life of the process. The  */
/* convention of this project therefore puts it into a binary of its own,    */
/* inside the same test directory. tests_tls.c and tests_mem_mgmt.c follow   */
/* the same convention. Inside tests.c it would permanently break the use of */
/* chttp_do and chttp_get in every other test, for the rest of that process. */
/* ========================================================================== */

TEST(default_client, destroying_it_directly_does_not_crash_or_double_free) {
  chttpcli cli = chttp_default_client();
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  /* This is the misuse that this test exists to make safe. The caller
     destroys the handle that chttp_default_client() owns itself. That
     function is responsible for the teardown of it at the exit of the
     process. */
  chttpclient_destroy(cli);
  REQUIRE_EQ(cli, CHTTPCLI_INVALID); /* the macro clears the local, as it
                                        always does */

  /* The destroy must clear the singleton. Without that, this call gives
   * back the same handle, which is now stale, and ccol_call_once never
   * fires again. Any use of it below is then a use after the destroy. With
   * the singleton cleared, this call always gives back CHTTPCLI_INVALID,
   * and it does so for good. Nothing can rebuild the default client after
   * a destroy of this kind. See the doc comment of
   * chttp_default_client. */
  chttpcli cli2 = chttp_default_client();
  REQUIRE_EQ(cli2, CHTTPCLI_INVALID);

  /* Every convenience function that sits on the default client must fail
   * cleanly. None of them may crash, and none may dereference a dangling
   * pointer. Nothing ever tries a real request here. chttp_do stops on a
   * default client of NULL, before any network I/O. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://127.0.0.1:1/x", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_unexpected_failure);
  REQUIRE_EQ((void *)resp, NULL);
  chttp_request_free(req);

  /* The __attribute__((destructor)) cleanup runs at the exit of this test
   * process. It must find default_client_bundler.client already NULL,
   * because the code above cleared it. It must then do nothing. It must not
   * destroy the same freed memory a second time. A clean valgrind run of
   * this binary under `make memtest` is what checks that. No hook inside a
   * test can watch the destructor directly. */
}
