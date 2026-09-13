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
 * Standalone differential-testing helper, not part of the public library
 * surface and deliberately kept out of `make test`/`make memtest` (see
 * tests/cyaml/differential/compare_pyyaml.py, this file's only caller).
 *
 * Reads a YAML document (or stream) from stdin and dumps the resulting DOM
 * as JSON on stdout, so an independent YAML implementation's own output
 * (PyYAML, via compare_pyyaml.py) can be diffed against it at the value
 * level rather than merely the accept/reject level tests_spec_suite.c
 * already covers.
 *
 * Exit codes: 0 on successful parse + dump; 1 on a genuine parse error
 * (nothing is written to stdout in that case; the message goes to stderr
 * instead); 2 on a usage/environment error (out of memory, bad output).
 */

#include <cyaml.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* RUNNING_UNIT_TESTS-gated debug accessor, implemented in src/cyaml.c.
 * Deliberately not declared in the public cyaml.h header; this matches
 * this project's own established white-box-test-accessor convention (see
 * e.g. cvector_get_capacity, cbmap_debug_validate_avl), just with the
 * `extern` declaration living in this standalone tool instead of a
 * tests.c file, since this program is not itself a tau test suite. */
extern const char *cyaml_debug_dictionary_key_at(cyaml map, size_t index);

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
      /* JSON has no NaN/Infinity literal. Encoding these as ordinary
       * strings would make them indistinguishable from a genuine
       * CYAML_STRING node on the comparison side, silently hiding a real
       * type mismatch; encode as a value no legitimate parsed string
       * could ever equal instead, so compare_pyyaml.py can special-case
       * them explicitly. */
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
      size_t i = 0;
      const char *key;
      bool first = true;
      while ((key = cyaml_debug_dictionary_key_at(node, i)) != NULL) {
        if (!first) putchar(',');
        first = false;
        json_print_escaped_string(key);
        putchar(':');
        dump_node(cyaml_dictionary_get(node, key));
        i++;
      }
      putchar('}');
      break;
    }
  }
}

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
    free(err);
    return 1;
  }

  dump_node(root);
  putchar('\n');
  cyaml_destroy(root);

  /* dump_node/putchar above write through buffered stdio (putchar/fputs/
   * printf), none of whose individual return values this function checks;
   * a single fflush()+ferror() check here catches any failure among all of
   * them (a full disk when stdout is redirected to a file, most plausibly),
   * matching this file's own documented "2 on a usage/environment error
   * (out of memory, bad output)" exit code contract, which nothing else in
   * this function currently implements. */
  if (fflush(stdout) != 0 || ferror(stdout)) {
    fprintf(stderr, "cyaml_to_json: error writing output\n");
    return 2;
  }
  return 0;
}
