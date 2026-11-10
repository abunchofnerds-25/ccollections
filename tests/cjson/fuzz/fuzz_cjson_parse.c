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

// A libFuzzer target for cjson_parse_n_mp() that also drives both serializers
// and cjson_clone() on whatever DOM the parser builds, and checks that every
// rendering parses back to the same document. fuzz_cjson_path.c covers the path
// grammar, which this target never reaches.
#include <cjson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Re-parses text that the library just emitted and aborts when it doesn't parse
// or doesn't give the same compact rendering (every member, key byte, value and
// member order) as the first parse, compact. style names the rendering in the
// report. A failure is always a library defect: the input was already accepted
// and the text is our own rendering of it. A NULL text means an allocation
// failed and is skipped.
static void assert_reparses(const char *style, const char *text,
                            const char *compact) {
  if (!text) return;
  char *err = NULL;
  cjson again = cjson_parse(text, &err);
  if (!again) {
    fprintf(stderr,
            "cjson round trip failed: %s output does not parse again: %s\n"
            "--- emitted ---\n%s\n--- end ---\n",
            style, err ? err : "(no error string)", text);
    abort();
  }
  char *again_compact = cjson_serialize(again);
  if (again_compact && compact && strcmp(again_compact, compact) != 0) {
    fprintf(stderr,
            "cjson round trip failed: %s output parses to a different "
            "document\n--- first ---\n%s\n--- again ---\n%s\n--- end ---\n",
            style, compact, again_compact);
    abort();
  }
  cjson_serialize_free(again_compact);
  cjson_destroy(again);
}

// The entry point libFuzzer calls with each input: parses it and round-trips
// the compact, pretty and cloned forms through assert_reparses().
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *err = NULL;
  cjson root = cjson_parse_n_mp((const char *)data, size, &err, NULL);
  if (!root) return 0;

  char *compact = cjson_serialize(root);
  assert_reparses("compact", compact, compact);

  char *pretty = cjson_serialize_pretty(root, 2);
  assert_reparses("pretty", pretty, compact);
  cjson_serialize_free(pretty);

  cjson copy = cjson_clone(root);
  if (copy) {
    char *copied = cjson_serialize(copy);
    assert_reparses("clone compact", copied, compact);
    cjson_serialize_free(copied);
    cjson_destroy(copy);
  }
  cjson_serialize_free(compact);

  cjson_destroy(root);
  return 0;
}
