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

// A libFuzzer target for the path grammar of cjson (dot-separated keys, #N list
// indices, the \. and \\ escapes) that cjson_get(), cjson_set() and
// cjson_remove() read. The input is split on its first NUL byte: The part
// before it is parsed as JSON (it shares seed material with fuzz_cjson_parse),
// and the part after it is the path, byte for byte. An input with no NUL or an
// empty path adds nothing and is skipped.
#include <cjson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The entry point libFuzzer calls with each input: Parses the JSON half, then
// gets, sets and deletes the path half on it. The last byte of the input picks
// the C type that cjson_set() receives, so a saved crash replays the same
// branch.
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

  cjson_remove(root, path);

  free(path);
  cjson_destroy(root);
  return 0;
}
