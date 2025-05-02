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

#include <cstring.h>
#include <ctype.h>

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

/* Releases the character buffer and the container struct. Mirrors
 * __cvector_destroy in the ordering requirement: the free function pointer
 * must be read before the allocator struct it belongs to is freed. */
void __cstring_destroy(cstr s) {
  if (s) {
    _mem_free(s->m_procs, s->data);

    if (s->m_procs) {
      ccol_free_t free_func = s->m_procs->free;
      free_func(s->m_procs);
      free_func(s);
    } else {
      mem_free(s);
    }
  }
}

/* Grows the character buffer to at least needed_capacity bytes. The new size
 * is rounded up to the nearest power of two to amortise future allocations,
 * with a floor of cstring_minimum_capacity. A no-op if current capacity
 * already satisfies the request. */
static bool cstring_grow_to(cstr s, size_t needed_capacity) {
  if (needed_capacity <= s->capacity) {
    return true;
  }

  size_t new_cap = find_nearest_gte_power_of_two(needed_capacity);
  if (new_cap == ccol_invalid_size) {
    return false;
  }
  if (new_cap < cstring_minimum_capacity) {
    new_cap = cstring_minimum_capacity;
  }

  char *orig = s->data;
  s->data = (char *)_mem_realloc(s->m_procs, s->data, new_cap);
  if (!s->data) {
    s->data = orig;
    return false;
  }

  s->capacity = new_cap;
  return true;
}

/* Error-path cleanup helper: iterates the vector of cstr pointers produced by
 * cstring_split, destroying each cstr before destroying the vector itself.
 * Used when a mid-split failure requires rolling back all tokens allocated so
 * far, so the caller always gets either a fully populated result or NULL. */
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

/* Allocates and initialises a new cstr, optionally pre-populated with the
 * C-string initial (may be NULL for an empty string). Initial capacity is
 * the next power of two above strlen(initial)+1, with a minimum of
 * cstring_minimum_capacity. On failure all partially allocated resources
 * are freed and NULL is returned. */
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs_t *m_procs,
                         char **err) {
  if (!ccol_verify_memmgmt_procs(m_procs, err)) {
    return NULL;
  }

  cstr s = (cstr)_mem_calloc(m_procs, 1, sizeof(cstring));
  if (!s) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate cstring container");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(s, m_procs, err)) {
    _mem_free(m_procs, s);
    return NULL;
  }

  size_t init_len = initial ? strlen(initial) : 0;
  size_t init_cap = find_nearest_gte_power_of_two(init_len + 1);
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

  s->data = (char *)_mem_alloc(s->m_procs, init_cap);
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

/* Returns the custom allocator stored in the cstr, or NULL for default
 * malloc/free. */
ccol_memmgmt_procs_t *cstring_get_mprocs(cstr s) { return s->m_procs; }

/* ========================================================================== */
/*                         QUERY FUNCTIONS                                    */
/* ========================================================================== */

/* Returns the number of bytes in the string, not counting the null terminator.
 * The length is maintained incrementally so this is O(1). */
size_t cstring_length(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->length;
}

/* Returns a read-only pointer to the null-terminated character buffer. The
 * pointer is invalidated by any mutating operation (append, insert, etc.). */
const char *cstring_c_str(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->data;
}

/* Returns the character at position idx, or '\0' if idx is out of range.
 * Unlike array indexing, an out-of-bounds access is non-fatal. */
char cstring_at(cstr s, size_t idx) {
  if (!s) {
    ccol_assert(false);
  }
  if (idx >= s->length) {
    return '\0';
  }
  return s->data[idx];
}

/* Returns true when the string contains zero characters. */
bool cstring_is_empty(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->length == 0;
}

/* ========================================================================== */
/*                         MODIFICATION FUNCTIONS                             */
/* ========================================================================== */

/* Appends str to the end of the string, growing the buffer as needed. An
 * empty str is accepted as a no-op. The overflow check on new_len handles the
 * unlikely case where the combined length wraps size_t. */
ccol_retval_t cstring_append(cstr s, const char *str) {
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
  if (new_len < s->length) {
    return ccol_container_full;
  }

  /* str may alias s->data: save the offset before grow (realloc may move
   * the buffer), then re-derive the pointer from the new base address. */
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

/* Prepends str to the front of the string. Existing content is shifted right
 * with memmove before the new prefix is written in. The buffer grows first if
 * needed, so memmove always operates on valid memory. */
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
  if (new_len < s->length) {
    return ccol_container_full;
  }

  /* str may alias s->data.  Save the offset before grow (realloc may move
   * the buffer).  After memmove, any alias at offset > 0 has also shifted
   * right by str_len, so a second correction is needed. */
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

/* Inserts str starting at byte position pos. pos == s->length is equivalent to
 * cstring_append. The buffer is grown before the memmove so the shift into the
 * freshly allocated space is always safe. */
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
  if (new_len < s->length) {
    return ccol_container_full;
  }

  /* str may alias s->data.  Save the offset before grow (realloc may move
   * the buffer).  After memmove, any alias at offset > pos has shifted right
   * by str_len and needs a second correction. */
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

  /* When alias_off <= pos the source region [alias_off, alias_off+str_len)
   * can overlap the destination [pos, pos+str_len) with dst > src, making
   * forward-copy memcpy corrupt data.  memmove handles that safely. */
  memmove(s->data + pos, str, str_len);
  s->length = new_len;

  return ccol_success;
}

/* Overwrites the entire string content with str, discarding the old content.
 * The null terminator is included in the memcpy (str_len + 1 bytes), so the
 * capacity check accounts for it. Like the other mutating functions, str may
 * alias s->data, so the offset is saved before any potential realloc. */
ccol_retval_t cstring_set(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return ccol_invalid_args;
  }

  size_t str_len = strlen(str);

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

/* Clears the string content and attempts to shrink the buffer back to
 * cstring_minimum_capacity. Length is zeroed unconditionally; if the
 * reallocation fails the buffer retains its previous capacity. */
void cstring_reset(cstr s) {
  if (!s) {
    ccol_assert(false);
  }

  char *orig = s->data;
  s->data = (char *)_mem_realloc(s->m_procs, s->data, cstring_minimum_capacity);
  if (!s->data) {
    s->data = orig;
  } else {
    s->capacity = cstring_minimum_capacity;
  }
  s->length = 0;
  s->data[0] = '\0';
}

/* Ensures the buffer is at least min_capacity bytes, growing if needed.
 * Requests below cstring_minimum_capacity are silently clamped up. */
bool cstring_reserve(cstr s, size_t min_capacity) {
  if (!s) {
    ccol_assert(false);
  }

  size_t needed = min_capacity < cstring_minimum_capacity
                      ? cstring_minimum_capacity
                      : min_capacity;
  return cstring_grow_to(s, needed);
}

/* Converts every character to upper case in-place. The cast to unsigned char
 * before passing to toupper is required by the C standard to avoid undefined
 * behaviour when char is signed and the value is negative (non-ASCII). */
void cstring_to_upper(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)toupper((unsigned char)s->data[i]);
  }
}

/* Converts every character to lower case in-place. Same unsigned char cast
 * caveat applies as in cstring_to_upper. */
void cstring_to_lower(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)tolower((unsigned char)s->data[i]);
  }
}

/* Removes leading and trailing whitespace in-place. Trailing whitespace is
 * trimmed first by simply walking the null terminator backwards – no memory
 * movement required. Leading whitespace then requires a memmove. */
void cstring_trim(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  if (s->length == 0) {
    return;
  }

  /* Trim trailing whitespace first (cheaper: just move the null terminator). */
  while (s->length > 0 && isspace((unsigned char)s->data[s->length - 1])) {
    s->length--;
  }
  s->data[s->length] = '\0';

  /* Trim leading whitespace. */
  size_t start = 0;
  while (start < s->length && isspace((unsigned char)s->data[start])) {
    start++;
  }
  if (start > 0) {
    memmove(s->data, s->data + start, s->length - start + 1);
    s->length -= start;
  }
}

/* Replaces all occurrences of needle with replacement in-place. To avoid
 * multiple reallocations, the total number of occurrences is counted first so
 * a single new buffer of the exact required size can be allocated before the
 * content is rebuilt. The overflow check on added bytes handles edge cases
 * where count * (rlen - nlen) exceeds size_t. */
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

  /* Count occurrences. */
  size_t count = 0;
  const char *ptr = s->data;
  while ((ptr = strstr(ptr, needle)) != NULL) {
    count++;
    ptr += nlen;
  }

  if (count == 0) {
    return ccol_success;
  }

  /* Calculate the new length, checking for overflow. */
  size_t new_len;
  if (rlen >= nlen) {
    size_t added = (rlen - nlen) * count;
    /* Overflow check: if count > 0 the division must round-trip. */
    if (count > 0 && added / count != (rlen - nlen)) {
      return ccol_container_full;
    }
    new_len = s->length + added;
    if (new_len < s->length) {
      return ccol_container_full;
    }
  } else {
    new_len = s->length - (nlen - rlen) * count;
  }

  size_t new_cap = find_nearest_gte_power_of_two(new_len + 1);
  if (new_cap == ccol_invalid_size) {
    return ccol_container_full;
  }
  if (new_cap < cstring_minimum_capacity) {
    new_cap = cstring_minimum_capacity;
  }

  char *new_buf = (char *)_mem_alloc(s->m_procs, new_cap);
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
  /* Copy remainder including the null terminator. */
  size_t tail = strlen(src);
  memcpy(dst, src, tail + 1);

  _mem_free(s->m_procs, s->data);
  s->data = new_buf;
  s->length = new_len;
  s->capacity = new_cap;

  return ccol_success;
}

/* ========================================================================== */
/*                         SEARCH AND COMPARISON                              */
/* ========================================================================== */

/* Lexicographically compares the cstr against a C-string using strcmp.
 * Returns 1 (cstr is "greater") when str is NULL, matching the convention
 * that a non-null object is considered greater than null. */
int cstring_compare(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return 1;
  }
  return strcmp(s->data, str);
}

/* Returns true when the cstr content is byte-for-byte identical to str.
 * Returns false (not equal) when str is NULL. */
bool cstring_equals(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return false;
  }
  return strcmp(s->data, str) == 0;
}

/* Returns true when the string begins with prefix. A prefix longer than the
 * string is immediately rejected without calling strncmp. */
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

/* Returns true when the string ends with suffix. Compares the tail of the
 * string (s->data + s->length - slen) using strncmp so no temporary copy is
 * needed. */
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
 * ccol_invalid_size if not found. Delegates to strstr. */
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

/* Returns the byte offset of the last occurrence of needle, or
 * ccol_invalid_size if not found. Implemented as a linear scan with strstr
 * advancing one byte at a time past each match, because the C standard
 * library provides no reverse-search counterpart to strstr.
 *
 * The +1 advance (not +nlen) is deliberate: it ensures overlapping occurrences
 * are considered, so the truly rightmost match is always found.  For example,
 * rfind("ababa", "aba") must return 2, not 0; advancing by nlen=3 would skip
 * the match at offset 2 entirely. */
size_t cstring_rfind(cstr s, const char *needle) {
  if (!s) {
    ccol_assert(false);
  }
  if (!needle) {
    return ccol_invalid_size;
  }

  size_t nlen = strlen(needle);
  if (nlen == 0) {
    return s->length;
  }
  if (nlen > s->length) {
    return ccol_invalid_size;
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

/* ========================================================================== */
/*                    SUBSTRING, COPY AND SPLIT                               */
/* ========================================================================== */

/* Creates a new cstr containing at most length bytes starting at byte offset
 * start. If start is beyond the end of the string, an empty cstr is returned
 * rather than an error. The actual length is clamped to the remaining bytes. */
cstr cstring_substring(cstr s, size_t start, size_t length, char **err) {
  if (!s) {
    ccol_assert(false);
  }

  if (s->length == 0 || start >= s->length) {
    return cstring_create_full("", s->m_procs, err);
  }

  size_t actual_len = ccol_min(length, s->length - start);

  cstr result = cstring_create_full(NULL, s->m_procs, err);
  if (!result) {
    return NULL;
  }

  if (actual_len > 0) {
    if (!cstring_grow_to(result, actual_len + 1)) {
      __cstring_destroy(result);
      if (err) {
        *err = CCOL_ERR_STR("failed to allocate substring buffer");
      }
      return NULL;
    }
    memcpy(result->data, s->data + start, actual_len);
    result->data[actual_len] = '\0';
    result->length = actual_len;
  }

  if (err) {
    *err = NULL;
  }
  return result;
}

/* Creates an independent deep copy of s with the same content and allocator. */
cstr cstring_copy(cstr s, char **err) {
  if (!s) {
    ccol_assert(false);
  }
  return cstring_create_full(s->data, s->m_procs, err);
}

/* Splits the string on every occurrence of delimiter and returns a cvec of
 * cstr tokens. The result always contains at least one token (the tail after
 * the last delimiter, which may be empty). On any allocation failure all
 * tokens created so far are destroyed via destroy_cstr_vector and NULL is
 * returned, so the caller either gets a complete result or nothing. */
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

    cstr part = cstring_create_full(NULL, s->m_procs, err);
    if (!part) {
      destroy_cstr_vector(result);
      return NULL;
    }

    if (part_len > 0) {
      if (!cstring_grow_to(part, part_len + 1)) {
        __cstring_destroy(part);
        destroy_cstr_vector(result);
        if (err) {
          *err = CCOL_ERR_STR("failed to allocate split token buffer");
        }
        return NULL;
      }
      memcpy(part->data, current, part_len);
      part->data[part_len] = '\0';
      part->length = part_len;
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

  /* Last token (may be empty if the string ended with the delimiter). */
  cstr last = cstring_create_full(current, s->m_procs, err);
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
/* Exposes the internal buffer capacity for white-box unit tests that verify
 * grow thresholds and minimum capacity enforcement. Not part of the public API.
 */
size_t cstring_get_capacity(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->capacity;
}
#endif
