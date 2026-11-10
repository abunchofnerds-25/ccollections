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

// A standalone helper for differential testing, kept out of make test and make
// memtest; only tests/cyaml/differential/compare_pyyaml.py runs it. It reads a
// YAML document or stream from stdin and prints the DOM as JSON, so the value
// level can be compared with PyYAML (tests_spec_suite.c covers accept and
// reject). Exit codes: 0 after a parse and dump, 1 on a parse error (message on
// stderr, nothing on stdout), 2 on an environment error (out of memory or a
// failed write).
#include <cyaml.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Prints s as a JSON string with the escapes JSON needs, for scalar values and
// keys.
static void json_print_escaped_string(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"':
        fputs("\\\"", stdout);
        break;
      case '\\':
        fputs("\\\\", stdout);
        break;
      case '\n':
        fputs("\\n", stdout);
        break;
      case '\r':
        fputs("\\r", stdout);
        break;
      case '\t':
        fputs("\\t", stdout);
        break;
      default:
        if (*p < 0x20) {
          printf("\\u%04x", *p);
        } else {
          putchar((int)*p);
        }
    }
  }
  putchar('"');
}

// Prints node and everything below it as JSON. NaN and the infinities have no
// JSON literal, so they go out as the marker strings "__cyaml_nan__",
// "__cyaml_inf__" and "__cyaml_neg_inf__", which no real string can equal and
// which compare_pyyaml.py treats specially.
static void dump_node(cyaml node) {
  switch (cyaml_type(node)) {
    case CYAML_NULL:
      fputs("null", stdout);
      break;
    case CYAML_BOOL:
      fputs(cyaml_bool_val(node) ? "true" : "false", stdout);
      break;
    case CYAML_INTEGER:
      printf("%lld", cyaml_int_val(node));
      break;
    case CYAML_FLOAT: {
      double d = cyaml_double_val(node);
      if (isnan(d)) {
        fputs("\"__cyaml_nan__\"", stdout);
      } else if (isinf(d)) {
        fputs(d > 0 ? "\"__cyaml_inf__\"" : "\"__cyaml_neg_inf__\"", stdout);
      } else {
        printf("%.17g", d);
      }
      break;
    }
    case CYAML_STRING:
      json_print_escaped_string(cyaml_str_val(node));
      break;
    case CYAML_LIST: {
      putchar('[');
      size_t n = cyaml_list_len(node);
      for (size_t i = 0; i < n; i++) {
        if (i) putchar(',');
        dump_node(cyaml_list_get(node, i));
      }
      putchar(']');
      break;
    }
    case CYAML_DICTIONARY: {
      putchar('{');
      bool first = true;
      cyaml_dictionary_iter it;
      for (bool more = cyaml_dictionary_first(node, &it); more;
           more = cyaml_dictionary_next(&it)) {
        if (!first) putchar(',');
        first = false;
        json_print_escaped_string(it.key);
        putchar(':');
        dump_node(it.value);
      }
      putchar('}');
      break;
    }
  }
}

// Reads all of stdin, parses it and prints the tree. A final fflush() and
// ferror() check catches any failed write (a full disk, say), which is what
// gives exit code 2 for bad output.
int main(void) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fprintf(stderr, "cyaml_to_json: out of memory\n");
    return 2;
  }

  size_t n;
  while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
    len += n;
    if (len == cap) {
      cap *= 2;
      char *grown = realloc(buf, cap);
      if (!grown) {
        free(buf);
        fprintf(stderr, "cyaml_to_json: out of memory\n");
        return 2;
      }
      buf = grown;
    }
  }

  char *err = NULL;
  cyaml root = cyaml_parse_n(buf, len, &err);
  free(buf);

  if (!root) {
    fprintf(stderr, "cyaml_to_json: parse error: %s\n",
            err ? err : "(unknown)");
    return 1;
  }

  dump_node(root);
  putchar('\n');
  cyaml_destroy(root);

  if (fflush(stdout) != 0 || ferror(stdout)) {
    fprintf(stderr, "cyaml_to_json: error writing output\n");
    return 2;
  }
  return 0;
}
