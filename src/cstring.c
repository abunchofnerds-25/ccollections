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

/* memrchr() is a GNU extension, which cstring_rfind() uses. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstring.h>
#include <ctype.h>
#include <internal/cpow2.h>

/* memrchr() is in glibc and FreeBSD's libc, but not in macOS's, where a
 * backward scan takes its place; _CCOL_EMULATE_DARWIN_MEMRCHR selects that
 * scan elsewhere, for the test suites. */
#if defined(__APPLE__) || defined(_CCOL_EMULATE_DARWIN_MEMRCHR)
static const void *_cstring_memrchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  const unsigned char b = (unsigned char)c;
  while (p != (const unsigned char *)s) {
    if (*--p == b) return p;
  }
  return NULL;
}
#else
#define _cstring_memrchr memrchr
#endif

static const size_t cstring_minimum_capacity = 16;

struct cstring {
  char *data;
  size_t length;
  size_t capacity;
  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/* Frees the character buffer and the container struct, in the same order as
 * __cvector_destroy: the code must read the free function pointer before it
 * frees the allocator struct that holds that pointer. */
void __cstring_destroy(cstr s) {
  if (s) {
    _ccol_mem_free(s->m_procs, s->data);

    if (s->m_procs) {
      ccol_free_t free_func = s->m_procs->free;
      free_func(s->m_procs);
      free_func(s);
    } else {
      ccol_mem_free(s);
    }
  }
}

/* True when a size_t can hold length + 1, where the extra 1 is the room for
 * the null terminator. A call site computes a buffer capacity as length + 1
 * before it gives that capacity to _ccol_find_nearest_gte_power_of_two(),
 * and every such call site must make this check first. The overflow
 * detection of that function works on the length + 1 value that the caller
 * has already computed, not on length itself, so a length of exactly
 * SIZE_MAX silently wraps length + 1 to 0 before that check runs. A capacity
 * of 0 always passes the "the capacity is already enough" fast path of
 * cstring_grow_to(), because 0 <= any real capacity. Without this check, the
 * wrap reports success, the code allocates nothing, and the caller then
 * reads or writes far past the real, small buffer. No real allocation can
 * reach this length, because a string that fills the full address space
 * cannot exist, but the check is explicit so that the code does not depend
 * on that fact. */
static bool cstring_length_fits_with_terminator(size_t length) {
  return length != ccol_invalid_size;
}

/* Grows the character buffer to at least needed_capacity bytes. This
 * function rounds the new size up to the nearest power of two, which
 * amortises the cost of later allocations, with cstring_minimum_capacity as
 * the smallest new size. The function does nothing if the current capacity
 * is already enough. */
static bool cstring_grow_to(cstr s, size_t needed_capacity) {
  if (needed_capacity <= s->capacity) {
    return true;
  }

  size_t new_cap = _ccol_find_nearest_gte_power_of_two(needed_capacity);
  if (new_cap == ccol_invalid_size) {
    return false;
  }
  if (new_cap < cstring_minimum_capacity) {
    new_cap = cstring_minimum_capacity;
  }

  char *orig = s->data;
  s->data = (char *)_ccol_mem_realloc(s->m_procs, s->data, new_cap);
  if (!s->data) {
    s->data = orig;
    return false;
  }

  s->capacity = new_cap;
  return true;
}

/* Cleanup helper for the error path: it walks the vector of cstr pointers
 * that cstring_split builds, destroys each cstr, and then destroys the
 * vector. A failure in the middle of a split must roll back every token that
 * the code has allocated up to that point, and this helper does that roll
 * back, so the caller always gets either a complete result or NULL. */
static void destroy_cstr_vector(cvec v) {
  if (!v) {
    return;
  }
  size_t count = cvector_elem_count(v);
  for (size_t i = 0; i < count; i++) {
    cstr *part = (cstr *)cvector_at(v, i);
    if (part && *part) {
      __cstring_destroy(*part);
    }
  }
  __cvector_destroy(v);
}

/* ========================================================================== */
/*                         CREATION / DESTRUCTION                             */
/* ========================================================================== */

/* Allocates and initialises a new cstr that holds the init_len bytes at
 * initial. initial can be NULL when init_len is 0, and the bytes must hold
 * no null byte. The capacity is the next power of two above init_len + 1,
 * and at least cstring_minimum_capacity, so the buffer is allocated once at
 * its final size. On failure this function frees every resource that it has
 * allocated up to that point and returns NULL. cstring_create_full(),
 * cstring_substring() and cstring_split() build every new cstr through it. */
static cstr cstring_create_from_span(const char *initial, size_t init_len,
                                     ccol_memmgmt_procs_t *m_procs,
                                     char **err) {
  if (!ccol_verify_memmgmt_procs(m_procs, err)) {
    return NULL;
  }

  cstr s = (cstr)_ccol_mem_calloc(m_procs, 1, sizeof(cstring));
  if (!s) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate cstring container");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(s, m_procs, err)) {
    _ccol_mem_free(m_procs, s);
    return NULL;
  }

  if (!cstring_length_fits_with_terminator(init_len)) {
    __cstring_destroy(s);
    if (err) {
      *err = CCOL_ERR_STR("initial string too large");
    }
    return NULL;
  }
  size_t init_cap = _ccol_find_nearest_gte_power_of_two(init_len + 1);
  if (init_cap == ccol_invalid_size) {
    __cstring_destroy(s);
    if (err) {
      *err = CCOL_ERR_STR("initial string too large");
    }
    return NULL;
  }
  if (init_cap < cstring_minimum_capacity) {
    init_cap = cstring_minimum_capacity;
  }

  s->data = (char *)_ccol_mem_alloc(s->m_procs, init_cap);
  if (!s->data) {
    __cstring_destroy(s);
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate cstring data buffer");
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

/* Allocates and initialises a new cstr whose first content comes from the
 * C string initial, which can be NULL for an empty string. */
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs_t *m_procs,
                         char **err) {
  return cstring_create_from_span(initial, initial ? strlen(initial) : 0,
                                  m_procs, err);
}

/* Returns the custom allocator that the cstr holds, or NULL when the cstr
 * uses the default malloc/free. */
ccol_memmgmt_procs_t *cstring_get_mprocs(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->m_procs;
}

/* ========================================================================== */
/*                         QUERY FUNCTIONS                                    */
/* ========================================================================== */

/* Returns the number of bytes in the string, without the null terminator.
 * The library keeps the length up to date after each operation, so this
 * function is O(1). */
size_t cstring_length(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->length;
}

/* Returns a read-only pointer to the null-terminated character buffer. Any
 * operation that changes the string, such as append or insert, makes the
 * pointer invalid. */
const char *cstring_c_str(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->data;
}

/* Returns the character at position idx, or '\0' if idx is out of range.
 * Unlike a direct index into an array, an access out of the bounds is not
 * fatal here. */
char cstring_at(cstr s, size_t idx) {
  if (!s) {
    ccol_assert(false);
  }
  if (idx >= s->length) {
    return '\0';
  }
  return s->data[idx];
}

/* Returns true when the string holds zero characters. */
bool cstring_is_empty(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->length == 0;
}

/* ========================================================================== */
/*                         MODIFICATION FUNCTIONS                             */
/* ========================================================================== */

/* Appends str to the end of the string, growing the buffer when it needs
 * more room. An empty str is valid, and the function then does nothing. The
 * overflow check on new_len covers the unlikely case where the combined
 * length wraps a size_t.
 *
 * The function starts on a 64-byte boundary. On an append that needs no
 * growth, the hot path branches forward to the short block that copies the
 * bytes, stores the new length and returns. That block must sit inside one
 * 64-byte line: a start offset of 32 or 48 bytes splits it across two lines,
 * and every append then costs about 45 percent more, with the same
 * instructions. Without the alignment the offset depends on the size of the
 * code that the linker places ahead of this function, so an unrelated edit
 * anywhere in the library could move it into the slow position. */
__attribute__((aligned(64))) ccol_retval_t cstring_append(cstr s,
                                                          const char *str) {
  if (!s) {
    ccol_assert(false);
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

  /* str can alias s->data. Save the offset before the grow, because realloc
   * can move the buffer, and then compute the pointer again from the new
   * base address. */
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

/* Prepends str to the front of the string. The function moves the old
 * content to the right with memmove before it writes the new prefix. The
 * buffer grows first when it needs more room, so memmove always works on
 * valid memory. */
ccol_retval_t cstring_prepend(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
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

  /* str can alias s->data. Save the offset before the grow, because realloc
   * can move the buffer. The memmove also moves any alias at an offset > 0
   * to the right by str_len, so such an alias needs a second correction. */
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

/* Inserts str at the byte position pos; a pos that equals s->length is
 * equivalent to cstring_append. The buffer grows before the memmove, so the
 * move into the new space is always safe. */
ccol_retval_t cstring_insert(cstr s, size_t pos, const char *str) {
  if (!s) {
    ccol_assert(false);
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

  /* str can alias s->data. Save the offset before the grow, because realloc
   * can move the buffer. The memmove also moves any alias at an offset > pos
   * to the right by str_len, so such an alias needs a second correction. */
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

  /* When alias_off <= pos, the source region [alias_off, alias_off+str_len)
   * can overlap the destination region [pos, pos+str_len) with dst > src.
   * Because memcpy copies forward, it corrupts the data in that case, while
   * memmove handles the overlap safely. */
  memmove(s->data + pos, str, str_len);
  s->length = new_len;

  return ccol_success;
}

/* Overwrites the full string content with str and drops the old content.
 * The memcpy also copies the null terminator, str_len + 1 bytes in total,
 * and the capacity check includes that byte. As in the other functions that
 * change the string, str can alias s->data, which is why the code saves the
 * offset before a possible realloc. */
ccol_retval_t cstring_set(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
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

/* Clears the string content and tries to shrink the buffer back to
 * cstring_minimum_capacity. The function always sets the length to zero; if
 * the reallocation fails, the buffer keeps its old capacity. A string that
 * is already at the smallest capacity skips the reallocation call, because
 * there is nothing to shrink. That is the common case, because a new string
 * and a string after a reset are both at that capacity. */
void cstring_reset(cstr s) {
  if (!s) {
    ccol_assert(false);
  }

  if (s->capacity != cstring_minimum_capacity) {
    char *orig = s->data;
    s->data = (char *)_ccol_mem_realloc(s->m_procs, s->data,
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

/* Makes sure that the buffer holds at least min_capacity bytes, growing it
 * when it needs more room. The function silently clamps a request below
 * cstring_minimum_capacity up to that value. */
bool cstring_reserve(cstr s, size_t min_capacity) {
  if (!s) {
    ccol_assert(false);
  }

  size_t needed = min_capacity < cstring_minimum_capacity
                      ? cstring_minimum_capacity
                      : min_capacity;
  return cstring_grow_to(s, needed);
}

/* Converts every character to upper case in-place. The C standard needs the
 * cast to unsigned char before the call to toupper: without it, the
 * behaviour is undefined when char is signed and the value is negative,
 * which is what a non-ASCII byte gives. */
void cstring_to_upper(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)toupper((unsigned char)s->data[i]);
  }
}

/* Converts every character to lower case in-place. The cast to unsigned
 * char is necessary here for the same reason as in cstring_to_upper. */
void cstring_to_lower(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)tolower((unsigned char)s->data[i]);
  }
}

/* Removes the whitespace at the start and at the end of the string,
 * in-place. The function trims the end first, because there it only moves
 * the null terminator backwards and needs no memory move; the whitespace at
 * the start then needs a memmove. */
void cstring_trim(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  if (s->length == 0) {
    return;
  }

  /* Trim the whitespace at the end first, which is cheaper because it only
   * moves the null terminator. */
  while (s->length > 0 && isspace((unsigned char)s->data[s->length - 1])) {
    s->length--;
  }
  s->data[s->length] = '\0';

  /* Trim the whitespace at the start. */
  size_t start = 0;
  while (start < s->length && isspace((unsigned char)s->data[start])) {
    start++;
  }
  if (start > 0) {
    memmove(s->data, s->data + start, s->length - start + 1);
    s->length -= start;
  }
}

/* Computes the new length of a string of orig_length bytes after a replace
 * that puts an rlen-byte replacement in the place of count non-overlapping
 * occurrences of an nlen-byte needle. The function detects an overflow of a
 * size_t both in the multiplication (rlen - nlen) * count and in the final
 * addition. count must be > 0, because the caller handles the case of zero
 * occurrences itself.
 *
 * This arithmetic is its own function rather than inline in
 * cstring_replace() so that a test can drive it directly. Reaching these
 * overflow guards through cstring_replace() itself needs real strings of
 * several gigabytes, such as a source string of several gigabytes that
 * holds one character many times, plus a replacement string of several
 * gigabytes, which is not practical for a normal test run. */
static ccol_retval_t compute_replace_new_length(size_t orig_length, size_t nlen,
                                                size_t rlen, size_t count,
                                                size_t *new_len_out) {
  ccol_assert(count > 0);

  if (rlen >= nlen) {
    size_t added = (rlen - nlen) * count;
    if (added / count != (rlen - nlen)) {
      return ccol_container_full;
    }
    size_t new_len = orig_length + added;
    /* The value new_len == orig_length + added can land on exactly SIZE_MAX.
     * Such a value is never "less than orig_length", which is the wraparound
     * that the first check below catches, but the caller of
     * cstring_replace() still needs a size_t that holds new_len + 1 for the
     * null terminator. This code must therefore reject that exact value too,
     * not only a real wraparound past it. */
    if (new_len < orig_length ||
        !cstring_length_fits_with_terminator(new_len)) {
      return ccol_container_full;
    }
    *new_len_out = new_len;
  } else {
    /* Unlike the growth branch above, this branch needs no guard against an
     * overflow or an underflow, for this reason. The code found count
     * occurrences of an nlen-byte needle in a string of orig_length bytes,
     * and those occurrences do not overlap, because the loop that counts
     * them in cstring_replace() moves forward by nlen after each match. The
     * relation count * nlen <= orig_length therefore always holds, and it
     * also bounds count * (nlen - rlen), which is not larger than
     * count * nlen. So the subtraction below can never underflow and the
     * multiplication in the middle can never overflow, for every input that
     * a caller gives to this function. */
    *new_len_out = orig_length - (nlen - rlen) * count;
  }

  return ccol_success;
}

/* Replaces all the occurrences of needle with replacement, in-place. The
 * function first counts the total number of occurrences, so that it can
 * allocate one new buffer of exactly the size that it needs before it
 * builds the new content. This keeps the number of reallocations at one. */
ccol_retval_t cstring_replace(cstr s, const char *needle,
                              const char *replacement) {
  if (!s) {
    ccol_assert(false);
  }
  if (!needle || needle[0] == '\0' || !replacement) {
    return ccol_invalid_args;
  }

  size_t nlen = strlen(needle);
  size_t rlen = strlen(replacement);

  /* Count the occurrences. */
  size_t count = 0;
  const char *ptr = s->data;
  while ((ptr = strstr(ptr, needle)) != NULL) {
    count++;
    ptr += nlen;
  }

  if (count == 0) {
    return ccol_success;
  }

  /* Compute the new length, which also checks for an overflow. */
  size_t new_len;
  ccol_retval_t len_rv =
      compute_replace_new_length(s->length, nlen, rlen, count, &new_len);
  if (len_rv != ccol_success) {
    return len_rv;
  }

  size_t new_cap = _ccol_find_nearest_gte_power_of_two(new_len + 1);
  if (new_cap == ccol_invalid_size) {
    return ccol_container_full;
  }
  if (new_cap < cstring_minimum_capacity) {
    new_cap = cstring_minimum_capacity;
  }

  char *new_buf = (char *)_ccol_mem_alloc(s->m_procs, new_cap);
  if (!new_buf) {
    return ccol_not_enough_memory;
  }

  /* Build the result string. */
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
  /* Copy the rest of the string, with the null terminator. */
  size_t tail = strlen(src);
  memcpy(dst, src, tail + 1);

  _ccol_mem_free(s->m_procs, s->data);
  s->data = new_buf;
  s->length = new_len;
  s->capacity = new_cap;

  return ccol_success;
}

/* ========================================================================== */
/*                         SEARCH AND COMPARISON                              */
/* ========================================================================== */

/* Compares the cstr against a C string with strcmp, in lexicographic order.
 * Returns 1 when str is NULL, which means that the cstr is the greater one,
 * following the convention that a non-null value is greater than a null
 * value. */
int cstring_compare(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return 1;
  }
  return strcmp(s->data, str);
}

/* Returns true when the content of the cstr is byte-for-byte identical to
 * str, and false when str is NULL, which counts as not equal. */
bool cstring_equals(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return false;
  }
  return strcmp(s->data, str) == 0;
}

/* Returns true when the string begins with prefix. The function rejects a
 * prefix that is longer than the string immediately, with no call to
 * strncmp. */
bool cstring_starts_with(cstr s, const char *prefix) {
  if (!s) {
    ccol_assert(false);
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

/* Returns true when the string ends with suffix. The function compares the
 * tail of the string, which starts at s->data + s->length - slen, with
 * strncmp, so it needs no temporary copy. */
bool cstring_ends_with(cstr s, const char *suffix) {
  if (!s) {
    ccol_assert(false);
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

/* Returns the byte offset of the first occurrence of needle, or
 * ccol_invalid_size when the function does not find needle. The function
 * delegates the work to strstr. */
size_t cstring_find(cstr s, const char *needle) {
  if (!s) {
    ccol_assert(false);
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
/* Counts the cstring_rfind() calls that hand the search over to
 * cstring_rfind_linear(). A white-box test uses it to prove that its inputs
 * reach that path, both with the table on the stack and with the table on
 * the heap. This counter is not part of the public API. */
unsigned long cstring_rfind_linear_count_for_tests = 0;
#endif

/* The largest needle whose failure table cstring_rfind_linear() keeps on the
 * stack; a longer needle takes its table from the allocator of the string. */
#define CSTRING_RFIND_STACK_TABLE_LEN 128

/* Returns the start of the rightmost occurrence of needle that lies wholly
 * inside hay[0, limit), or ccol_invalid_size when there is none. This is the
 * Knuth-Morris-Pratt search run from right to left: the pattern is the
 * needle read backward, and the scan reads hay backward from limit. The
 * failure table has one entry for each byte of the needle, so the search
 * costs O(limit + nlen) whatever the text and the needle hold.
 *
 * *table_ok is set to false, and nothing is searched, when the table does
 * not fit on the stack and the allocator refuses it. */
static __attribute__((noinline)) size_t cstring_rfind_linear(
    const char *hay, size_t limit, const char *needle, size_t nlen,
    ccol_memmgmt_procs_t *m_procs, bool *table_ok) {
#ifdef RUNNING_UNIT_TESTS
  __atomic_fetch_add(&cstring_rfind_linear_count_for_tests, 1,
                     __ATOMIC_RELAXED);
#endif
  size_t stack_table[CSTRING_RFIND_STACK_TABLE_LEN];
  size_t *fail = stack_table;
  if (nlen > CSTRING_RFIND_STACK_TABLE_LEN) {
    fail = nlen <= SIZE_MAX / sizeof(size_t)
               ? (size_t *)_ccol_mem_alloc(m_procs, nlen * sizeof(size_t))
               : NULL;
    if (!fail) {
      *table_ok = false;
      return ccol_invalid_size;
    }
  }
  *table_ok = true;

  /* The pattern byte k is needle[tail - k]. fail[k] is the length of the
   * longest proper border of the first k + 1 pattern bytes. */
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

  /* j pattern bytes are matched: hay[i, i + j) equals the last j bytes of
   * the needle. */
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
    _ccol_mem_free(m_procs, fail);
  }
  return found;
}

/* The state of one backward scan of cstring_rfind(). */
typedef struct {
  const char *hay;
  size_t length;
  const char *needle;
  size_t nlen;
  size_t charged;
  bool budget;
  ccol_memmgmt_procs_t *m_procs;
} cstring_rfind_scan;

/* Tests the candidate that starts at st, whose first and last bytes already
 * equal those of the needle. Returns true, and stores the answer in
 * *result, when the answer of the whole search is settled: either st
 * matches, or the scan has spent its budget and cstring_rfind_linear()
 * answered for every start below st. Returns false when the scan must go on
 * below st. */
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
    /* No memory for the table, so finish with the scan, which is always
     * correct. */
    sc->budget = false;
  }
  return false;
}

/* 16 bytes that the compiler handles as one vector register where the
 * target has one. */
typedef unsigned char cstring_bytes16 __attribute__((vector_size(16)));

/* The 16 candidate starts [b, b + 16) whose filter lanes are set in hit, from
 * the highest down. Returns true when the answer is settled, as
 * cstring_rfind_candidate() does. */
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

/* True when any lane of v is set. */
static inline bool cstring_bytes16_any(cstring_bytes16 v) {
  uint64_t halves[2];
  memcpy(halves, &v, sizeof(halves));
  return (halves[0] | halves[1]) != 0;
}

/* The length below which cstring_rfind() searches forward with strstr()
 * instead of scanning backward. Measured against the forward search, the
 * backward scan wins from about 100 bytes when the match is near the end,
 * breaks even at about 256 bytes when it is in the middle, and loses by a
 * few nanoseconds up to about 1 KiB when there is no match at all. */
#define CSTRING_RFIND_SHORT 256

/* The backward vector scan of cstring_rfind() for a string of at least
 * CSTRING_RFIND_SHORT bytes. It is a separate function so that only the
 * calls that scan backward pay for its stack frame and vector setup, and
 * the short path never does. */
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
        (const char *)_cstring_memrchr(s->data, needle[0], s->length);
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

  /* Every start position below pos is still to be tested. The main loop
   * filters 64 positions for each test of its result, and only when that
   * test fires does the 16-position loop below it look at the block lane by
   * lane. */
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
    /* The 16 positions below pos, lane by lane when any of them passes. */
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

/* Returns the byte offset of the last occurrence of needle, or
 * ccol_invalid_size when the function does not find needle. Overlapping
 * occurrences count, so rfind("ababa", "aba") is 2.
 *
 * A string shorter than CSTRING_RFIND_SHORT is searched forward with
 * strstr(). A longer one is scanned from the end toward its start, so the
 * first match that the scan meets is the answer and the bytes to its left
 * are never read. A one-byte needle is one memrchr(). A longer needle is
 * filtered with 16-byte vector compares: one compare tests the first byte of
 * the needle against 16 start positions, and another tests the last byte
 * against the 16 positions nlen - 1 further on. Only a position that passes
 * both is compared in full. A text needs both bytes of the needle, at the
 * right distance, to stop the filter, so ordinary text passes through it at
 * the speed of the two compares.
 *
 * The filter alone costs O(n * m) on a text and a needle built to agree on
 * long runs, such as a needle "aa...aba...a" in a text of 'a' bytes. The
 * function therefore charges nlen for every candidate that fails the full
 * compare, and once the charge exceeds four times the bytes that the scan
 * has passed, it hands the rest of the string to cstring_rfind_linear(),
 * which is O(n + m). So the scan does O(n) work before any hand-over, and an
 * ordinary text never pays for the table. A cstr holds no null byte before
 * its terminator, so the search covers exactly s->data[0, s->length). */
size_t cstring_rfind(cstr s, const char *needle) {
  if (!s) {
    ccol_assert(false);
  }
  if (!needle) {
    return ccol_invalid_size;
  }
  /* A long string goes straight to the backward scan, with nothing saved
   * here that it would have to restore first. */
  if (s->length >= CSTRING_RFIND_SHORT) {
    return cstring_rfind_backward(s, needle);
  }
  if (needle[0] == '\0') {
    return s->length;
  }
  if (needle[1] == '\0') {
    const char *p =
        (const char *)_cstring_memrchr(s->data, needle[0], s->length);
    return p ? (size_t)(p - s->data) : ccol_invalid_size;
  }
  /* Below CSTRING_RFIND_SHORT, the fixed cost of the backward vector scan
   * exceeds a forward search with the SIMD strstr() of the C library. That
   * search steps past each match, so its cost grows with the number of
   * matches, which is bounded on a string this short. A needle longer than
   * the string matches nowhere, which strstr() reports by itself. */
  size_t last = ccol_invalid_size;
  const char *ptr = s->data;
  const char *found;
  while ((found = strstr(ptr, needle)) != NULL) {
    last = (size_t)(found - s->data);
    ptr = found + 1;
  }
  return last;
}

/* ========================================================================== */
/*                    SUBSTRING, COPY AND SPLIT                               */
/* ========================================================================== */

/* Creates a new cstr that holds at most length bytes, starting at the byte
 * offset start. If start is past the end of the string, the function returns
 * an empty cstr rather than an error. The function clamps the real length to
 * the number of bytes that are left, and allocates the buffer of the result
 * once, at its final size. */
cstr cstring_substring(cstr s, size_t start, size_t length, char **err) {
  if (!s) {
    ccol_assert(false);
  }

  if (start >= s->length) {
    return cstring_create_from_span(NULL, 0, s->m_procs, err);
  }

  size_t actual_len = ccol_min(length, s->length - start);
  return cstring_create_from_span(s->data + start, actual_len, s->m_procs, err);
}

/* Creates an independent deep copy of s, with the same content and the same
 * allocator. */
cstr cstring_copy(cstr s, char **err) {
  if (!s) {
    ccol_assert(false);
  }
  return cstring_create_full(s->data, s->m_procs, err);
}

/* Splits the string at every occurrence of delimiter and returns a cvec of
 * cstr tokens. The result always holds at least one token: the tail after
 * the last delimiter, which can be empty. Each token is allocated once, at
 * its final size. On any allocation failure, destroy_cstr_vector destroys
 * every token that the function has made up to that point, and the function
 * returns NULL, so the caller gets either a complete result or nothing. */
cvec cstring_split(cstr s, const char *delimiter, char **err) {
  if (!s) {
    ccol_assert(false);
  }
  if (!delimiter || delimiter[0] == '\0') {
    if (err) {
      *err = CCOL_ERR_STR("delimiter must be a non-empty string");
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

    ccol_retval_t rv = cvector_push_back(result, &part);
    if (rv != ccol_success) {
      __cstring_destroy(part);
      destroy_cstr_vector(result);
      if (err) {
        *err = CCOL_ERR_STR("failed to push split token into result vector");
      }
      return NULL;
    }

    current = found + dlen;
  }

  /* The last token, which is empty when the string ends with the delimiter. */
  cstr last = cstring_create_from_span(
      current, s->length - (size_t)(current - s->data), s->m_procs, err);
  if (!last) {
    destroy_cstr_vector(result);
    return NULL;
  }

  ccol_retval_t rv = cvector_push_back(result, &last);
  if (rv != ccol_success) {
    __cstring_destroy(last);
    destroy_cstr_vector(result);
    if (err) {
      *err = CCOL_ERR_STR("failed to push last split token into result vector");
    }
    return NULL;
  }

  if (err) {
    *err = NULL;
  }
  return result;
}

/* ========================================================================== */
/*                         UNIT TEST INTERNALS                                */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/* Exposes the internal buffer capacity to the white-box unit tests, which
 * check the thresholds of the grow step and that the library keeps the
 * smallest capacity. This function is not part of the public API. */
size_t cstring_get_capacity(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->capacity;
}

/* Exposes cstring_length_fits_with_terminator() to the white-box unit
 * tests; the comment on that function explains why the check exists. This
 * function is not part of the public API. */
bool cstring_length_fits_with_terminator_for_tests(size_t length) {
  return cstring_length_fits_with_terminator(length);
}

/* Exposes the internal length arithmetic of cstring_replace(), which checks
 * for an overflow, to the white-box unit tests. The comment on
 * compute_replace_new_length() explains why this function is necessary. This
 * function is not part of the public API. */
ccol_retval_t cstring_replace_compute_new_length_for_tests(
    size_t orig_length, size_t nlen, size_t rlen, size_t count,
    size_t *new_len_out) {
  return compute_replace_new_length(orig_length, nlen, rlen, count,
                                    new_len_out);
}
#endif
