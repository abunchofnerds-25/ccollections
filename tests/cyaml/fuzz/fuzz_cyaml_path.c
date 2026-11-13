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

// libFuzzer harness for the path grammar that cyaml__get(), cyaml_set() and
// cyaml__remove() read, the counterpart of fuzz_cjson_path.c. The path walker
// is a second parser that mutates the DOM the first one built, and aliases,
// merge keys and tags give it shapes a plain parse never exercises. The input
// splits at its first NUL: The bytes before it are parsed as YAML and the bytes
// after it are the path. An input with no NUL or an empty path is skipped,
// since fuzz_cyaml covers it already.
#include <cyaml.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Runs one input: Parses the document, then gets, sets and deletes the path,
// and serializes the mutated tree. The last byte of the input picks which C
// type cyaml_set() writes, so every accepted type is driven and a saved crash
// replays the same branch.
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

  cyaml_remove(root, path);

  char *text = cyaml_serialize(root);
  cyaml_serialize_free(text);

  free(path);
  cyaml_destroy(root);
  return 0;
}
