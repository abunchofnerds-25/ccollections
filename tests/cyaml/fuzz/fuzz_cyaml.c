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

// libFuzzer harness for cyaml_parse_n(), the three serializers and
// cyaml_clone(). It parses through the length-taking entry point, so the parser
// reads the raw buffer as it is, NUL bytes included; a token that a NUL would
// cut short is only reachable this way. Then it checks the round trip: The
// output of each serializer must parse again, a stream of two or more documents
// must come back as the same documents, and a clone must serialize to exactly
// the source's text. Every serializer and the clone must succeed on a tree the
// parser accepted (they share its nesting limit, and libFuzzer reports OOM on
// its own). The error message of a refused input must be printable ASCII. Each
// violation aborts, which libFuzzer records as a finding.
#include <cyaml.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Aborts unless text, which a serializer named by style just wrote, is non-NULL
// and parses again. The input was already accepted, so a failure here is always
// a library defect.
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

// Aborts when a parse error message holds a byte outside printable ASCII,
// which the library promises never to write.
static void assert_printable_error(const char *err) {
  if (!err) return;
  for (const unsigned char *p = (const unsigned char *)err; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) {
      fprintf(stderr, "cyaml error message holds byte 0x%02X\n", *p);
      abort();
    }
  }
}

// Runs one input through the parse, every serializer and the clone with the
// checks described at the top. The stream check skips a tagged list, which only
// a single document gives, since a stream has no node to carry that tag.
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

  cyaml copy = cyaml_clone(doc);
  if (!copy) {
    fprintf(stderr, "cyaml clone is NULL for a tree that parsed\n");
    abort();
  }
  {
    char *copied = cyaml_serialize(copy);
    assert_reparses("clone block-style", copied);
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
