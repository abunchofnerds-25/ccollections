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

// A caller may pass the handle of chttp_default_client() to chttpcli_destroy.
// The destroy must then clear the process-wide singleton, or every later
// convenience call uses it after the free and the exit-time destructor frees it
// a second time. A destroyed default client is never rebuilt, so the next
// chttp_default_client() gives CHTTPCLI_NULL and chttp_do fails cleanly without
// network I/O. The destructor at exit must then do nothing, which a clean
// memtest run checks. This can't be undone in a process, so it has a binary of
// its own.
TEST(default_client, destroying_it_directly_does_not_crash_or_double_free) {
  chttpcli cli = chttp_default_client();
  REQUIRE_NE(cli, CHTTPCLI_NULL);

  chttpcli_destroy(cli);
  REQUIRE_EQ(cli, CHTTPCLI_NULL);

  chttpcli cli2 = chttp_default_client();
  REQUIRE_EQ(cli2, CHTTPCLI_NULL);

  chttp_request *req =
      chttp_request_create(CHTTP_GET, "http://127.0.0.1:1/x", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_unexpected_failure);
  REQUIRE_EQ((void *)resp, NULL);
  chttp_request_free(req);
}
