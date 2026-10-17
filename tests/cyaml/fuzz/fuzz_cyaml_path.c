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
 * libFuzzer harness for the dot-separated path grammar of cyaml, which is
 * what _cyaml_get(), cyaml_set() and _cyaml_delete() read. It's the
 * counterpart of fuzz_cjson_path.c, which drives the same grammar in cjson,
 * and it exists for the same reason. The path is a second parser of its own
 * that walks a DOM the first parser built, and it MUTATES that DOM, so a
 * harness that only parses documents never gets there.
 *
 * cyaml can build shapes that cjson can't. An alias makes one subtree
 * reachable from two places, a merge key ("<<:") splices one mapping into
 * another, and a document can carry tags that change a node's type. A path
 * walk over any of these is worth driving directly.
 *
 * The target splits the fuzz input on the first NUL byte. Everything before
 * that byte is parsed as YAML with the bounded entry point, so the document
 * half draws on the same seed material as the fuzz_cyaml corpus, and
 * everything after it is the path string, byte for byte. An input with no
 * NUL byte or with an empty path is skipped, since it adds nothing that
 * fuzz_cyaml doesn't already cover.
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

  /* Drive every C type that cyaml_set() accepts and not just one of them.
   * The last byte of the fuzz input picks the type, and since that choice
   * is deterministic a saved crash takes the same branch when it's
   * replayed. */
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

  /* The document was mutated in place. Serializing it runs the emitter over
   * a shape that no parse alone produces, and a re-parse would be the
   * round-trip check of fuzz_cyaml applied to that shape. */
  char *text = cyaml_serialize(root);
  cyaml_serialize_free(text);

  free(path);
  cyaml_destroy(root);
  return 0;
}
