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

/*
 * A libFuzzer target for the JSON parser of cjson.c, cjson_parse_n_mp(),
 * that also covers the DOM's two serializers and cjson_clone(). It drives
 * all of them directly from whatever DOM shape the parser builds out of the
 * fuzz input, so a run does more than exercise the parser. A document that
 * is malformed but that the parser accepted goes back out through
 * cjson_serialize() and cjson_serialize_pretty(), and we also check that
 * cjson_clone() doesn't crash on such a document.
 *
 * fuzz_cjson_path.c is the sibling target. It covers the OTHER hand-written
 * parser of cjson, the dot-separated path navigation grammar that
 * cjson_get(), cjson_set() and cjson_delete() read, and this target never
 * gets there.
 */

#include <cjson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Re-parses text that this library just emitted. A failure here is a defect
 * in the library and never in the fuzz input, because the input was already
 * accepted and this text is our own rendering of that document. Calling the
 * serializer alone would only catch a serializer that CRASHES, and not one
 * that emits text which looks well formed but which the matching parser
 * then refuses. In production that disagreement is silent: a caller stores
 * the serialized form, reads it back and gets a parse error for a document
 * this library wrote. */
static void assert_reparses(const char *style, const char *text,
                            const char *compact) {
  if (!text) return; /* an allocation failed; that is not a round-trip fault */
  char *err = NULL;
  cjson again = cjson_parse(text, &err);
  if (!again) {
    fprintf(stderr,
            "cjson round trip failed: %s output does not parse again: %s\n"
            "--- emitted ---\n%s\n--- end ---\n",
            style, err ? err : "(no error string)", text);
    abort();
  }
  /* The text must also mean the same document, so we compare the compact
   * rendering of the re-parsed tree with the compact rendering of the first
   * one. Every member, every key byte, every value and the order of the
   * members needs to survive. A parser that repairs or merges what it reads,
   * or a serializer that changes a value, fails here even though the
   * re-parse above succeeds. */
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
