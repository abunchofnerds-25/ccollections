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
 * libFuzzer harness for the dot-separated path grammar of cyaml. That grammar
 * is what _cyaml_get(), cyaml_set() and _cyaml_delete() read. It is the
 * counterpart of fuzz_cjson_path.c, which drives the identical grammar in
 * cjson, and it exists for the same reason: the path is a second parser in
 * its own right, it walks a DOM that the first parser built, and it MUTATES
 * that DOM. A harness that only parses a document never reaches it.
 *
 * cyaml reaches shapes that cjson cannot. An alias makes one subtree
 * reachable from two places, a merge key ("<<:") splices one mapping into
 * another, and a document can carry tags that change the type of a node. A
 * path walk over any of those is worth driving directly.
 *
 * The target splits the fuzz input on the first NUL byte. It parses
 * everything before that byte as YAML, with the bounded entry point, so the
 * document half draws on the same seed material as the corpus of fuzz_cyaml.
 * It uses everything after that byte as the path string, byte for byte. It
 * skips an input with no NUL byte, and an input with an empty path. Such an
 * input adds nothing beyond what fuzz_cyaml already covers.
 */

#include <cyaml.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const uint8_t *nul = memchr(data, 0, size);
  if (!nul) return 0;

  size_t yaml_len = (size_t)(nul - data);
  const uint8_t *path_bytes = nul + 1;
  size_t path_len = size - yaml_len - 1;
  if (path_len == 0) return 0;

  char *path = malloc(path_len + 1);
  if (!path) return 0;
  memcpy(path, path_bytes, path_len);
  path[path_len] = '\0';

  char *err = NULL;
  cyaml root = cyaml_parse_n_mp((const char *)data, yaml_len, &err, NULL);
  if (!root) {
    free(path);
    return 0;
  }

  cyaml got = cyaml_get(root, path);
  (void)got;

  /* Drive every C type that cyaml_set() accepts, and not only one of them.
   * The last byte of the fuzz input selects the type. That choice is
   * deterministic, so a saved crash takes the identical branch on a
   * replay. */
  switch (path_bytes[path_len - 1] % 5) {
    case 0:
      cyaml_set(root, path, (long long)42);
      break;
    case 1:
      cyaml_set(root, path, true);
      break;
    case 2:
      cyaml_set(root, path, 3.5);
      break;
    case 3:
      cyaml_set(root, path, "fuzz");
      break;
    default:
      cyaml_set(root, path, (void *)NULL);
      break;
  }

  cyaml_delete(root, path);

  /* The document has been mutated in place. Serializing it exercises the
   * emitter over a shape that no parse alone produces, and a re-parse would
   * be the round-trip check of fuzz_cyaml applied to that shape. */
  char *text = cyaml_serialize(root);
  cyaml_serialize_free(text);

  free(path);
  cyaml_destroy(root);
  return 0;
}
