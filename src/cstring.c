/*
MIT License

Copyright (c) 2024 A bunch of nerds

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

void __cstring_destroy(cstr s) {
  if (s) {
    _mem_free(s->m_procs, s->data);

    if (s->m_procs) {
      ccol_memmgmt_procs_free_t free_func = s->m_procs->free;
      free_func(s->m_procs);
      free_func(s);
    } else {
      mem_free(s);
    }
  }
}

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

/* Frees every cstr stored in the vector, then destroys the vector itself. */
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
  if (init_cap == ccol_invalid_size || init_cap < cstring_minimum_capacity) {
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

ccol_memmgmt_procs_t *cstring_get_mprocs(cstr s) { return s->m_procs; }

/* ========================================================================== */
/*                         QUERY FUNCTIONS                                    */
/* ========================================================================== */

size_t cstring_length(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->length;
}

const char *cstring_c_str(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->data;
}

char cstring_at(cstr s, size_t idx) {
  if (!s) {
    ccol_assert(false);
  }
  if (idx >= s->length) {
    return '\0';
  }
  return s->data[idx];
}

bool cstring_is_empty(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->length == 0;
}

/* ========================================================================== */
/*                         MODIFICATION FUNCTIONS                             */
/* ========================================================================== */

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

  if (!cstring_grow_to(s, new_len + 1)) {
    return ccol_not_enough_memory;
  }

  memcpy(s->data + s->length, str, str_len);
  s->length = new_len;
  s->data[s->length] = '\0';

  return ccol_success;
}

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

  if (!cstring_grow_to(s, new_len + 1)) {
    return ccol_not_enough_memory;
  }

  memmove(s->data + str_len, s->data, s->length + 1);
  memcpy(s->data, str, str_len);
  s->length = new_len;

  return ccol_success;
}

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

  if (!cstring_grow_to(s, new_len + 1)) {
    return ccol_not_enough_memory;
  }

  memmove(s->data + pos + str_len, s->data + pos, s->length - pos + 1);
  memcpy(s->data + pos, str, str_len);
  s->length = new_len;

  return ccol_success;
}

ccol_retval_t cstring_set(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return ccol_invalid_args;
  }

  size_t str_len = strlen(str);

  if (!cstring_grow_to(s, str_len + 1)) {
    return ccol_not_enough_memory;
  }

  memcpy(s->data, str, str_len + 1);
  s->length = str_len;

  return ccol_success;
}

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

bool cstring_reserve(cstr s, size_t min_capacity) {
  if (!s) {
    ccol_assert(false);
  }

  size_t needed = min_capacity < cstring_minimum_capacity
                      ? cstring_minimum_capacity
                      : min_capacity;
  return cstring_grow_to(s, needed);
}

void cstring_to_upper(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)toupper((unsigned char)s->data[i]);
  }
}

void cstring_to_lower(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  for (size_t i = 0; i < s->length; i++) {
    s->data[i] = (char)tolower((unsigned char)s->data[i]);
  }
}

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

int cstring_compare(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return 1;
  }
  return strcmp(s->data, str);
}

bool cstring_equals(cstr s, const char *str) {
  if (!s) {
    ccol_assert(false);
  }
  if (!str) {
    return false;
  }
  return strcmp(s->data, str) == 0;
}

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

cstr cstring_copy(cstr s, char **err) {
  if (!s) {
    ccol_assert(false);
  }
  return cstring_create_full(s->data, s->m_procs, err);
}

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
size_t cstring_get_capacity(cstr s) {
  if (!s) {
    ccol_assert(false);
  }
  return s->capacity;
}
#endif
