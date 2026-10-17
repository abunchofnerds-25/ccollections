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

/* libFuzzer harness for cyaml_parse_n, its three serializers and
 * cyaml_clone.
 *
 * We parse through the entry point that takes an explicit length rather
 * than cyaml_parse, so the parser reads the fuzzer's raw buffer exactly as
 * it is. There's no copy step that adds a NUL terminator and no truncation
 * at a NUL byte inside the buffer. This matters for more than coverage,
 * because a token that a NUL would cut short can only be reached through
 * this entry point, and the tag and anchor lexers refuse such a byte
 * precisely because they store a token as a plain NUL-terminated string.
 *
 * The harness then asserts the ROUND TRIP. Text that cyaml_serialize(),
 * cyaml_serialize_flow() or cyaml_serialize_stream() produced must parse
 * again, and a stream of two or more documents should parse back to the
 * same documents. A parse-only harness can't see a serializer that emits
 * something its own parser rejects, since nothing ever feeds the output
 * back in, and in production that failure is silent too: a caller stores
 * the serialized form, reads it back and gets a parse error for a document
 * the library wrote itself. The harness also asserts that the message of
 * every refused input is printable ASCII. abort() is the right answer in
 * all of these cases because that's what libFuzzer records as a finding.
 *
 * Every serializer and cyaml_clone() must also SUCCEED on a tree that the
 * parser just accepted. The parse, the clone and the serializers share one
 * nesting limit, so the only other reason for a NULL would be a failed
 * allocation. This harness runs on the default allocator though, and
 * libFuzzer reports running out of memory as a finding of its own
 * (-rss_limit_mb and -malloc_limit_mb) instead of handing NULL back. So a
 * NULL here means a tree that the parser accepted and the rest of the
 * library refuses. */

#include <cyaml.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Re-parses text that this library just emitted. A failure here is always a
 * defect in the library and never in the fuzz input, since the input was
 * already accepted and this text is our own rendering of that document. */
static void assert_reparses(const char *style, const char *text) {
  if (!text) {
    fprintf(stderr, "cyaml %s output is NULL for a tree that parsed\n", style);
    abort();
  }
  char *err = NULL;
  cyaml again = cyaml_parse(text, &err);
  if (!again) {
    fprintf(stderr,
            "cyaml round trip failed: %s output does not parse again: %s\n"
            "--- emitted ---\n%s\n--- end ---\n",
            style, err ? err : "(no error string)", text);
    abort();
  }
  cyaml_destroy(again);
}

/* Every parse error message is printable ASCII whatever the input holds, so
 * a failure here is a defect in the library. */
static void assert_printable_error(const char *err) {
  if (!err) return;
  for (const unsigned char *p = (const unsigned char *)err; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) {
      fprintf(stderr, "cyaml error message holds byte 0x%02X\n", *p);
      abort();
    }
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *err = NULL;
  cyaml doc = cyaml_parse_n((const char *)data, size, &err);
  if (!doc) {
    assert_printable_error(err);
    return 0;
  }

  char *block = cyaml_serialize(doc);
  assert_reparses("block-style", block);

  char *flow = cyaml_serialize_flow(doc);
  assert_reparses("flow-style", flow);

  /* A list of two or more documents written as a stream parses back to a
   * list of the same documents, so both render to the same flow text. We
   * leave out a tagged list, which only a single document can give, because
   * a stream has no node that could carry a tag of the list itself. */
  char *stream = cyaml_serialize_stream(doc);
  if (!stream) {
    fprintf(stderr, "cyaml stream output is NULL for a tree that parsed\n");
    abort();
  }
  if (stream[0] != '\0') assert_reparses("stream", stream);
  if (cyaml_type(doc) == CYAML_LIST && cyaml_list_len(doc) >= 2 &&
      !cyaml_node_tag(doc)) {
    cyaml again = cyaml_parse(stream, NULL);
    char *again_flow = again ? cyaml_serialize_flow(again) : NULL;
    if (again_flow && strcmp(again_flow, flow) != 0) {
      fprintf(stderr,
              "cyaml stream round trip changed the documents\n--- before "
              "---\n%s\n--- after ---\n%s\n--- stream ---\n%s\n--- end ---\n",
              flow, again_flow, stream);
      abort();
    }
    cyaml_serialize_free(again_flow);
    cyaml_destroy(again);
  }
  cyaml_serialize_free(stream);
  cyaml_serialize_free(flow);

  /* A clone has to serialize to something that parses as well. This drives
   * the clone walk with whatever shape the parser just accepted. */
  cyaml copy = cyaml_clone(doc);
  if (!copy) {
    fprintf(stderr, "cyaml clone is NULL for a tree that parsed\n");
    abort();
  }
  {
    char *copied = cyaml_serialize(copy);
    assert_reparses("clone block-style", copied);
    /* A clone keeps every value, every tag and the member order of every
     * dictionary, so its serialized text is exactly the same. */
    if (strcmp(block, copied) != 0) {
      fprintf(stderr,
              "cyaml clone differs from its source\n--- source ---\n%s\n"
              "--- clone ---\n%s\n--- end ---\n",
              block, copied);
      abort();
    }
    cyaml_serialize_free(copied);
    cyaml_destroy(copy);
  }
  cyaml_serialize_free(block);

  cyaml_destroy(doc);
  return 0;
}
