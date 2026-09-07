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
 * libFuzzer target for cjson.c's JSON parser (cjson_parse_n_mp), the DOM's
 * two serializers, and cjson_clone(), all driven directly off whatever DOM
 * shape the parser produces from the fuzz input. A single run therefore also
 * exercises round-tripping an arbitrarily malformed-but-accepted document
 * back through cjson_serialize()/cjson_serialize_pretty(), and confirms
 * cjson_clone() never crashes on it either; not just the parser itself.
 *
 * See fuzz_cjson_path.c for the sibling target covering cjson's OTHER
 * hand-written parser: the dot-separated path-navigation grammar consumed by
 * cjson_get()/cjson_set()/cjson_delete(), which this target never reaches.
 */

#include <cjson.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *err = NULL;
  cjson root = cjson_parse_n_mp((const char *)data, size, &err, NULL);
  if (err) free(err);
  if (!root) return 0;

  char *compact = cjson_serialize(root);
  if (compact) cjson_serialize_free(compact);

  char *pretty = cjson_serialize_pretty(root, 2);
  if (pretty) cjson_serialize_free(pretty);

  cjson copy = cjson_clone(root);
  if (copy) cjson_destroy(copy);

  cjson_destroy(root);
  return 0;
}
