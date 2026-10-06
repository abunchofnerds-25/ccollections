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
 * A libFuzzer target for the path navigation grammar of cjson. That grammar
 * is the dot-separated component syntax: plain keys, '#N' list indices, and
 * the '\.' and '\\' escapes. The "Path syntax" section of cjson.h describes
 * it. _cjson_get(), cjson_set() and _cjson_delete() read it. They do so
 * through navigate(), path_find_unescaped_dot(),
 * path_find_last_unescaped_dot(), path_unescape_component() and
 * parse_list_index_component() in cjson.c. This is a second hand-written
 * parser in this module, beside the JSON grammar itself, and the two are
 * independent. fuzz_cjson_parse.c already covers the JSON grammar on its own.
 * This target therefore parses one JSON document one time. It then fuzzes the
 * PATH string, and navigates, changes and deletes against that document again
 * and again.
 *
 * The target splits the fuzz input on the first NUL byte. It parses
 * everything before that byte as JSON. That half draws on the same seed
 * material as the corpus of fuzz_cjson_parse, so a mutation can explore
 * malformed JSON and malformed paths together. The target uses everything
 * after that byte as the path string, byte for byte. It skips an input with
 * no NUL byte, and an input with an empty path. Such an input adds nothing
 * beyond what fuzz_cjson_parse already covers.
 */

#include <cjson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const uint8_t *nul = memchr(data, 0, size);
  if (!nul) return 0;

  size_t json_len = (size_t)(nul - data);
  const uint8_t *path_bytes = nul + 1;
  size_t path_len = size - json_len - 1;
  if (path_len == 0) return 0;

  char *path = malloc(path_len + 1);
  if (!path) return 0;
  memcpy(path, path_bytes, path_len);
  path[path_len] = '\0';

  char *err = NULL;
  cjson root = cjson_parse_n_mp((const char *)data, json_len, &err, NULL);
  if (!root) {
    free(path);
    return 0;
  }

  cjson got = cjson_get(root, path);
  (void)got;

  /* Drive every C type that cjson_set() accepts, and not only one of them.
   * The last byte of the fuzz input selects the type. That choice is
   * deterministic, so a saved crash takes the identical branch on a
   * replay. */
  switch (path_bytes[path_len - 1] % 5) {
    case 0:
      cjson_set(root, path, (long long)42);
      break;
    case 1:
      cjson_set(root, path, true);
      break;
    case 2:
      cjson_set(root, path, 3.5);
      break;
    case 3:
      cjson_set(root, path, "fuzz");
      break;
    default:
      cjson_set(root, path, (void *)NULL);
      break;
  }

  cjson_delete(root, path);

  free(path);
  cjson_destroy(root);
  return 0;
}
