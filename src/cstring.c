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

// cstring_rfind() uses memrchr(), which is a GNU extension.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstring.h>
#include <internal/cascii.h>
#include <internal/cpow2.h>
#include <internal/cprocsintern.h>
#include <internal/test_helpers/cstring.h>

// Finds the last byte c in the n bytes at s, or gives NULL. cstring_rfind()
// needs memrchr(), which glibc and FreeBSD have and macOS doesn't, so there
// this backward scan takes its place. CCOL_EMULATE_DARWIN_MEMRCHR selects it on
// other systems too, for the test suites.
#if defined(__APPLE__) || defined(CCOL_EMULATE_DARWIN_MEMRCHR)
static const void *cstring_memrchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  const unsigned char b = (unsigned char)c;
  while (p != (const unsigned char *)s) {
    if (*--p == b) return p;
  }
  return NULL;
}
#else
// Elsewhere cstring_memrchr is the C library's memrchr().
#define cstring_memrchr memrchr
#endif

// The smallest buffer a string ever has; creation and reset start from it.
static const size_t cstring_minimum_capacity = 16;

// The string. data holds length bytes and a '\0' in capacity bytes. m_procs is
// NULL for the default allocator, an interned procs pointer, or a pointer to
// the copy right after this struct in the same block.
struct cstring {
  char *data;
  size_t length;
  size_t capacity;
  ccol_memmgmt_procs *m_procs;
};

// The procs copy after the struct must be aligned for its type.
_Static_assert(sizeof(struct cstring) % _Alignof(ccol_memmgmt_procs) == 0,
               "the procs copy after struct cstring must be aligned");

// Frees the buffer and the string's block; cstring_destroy() and the scoped
// cleanup call it. NULL is fine. The free function is read before the block
// goes away, because m_procs can point into that block.
void cstring__destroy(cstr s) {
  if (s) {
    ccol__mem_free(s->m_procs, s->data);

    if (s->m_procs) {
      ccol_free free_func = s->m_procs->free;
      free_func(s);
    } else {
      ccol__default_mem_free(s);
    }
  }
}

// Says whether length + 1 fits in a size_t. Every function that sizes a buffer
// as length + 1 checks this first: A length of SIZE_MAX would wrap to a
// capacity of 0, which cstring_grow_to() takes as already enough, and the copy
// would then run past the real buffer.
static bool cstring_length_fits_with_terminator(size_t length) {
  return length != ccol_invalid_size;
}

// Grows the buffer of s to at least needed_capacity bytes, rounded up to a
// power of two (16 at least), so a run of appends reallocates rarely. Every
// function that makes the string longer calls it. Gives false, with s
// unchanged, when the size can't be represented or the realloc fails. Updates
// s->data and s->capacity.
static bool cstring_grow_to(cstr s, size_t needed_capacity) {
  if (needed_capacity <= s->capacity) {
    return true;
  }

  size_t new_cap = ccol__find_nearest_gte_power_of_two(needed_capacity);
  if (new_cap == ccol_invalid_size) {
    return false;
  }
  if (new_cap < cstring_minimum_capacity) {
    new_cap = cstring_minimum_capacity;
  }

  char *orig = s->data;
  s->data = (char *)ccol__mem_realloc(s->m_procs, s->data, new_cap);
  if (!s->data) {
    s->data = orig;
    return false;
  }

  s->capacity = new_cap;
  return true;
}

// Destroys every cstr in v and then v, so a split that fails halfway can undo
// what it built and give NULL instead of a partial result.
static void destroy_cstr_vector(cvec v) {
  if (!v) {
    return;
  }
  size_t count = cvector_elem_count(v);
  for (size_t i = 0; i < count; i++) {
    cstr *part = (cstr *)cvector_at(v, i);
    if (part && *part) {
      cstring__destroy(*part);
    }
  }
  cvector__destroy(v);
}

// Creates a string holding the init_len bytes at initial (NULL is fine when
// init_len is 0; the bytes hold no '\0'). Creation, substring and split all
// build strings here, so the checks and the allocation logic live in one place.
// The buffer gets its final size at once: The power of two above init_len + 1,
// 16 at least. An interned procs pointer is kept as it is; any other procs
// struct is copied into the tail of the string's block, so a string costs two
// allocations and never points at the caller's struct. Gives NULL with *err set
// on failure, after freeing what it allocated.
static cstr cstring_create_from_span(const char *initial, size_t init_len,
                                     ccol_memmgmt_procs *m_procs, char **err) {
  if (!ccol__verify_memmgmt_procs(m_procs, err)) {
    return NULL;
  }

  bool copy_procs = m_procs && !ccol_procs_is_interned(m_procs);
  cstr s = (cstr)ccol__mem_calloc(
      m_procs, 1,
      sizeof(cstring) + (copy_procs ? sizeof(ccol_memmgmt_procs) : 0));
  if (!s) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate cstring container");
    }
    return NULL;
  }

  if (copy_procs) {
    s->m_procs = (ccol_memmgmt_procs *)(s + 1);
    memcpy(s->m_procs, m_procs, sizeof(ccol_memmgmt_procs));
  } else {
    s->m_procs = m_procs;
  }

  if (!cstring_length_fits_with_terminator(init_len)) {
    cstring__destroy(s);
    if (err) {
      *err = CCOL__ERR_STR("initial string too large");
    }
    return NULL;
  }
  size_t init_cap = ccol__find_nearest_gte_power_of_two(init_len + 1);
  if (init_cap == ccol_invalid_size) {
    cstring__destroy(s);
    if (err) {
      *err = CCOL__ERR_STR("initial string too large");
    }
    return NULL;
  }
  if (init_cap < cstring_minimum_capacity) {
    init_cap = cstring_minimum_capacity;
  }

  s->data = (char *)ccol__mem_alloc(s->m_procs, init_cap);
  if (!s->data) {
    cstring__destroy(s);
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate cstring data buffer");
    }
    return NULL;
  }

  if (init_len > 0) {
    memcpy(s->data, initial, init_len);
  }
  s->data[init_len] = '\0';
  s->length = init_len;
  s->capacity = init_cap;

  if (err) {
    *err = NULL;
  }
  return s;
}

// Creates a string holding a copy of the C string initial (NULL for empty),
// allocating through m_procs. It's the public constructor behind every create
// function and cstr_construct form. Gives NULL with *err set on failure.
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs *m_procs,
                         char **err) {
  return cstring_create_from_span(initial, initial ? strlen(initial) : 0,
                                  m_procs, err);
}

// Gives the allocator of s (NULL for the default), so a caller can allocate
// related memory the same way. Asserts on a NULL s.
ccol_memmgmt_procs *cstring_get_mprocs(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  return s->m_procs;
}

// Gives the length of s without the '\0', kept up to date by every operation.
// Asserts on a NULL s.
size_t cstring_length(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  return s->length;
}

// Gives the NUL-terminated buffer of s, for APIs that take a char *. Any change
// to s can make it invalid. Asserts on a NULL s.
const char *cstring_c_str(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  return s->data;
}

// Gives the byte at idx, or '\0' out of range, so a bad index isn't fatal.
// Asserts on a NULL s.
char cstring_at(cstr s, size_t idx) {
  if (!s) {
    ccol__assert(false);
  }
  if (idx >= s->length) {
    return '\0';
  }
  return s->data[idx];
}

// Says whether s holds no bytes. Asserts on a NULL s.
bool cstring_is_empty(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  return s->length == 0;
}

// Appends str to s, growing the buffer when needed; the most common way to
// build a string. Gives ccol_success (also for an empty str), ccol_invalid_args
// for a NULL str, ccol_container_full on a length overflow or
// ccol_not_enough_memory. str may point into s; its offset is saved before the
// buffer can move. The function is 64-byte aligned on purpose: The short block
// that a non-growing append jumps to must sit in one 64-byte line, and at an
// offset of 32 or 48 bytes every append costs about 45 percent more. After a
// change here or a compiler change, check the block with objdump.
__attribute__((aligned(64))) ccol_retval cstring_append(cstr s,
                                                        const char *str) {
  if (!s) {
    ccol__assert(false);
  }
  if (!str) {
    return ccol_invalid_args;
  }

  size_t str_len = strlen(str);
  if (str_len == 0) {
    return ccol_success;
  }

  size_t new_len = s->length + str_len;
  if (new_len < s->length || !cstring_length_fits_with_terminator(new_len)) {
    return ccol_container_full;
  }

  ptrdiff_t alias_off =
      (str >= s->data && str < s->data + s->capacity) ? (str - s->data) : -1;

  if (!cstring_grow_to(s, new_len + 1)) {
    return ccol_not_enough_memory;
  }

  if (alias_off >= 0) {
    str = s->data + alias_off;
  }

  memcpy(s->data + s->length, str, str_len);
  s->length = new_len;
  s->data[s->length] = '\0';

  return ccol_success;
}

// Puts str in front of the content of s. The buffer grows first, then the old
// content moves right with memmove. str may point into s; its offset is saved
// before the buffer can move and moved again by the shift. Results as for
// cstring_append().
ccol_retval cstring_prepend(cstr s, const char *str) {
  if (!s) {
    ccol__assert(false);
  }
  if (!str) {
    return ccol_invalid_args;
  }

  size_t str_len = strlen(str);
  if (str_len == 0) {
    return ccol_success;
  }

  size_t new_len = s->length + str_len;
  if (new_len < s->length || !cstring_length_fits_with_terminator(new_len)) {
    return ccol_container_full;
  }

  ptrdiff_t alias_off =
      (str >= s->data && str < s->data + s->capacity) ? (str - s->data) : -1;

  if (!cstring_grow_to(s, new_len + 1)) {
    return ccol_not_enough_memory;
  }

  if (alias_off >= 0) {
    str = s->data + alias_off;
  }

  memmove(s->data + str_len, s->data, s->length + 1);

  if (alias_off > 0) {
    str = s->data + alias_off + str_len;
  }

  memmove(s->data, str, str_len);
  s->length = new_len;

  return ccol_success;
}

// Inserts str at byte pos (pos equal to the length appends). The buffer grows
// before the memmove. str may point into s: Its offset is saved before the
// buffer can move, shifted when it lies after pos, and the final copy is a
// memmove because source and destination can overlap. Results as for
// cstring_append(), plus ccol_invalid_args for pos past the end.
ccol_retval cstring_insert(cstr s, size_t pos, const char *str) {
  if (!s) {
    ccol__assert(false);
  }
  if (!str || pos > s->length) {
    return ccol_invalid_args;
  }

  size_t str_len = strlen(str);
  if (str_len == 0) {
    return ccol_success;
  }

  size_t new_len = s->length + str_len;
  if (new_len < s->length || !cstring_length_fits_with_terminator(new_len)) {
    return ccol_container_full;
  }

  ptrdiff_t alias_off =
      (str >= s->data && str < s->data + s->capacity) ? (str - s->data) : -1;

  if (!cstring_grow_to(s, new_len + 1)) {
    return ccol_not_enough_memory;
  }

  if (alias_off >= 0) {
    str = s->data + alias_off;
  }

  memmove(s->data + pos + str_len, s->data + pos, s->length - pos + 1);

  if (alias_off >= 0 && (size_t)alias_off > pos) {
    str = s->data + alias_off + str_len;
  }

  memmove(s->data + pos, str, str_len);
  s->length = new_len;

  return ccol_success;
}

// Replaces the content of s with str, '\0' included. str may point into s, so
// its offset is saved before a realloc and the copy is a memmove. Results as
// for cstring_append().
ccol_retval cstring_set(cstr s, const char *str) {
  if (!s) {
    ccol__assert(false);
  }
  if (!str) {
    return ccol_invalid_args;
  }

  size_t str_len = strlen(str);
  if (!cstring_length_fits_with_terminator(str_len)) {
    return ccol_container_full;
  }

  ptrdiff_t alias_off =
      (str >= s->data && str < s->data + s->capacity) ? (str - s->data) : -1;

  if (!cstring_grow_to(s, str_len + 1)) {
    return ccol_not_enough_memory;
  }

  if (alias_off >= 0) {
    str = s->data + alias_off;
  }

  memmove(s->data, str, str_len + 1);
  s->length = str_len;

  return ccol_success;
}

// Empties s and shrinks its buffer back to the minimum when it's larger; a
// failed realloc keeps the old capacity. A string already at the minimum skips
// the realloc. Asserts on a NULL s.
void cstring_reset(cstr s) {
  if (!s) {
    ccol__assert(false);
  }

  if (s->capacity != cstring_minimum_capacity) {
    char *orig = s->data;
    s->data = (char *)ccol__mem_realloc(s->m_procs, s->data,
                                        cstring_minimum_capacity);
    if (!s->data) {
      s->data = orig;
    } else {
      s->capacity = cstring_minimum_capacity;
    }
  }
  s->length = 0;
  s->data[0] = '\0';
}

// Makes the buffer of s hold at least min_capacity bytes, so later appends
// don't reallocate; a smaller request becomes the minimum. Gives the result of
// cstring_grow_to(). Asserts on a NULL s.
bool cstring_reserve(cstr s, size_t min_capacity) {
  if (!s) {
    ccol__assert(false);
  }

  size_t needed = min_capacity < cstring_minimum_capacity
                      ? cstring_minimum_capacity
                      : min_capacity;
  return cstring_grow_to(s, needed);
}

// Turns the ASCII lower-case letters of s into upper case in place. toupper()
// follows LC_CTYPE, so under a Turkish or single-byte locale it would change
// other bytes, a piece of UTF-8 among them; the ASCII fold never does. Asserts
// on a NULL s.
void cstring_to_upper(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)ccol_ascii_toupper((unsigned char)s->data[i]);
  }
}

// Turns the ASCII upper-case letters of s into lower case in place, in every
// locale, for the reason given at cstring_to_upper(). Asserts on a NULL s.
void cstring_to_lower(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)ccol_ascii_tolower((unsigned char)s->data[i]);
  }
}

// Removes the six ASCII white-space bytes at both ends of s in place, whatever
// the locale, so a UTF-8 byte is never cut. The end goes first because it only
// moves the '\0'; the start then needs one memmove. Asserts on a NULL s.
void cstring_trim(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  if (s->length == 0) {
    return;
  }

  while (s->length > 0 &&
         ccol_ascii_isspace((unsigned char)s->data[s->length - 1])) {
    s->length--;
  }
  s->data[s->length] = '\0';

  size_t start = 0;
  while (start < s->length &&
         ccol_ascii_isspace((unsigned char)s->data[start])) {
    start++;
  }
  if (start > 0) {
    memmove(s->data, s->data + start, s->length - start + 1);
    s->length -= start;
  }
}

// Computes the length of a string of orig_length bytes after count (more than
// 0) non-overlapping nlen-byte needles become rlen-byte replacements, for
// cstring_replace(). Gives ccol_success with the length in *new_len_out, or
// ccol_container_full when the product or the sum overflows or the result
// leaves no room for the '\0'. A shrinking replace can't underflow, because
// count * nlen is at most orig_length. It's a function of its own so the tests
// can reach the overflow guards without strings of several gigabytes.
static ccol_retval compute_replace_new_length(size_t orig_length, size_t nlen,
                                              size_t rlen, size_t count,
                                              size_t *new_len_out) {
  ccol__assert(count > 0);

  if (rlen >= nlen) {
    size_t added = (rlen - nlen) * count;
    if (added / count != (rlen - nlen)) {
      return ccol_container_full;
    }
    size_t new_len = orig_length + added;
    if (new_len < orig_length ||
        !cstring_length_fits_with_terminator(new_len)) {
      return ccol_container_full;
    }
    *new_len_out = new_len;
  } else {
    *new_len_out = orig_length - (nlen - rlen) * count;
  }

  return ccol_success;
}

// Replaces every non-overlapping needle in s with replacement. It counts the
// matches first, so the new buffer is allocated once at its exact size, builds
// the result there and swaps it in. Results as documented in cstring.h; s is
// unchanged on failure. Asserts on a NULL s.
ccol_retval cstring_replace(cstr s, const char *needle,
                            const char *replacement) {
  if (!s) {
    ccol__assert(false);
  }
  if (!needle || needle[0] == '\0' || !replacement) {
    return ccol_invalid_args;
  }

  size_t nlen = strlen(needle);
  size_t rlen = strlen(replacement);

  size_t count = 0;
  const char *ptr = s->data;
  while ((ptr = strstr(ptr, needle)) != NULL) {
    count++;
    ptr += nlen;
  }

  if (count == 0) {
    return ccol_success;
  }

  size_t new_len;
  ccol_retval len_rv =
      compute_replace_new_length(s->length, nlen, rlen, count, &new_len);
  if (len_rv != ccol_success) {
    return len_rv;
  }

  size_t new_cap = ccol__find_nearest_gte_power_of_two(new_len + 1);
  if (new_cap == ccol_invalid_size) {
    return ccol_container_full;
  }
  if (new_cap < cstring_minimum_capacity) {
    new_cap = cstring_minimum_capacity;
  }

  char *new_buf = (char *)ccol__mem_alloc(s->m_procs, new_cap);
  if (!new_buf) {
    return ccol_not_enough_memory;
  }

  const char *src = s->data;
  char *dst = new_buf;
  const char *found;
  while ((found = strstr(src, needle)) != NULL) {
    size_t before = (size_t)(found - src);
    memcpy(dst, src, before);
    dst += before;
    memcpy(dst, replacement, rlen);
    dst += rlen;
    src = found + nlen;
  }
  size_t tail = strlen(src);
  memcpy(dst, src, tail + 1);

  ccol__mem_free(s->m_procs, s->data);
  s->data = new_buf;
  s->length = new_len;
  s->capacity = new_cap;

  return ccol_success;
}

// Compares s with str as strcmp() does; a NULL str gives 1, since a value is
// greater than no value. Asserts on a NULL s.
int cstring_compare(cstr s, const char *str) {
  if (!s) {
    ccol__assert(false);
  }
  if (!str) {
    return 1;
  }
  return strcmp(s->data, str);
}

// Says whether s holds exactly the bytes of str; a NULL str is not equal.
// Asserts on a NULL s.
bool cstring_equals(cstr s, const char *str) {
  if (!s) {
    ccol__assert(false);
  }
  if (!str) {
    return false;
  }
  return strcmp(s->data, str) == 0;
}

// Says whether s starts with prefix; a NULL prefix gives false and a prefix
// longer than s is refused before any compare. Asserts on a NULL s.
bool cstring_starts_with(cstr s, const char *prefix) {
  if (!s) {
    ccol__assert(false);
  }
  if (!prefix) {
    return false;
  }
  size_t plen = strlen(prefix);
  if (plen > s->length) {
    return false;
  }
  return strncmp(s->data, prefix, plen) == 0;
}

// Says whether s ends with suffix, comparing the tail in place; a NULL suffix
// gives false. Asserts on a NULL s.
bool cstring_ends_with(cstr s, const char *suffix) {
  if (!s) {
    ccol__assert(false);
  }
  if (!suffix) {
    return false;
  }
  size_t slen = strlen(suffix);
  if (slen > s->length) {
    return false;
  }
  return strncmp(s->data + s->length - slen, suffix, slen) == 0;
}

// Gives the offset of the first needle in s with strstr(), or ccol_invalid_size
// when it's missing or needle is NULL. Asserts on a NULL s.
size_t cstring_find(cstr s, const char *needle) {
  if (!s) {
    ccol__assert(false);
  }
  if (!needle) {
    return ccol_invalid_size;
  }
  const char *found = strstr(s->data, needle);
  if (!found) {
    return ccol_invalid_size;
  }
  return (size_t)(found - s->data);
}

#ifdef RUNNING_UNIT_TESTS
// Counts the calls of cstring_rfind_linear(), so a test can prove that its
// inputs reach that path with the table on the stack and on the heap.
unsigned long cstring_rfind_linear_count_for_tests = 0;
#endif

// The longest needle whose failure table cstring_rfind_linear() keeps on the
// stack; a longer one takes its table from the string's allocator.
#define CSTRING_RFIND_STACK_TABLE_LEN 128

// Gives the start of the rightmost needle that lies entirely inside hay[0,
// limit), or ccol_invalid_size. The backward scan hands over to it when
// candidates keep failing, because it bounds the search at O(limit + nlen) for
// any text: It's Knuth-Morris-Pratt run right to left on the reversed needle.
// The failure table takes one size_t per needle byte, from m_procs when it
// doesn't fit on the stack; when that allocation fails it sets *table_ok to
// false and searches nothing. Bumps the test counter.
static __attribute__((noinline)) size_t
cstring_rfind_linear(const char *hay, size_t limit, const char *needle,
                     size_t nlen, ccol_memmgmt_procs *m_procs, bool *table_ok) {
#ifdef RUNNING_UNIT_TESTS
  __atomic_fetch_add(&cstring_rfind_linear_count_for_tests, 1,
                     __ATOMIC_RELAXED);
#endif
  size_t stack_table[CSTRING_RFIND_STACK_TABLE_LEN];
  size_t *fail = stack_table;
  if (nlen > CSTRING_RFIND_STACK_TABLE_LEN) {
    fail = nlen <= SIZE_MAX / sizeof(size_t)
               ? (size_t *)ccol__mem_alloc(m_procs, nlen * sizeof(size_t))
               : NULL;
    if (!fail) {
      *table_ok = false;
      return ccol_invalid_size;
    }
  }
  *table_ok = true;

  const size_t tail = nlen - 1;
  fail[0] = 0;
  size_t k = 0;
  for (size_t q = 1; q < nlen; q++) {
    while (k > 0 && needle[tail - q] != needle[tail - k]) {
      k = fail[k - 1];
    }
    if (needle[tail - q] == needle[tail - k]) {
      k++;
    }
    fail[q] = k;
  }

  size_t found = ccol_invalid_size;
  size_t j = 0;
  for (size_t i = limit; i-- > 0;) {
    char c = hay[i];
    while (j > 0 && c != needle[tail - j]) {
      j = fail[j - 1];
    }
    if (c == needle[tail - j] && ++j == nlen) {
      found = i;
      break;
    }
  }

  if (fail != stack_table) {
    ccol__mem_free(m_procs, fail);
  }
  return found;
}

// The state of one backward scan of cstring_rfind(). charged counts the bytes
// spent on candidates that failed, and budget is cleared once the hand-over to
// cstring_rfind_linear() is known to be impossible.
typedef struct {
  const char *hay;
  size_t length;
  const char *needle;
  size_t nlen;
  size_t charged;
  bool budget;
  ccol_memmgmt_procs *m_procs;
} cstring_rfind_scan;

// Tests the candidate at st, whose first and last bytes already match, for the
// backward scan. Gives true with *result set once the answer is settled: st
// matches, or the charge has passed four times the bytes scanned and
// cstring_rfind_linear() answered for everything below st. Gives false when the
// scan must go on. A failed table allocation turns the budget off, and the scan
// finishes alone, which is always correct.
static inline bool cstring_rfind_candidate(cstring_rfind_scan *sc, size_t st,
                                           size_t *result) {
  if (memcmp(sc->hay + st + 1, sc->needle + 1, sc->nlen - 2) == 0) {
    *result = st;
    return true;
  }
  sc->charged += sc->nlen;
  if (sc->budget && sc->charged > 4 * (sc->length - st)) {
    bool table_ok;
    size_t r = cstring_rfind_linear(sc->hay, st + sc->nlen - 1, sc->needle,
                                    sc->nlen, sc->m_procs, &table_ok);
    if (table_ok) {
      *result = r;
      return true;
    }
    sc->budget = false;
  }
  return false;
}

// 16 bytes the compiler keeps in one vector register where the target has one.
typedef unsigned char cstring_bytes16 __attribute__((vector_size(16)));

// Tests the candidates in [b, b + 16) whose lanes are set in hit, from the
// highest down, for the backward scan. Gives true with *result set when the
// answer is settled.
static inline bool cstring_rfind_block(cstring_rfind_scan *sc, size_t b,
                                       cstring_bytes16 hit, size_t *result) {
  unsigned char lanes[16];
  memcpy(lanes, &hit, sizeof(lanes));
  for (size_t k = 16; k-- > 0;) {
    if (lanes[k] && cstring_rfind_candidate(sc, b + k, result)) {
      return true;
    }
  }
  return false;
}

// Says whether any lane of v is set, so the scan can skip a block in one test.
static inline bool cstring_bytes16_any(cstring_bytes16 v) {
  uint64_t halves[2];
  memcpy(halves, &v, sizeof(halves));
  return (halves[0] | halves[1]) != 0;
}

// Below this length cstring_rfind() searches forward with strstr(); the
// backward scan only pays off on longer strings.
#define CSTRING_RFIND_SHORT 256

// The backward search of cstring_rfind() for a string of at least
// CSTRING_RFIND_SHORT bytes. It's out of line so the short path never pays for
// its frame and vector setup. A one-byte needle is one memrchr(). Otherwise a
// filter compares the needle's first byte against 16 start positions and its
// last byte against the positions nlen - 1 further on, 64 positions per test,
// and only a position that passes both gets a full compare. Gives the offset of
// the last needle, the length for an empty needle, or ccol_invalid_size.
static __attribute__((noinline)) size_t
cstring_rfind_backward(cstr s, const char *needle) {
  size_t nlen = strlen(needle);
  if (nlen == 0) {
    return s->length;
  }
  if (nlen > s->length) {
    return ccol_invalid_size;
  }
  if (nlen == 1) {
    const char *p =
        (const char *)cstring_memrchr(s->data, needle[0], s->length);
    return p ? (size_t)(p - s->data) : ccol_invalid_size;
  }
  cstring_rfind_scan sc = {.hay = s->data,
                           .length = s->length,
                           .needle = needle,
                           .nlen = nlen,
                           .charged = 0,
                           .budget = true,
                           .m_procs = s->m_procs};
  const size_t tail = nlen - 1;
  const unsigned char first = (unsigned char)needle[0];
  const unsigned char last = (unsigned char)needle[tail];
  const cstring_bytes16 vfirst = (cstring_bytes16){0} + first;
  const cstring_bytes16 vlast = (cstring_bytes16){0} + last;
  size_t result;

  size_t pos = s->length - tail;
  while (pos >= 16) {
    if (pos >= 64) {
      size_t b = pos - 64;
      const char *pf = sc.hay + b;
      const char *pl = pf + tail;
      cstring_bytes16 f0, f1, f2, f3, l0, l1, l2, l3;
      memcpy(&f0, pf, 16);
      memcpy(&f1, pf + 16, 16);
      memcpy(&f2, pf + 32, 16);
      memcpy(&f3, pf + 48, 16);
      memcpy(&l0, pl, 16);
      memcpy(&l1, pl + 16, 16);
      memcpy(&l2, pl + 32, 16);
      memcpy(&l3, pl + 48, 16);
      cstring_bytes16 any = (cstring_bytes16)(((f0 == vfirst) & (l0 == vlast)) |
                                              ((f1 == vfirst) & (l1 == vlast)) |
                                              ((f2 == vfirst) & (l2 == vlast)) |
                                              ((f3 == vfirst) & (l3 == vlast)));
      if (!cstring_bytes16_any(any)) {
        pos = b;
        continue;
      }
    }
    size_t b = pos - 16;
    cstring_bytes16 f, l;
    memcpy(&f, sc.hay + b, sizeof(f));
    memcpy(&l, sc.hay + b + tail, sizeof(l));
    cstring_bytes16 hit = (cstring_bytes16)((f == vfirst) & (l == vlast));
    if (cstring_bytes16_any(hit) && cstring_rfind_block(&sc, b, hit, &result)) {
      return result;
    }
    pos = b;
  }
  while (pos-- > 0) {
    if ((unsigned char)sc.hay[pos] == first &&
        (unsigned char)sc.hay[pos + tail] == last &&
        cstring_rfind_candidate(&sc, pos, &result)) {
      return result;
    }
  }
  return ccol_invalid_size;
}

// Gives the offset of the last needle in s, overlaps counted, or
// ccol_invalid_size when it's missing or needle is NULL. A string of 256 bytes
// or more goes straight to cstring_rfind_backward(); a shorter one is searched
// forward with strstr(), which is faster at that length. Every failed candidate
// of the backward filter is charged nlen, and past four times the bytes scanned
// the rest goes to the linear search, so a text built to defeat the filter
// costs O(n + m) all the same. Asserts on a NULL s.
size_t cstring_rfind(cstr s, const char *needle) {
  if (!s) {
    ccol__assert(false);
  }
  if (!needle) {
    return ccol_invalid_size;
  }
  if (s->length >= CSTRING_RFIND_SHORT) {
    return cstring_rfind_backward(s, needle);
  }
  if (needle[0] == '\0') {
    return s->length;
  }
  if (needle[1] == '\0') {
    const char *p =
        (const char *)cstring_memrchr(s->data, needle[0], s->length);
    return p ? (size_t)(p - s->data) : ccol_invalid_size;
  }
  size_t last = ccol_invalid_size;
  const char *ptr = s->data;
  const char *found;
  while ((found = strstr(ptr, needle)) != NULL) {
    last = (size_t)(found - s->data);
    ptr = found + 1;
  }
  return last;
}

// Creates a string holding at most length bytes of s from start, clamped to the
// end; a start past the end gives an empty string. It shares the allocator of
// s. Gives NULL with *err set on failure. Asserts on a NULL s.
cstr cstring_substring(cstr s, size_t start, size_t length, char **err) {
  if (!s) {
    ccol__assert(false);
  }

  if (start >= s->length) {
    return cstring_create_from_span(NULL, 0, s->m_procs, err);
  }

  size_t actual_len = ccol__min(length, s->length - start);
  return cstring_create_from_span(s->data + start, actual_len, s->m_procs, err);
}

// Creates an independent copy of s with the same allocator, or NULL with *err
// set on failure. Asserts on a NULL s.
cstr cstring_copy(cstr s, char **err) {
  if (!s) {
    ccol__assert(false);
  }
  return cstring_create_full(s->data, s->m_procs, err);
}

// Splits s at every delimiter into a cvec of new cstr, sharing the allocator of
// s; there is always at least one token, the tail, which may be empty. Gives
// NULL with *err set for a NULL or empty delimiter or a failed allocation,
// after destroying every token made so far. Asserts on a NULL s.
cvec cstring_split(cstr s, const char *delimiter, char **err) {
  if (!s) {
    ccol__assert(false);
  }
  if (!delimiter || delimiter[0] == '\0') {
    if (err) {
      *err = CCOL__ERR_STR("delimiter must be a non-empty string");
    }
    return NULL;
  }

  cvec result = cvector_create_full(sizeof(cstr), s->m_procs, err);
  if (!result) {
    return NULL;
  }

  size_t dlen = strlen(delimiter);
  const char *current = s->data;
  const char *found;

  while ((found = strstr(current, delimiter)) != NULL) {
    size_t part_len = (size_t)(found - current);

    cstr part = cstring_create_from_span(current, part_len, s->m_procs, err);
    if (!part) {
      destroy_cstr_vector(result);
      return NULL;
    }

    ccol_retval rv = cvector_push_back(result, &part);
    if (rv != ccol_success) {
      cstring__destroy(part);
      destroy_cstr_vector(result);
      if (err) {
        *err = CCOL__ERR_STR("failed to push split token into result vector");
      }
      return NULL;
    }

    current = found + dlen;
  }

  cstr last = cstring_create_from_span(
      current, s->length - (size_t)(current - s->data), s->m_procs, err);
  if (!last) {
    destroy_cstr_vector(result);
    return NULL;
  }

  ccol_retval rv = cvector_push_back(result, &last);
  if (rv != ccol_success) {
    cstring__destroy(last);
    destroy_cstr_vector(result);
    if (err) {
      *err =
          CCOL__ERR_STR("failed to push last split token into result vector");
    }
    return NULL;
  }

  if (err) {
    *err = NULL;
  }
  return result;
}

#ifdef RUNNING_UNIT_TESTS
// Gives the buffer capacity of s, so the tests can check the growth rules.
// Asserts on a NULL s.
size_t cstring__get_capacity(cstr s) {
  if (!s) {
    ccol__assert(false);
  }
  return s->capacity;
}

// Gives cstring_length_fits_with_terminator(length), so a test can drive that
// guard without a string that fills the address space.
bool cstring__length_fits_with_terminator_for_tests(size_t length) {
  return cstring_length_fits_with_terminator(length);
}

// Gives compute_replace_new_length() of its arguments, so a test can drive the
// overflow guards of cstring_replace() without huge strings.
ccol_retval cstring__replace_compute_new_length_for_tests(size_t orig_length,
                                                          size_t nlen,
                                                          size_t rlen,
                                                          size_t count,
                                                          size_t *new_len_out) {
  return compute_replace_new_length(orig_length, nlen, rlen, count,
                                    new_len_out);
}
#endif
