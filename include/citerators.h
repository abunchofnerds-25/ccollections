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

#pragma once

/**
 * @file citerators.h
 * @brief Unified iteration API for all c_collections container types.
 *
 * Automatically included by cbstmap.h, chashmap.h, and cvector.h, so any
 * code that includes a single container header gets the full unified iterator
 * API without an extra include.  Also re-exported by ccollections.h for
 * callers that need all three containers at once.
 *
 * Public API:
 *   ccol_begin(container)              - begin iterator (type-dispatched)
 *   ccol_end                           - end sentinel (NULL)
 *   ccol_for_each(container, it, body) - range-for loop
 *   ccol_iter_declare(container, it)   - typed iterator declaration (RAII)
 *   ccol_iter_next(it)                 - advance and return next iterator
 *   ccol_iter_key_ptr(it)              - typed const pointer to current key
 *   ccol_iter_val_ptr(it)              - typed pointer to current value
 *   ccol_iter_destroy(it)              - explicit early destruction
 */

#include <common.h>

/* Forward declarations; only pointer-to-incomplete-struct is needed for
 * _Generic type matching and for the begin_iter function prototypes below.
 * Full struct definitions live in each container's own header. */
struct cvector;
struct chashmap;
struct cbinarymap;

cmap_iterator *cvector_begin_iter(struct cvector *v, char **err);
cmap_iterator *chashmap_begin_iter(struct chashmap *v, char **err);
cmap_iterator *cbmap_begin_iter(struct cbinarymap *v, char **err);

/* ========================================================================== */
/*                    UNIFIED ITERATOR API                                    */
/* ========================================================================== */

/**
 * @brief RAII cleanup helper shared by all container iterators.
 *
 * Calls the iterator's own _free_fn so the right allocator is used regardless
 * of which container produced the iterator.
 */
static inline void ___ccol_iterator_destroy(cmap_iterator **it) {
  if (it && *it) {
    (*it)->_free_fn(*it);
    *it = NULL;
  }
}

/**
 * @brief Declare a unified, type-safe iterator for any container.
 *
 * Works with cvec, chmap, and cbmap.  All three containers expose companion
 * type variables named @c container##__ccol_key_type_var and
 * @c container##__ccol_val_type_var; this macro harvests them to produce
 * typed accessor companion variables for the iterator @p it.
 *
 * @param container  Container variable (must have been declared with a
 *                   @c *_declare / @c *_construct macro).
 * @param it         Name for the iterator variable.
 *
 * Example:
 * @code
 * ccol_iter_declare(vec, it);
 * for (it = ccol_begin(vec); it; it = ccol_iter_next(it))
 *   printf("%d\n", *ccol_iter_val_ptr(it));
 * @endcode
 */
#define ccol_iter_declare(container, it)                                \
  typeof(*container##__ccol_key_type_var) *it##__ccol_iter_key_type_var \
      __attribute__((unused)) = NULL;                                   \
  typeof(*container##__ccol_val_type_var) *it##__ccol_iter_val_type_var \
      __attribute__((unused)) = NULL;                                   \
  cmap_iterator *it _ccol_destructor(___ccol_iterator_destroy)

/**
 * @brief Advance a unified iterator to the next element.
 *
 * Delegates to the iterator's own @c _next_fn so no container argument is
 * needed.  Automatically destroys and NULLs the iterator when the end is
 * reached.
 *
 * @param it  Iterator variable declared with ccol_iter_declare().
 *
 * @return The (updated) iterator, or NULL when past the last element.
 */
#define ccol_iter_next(it) ((it)->_next_fn(it))

/**
 * @brief Get a typed const pointer to the iterator's key.
 *
 * For cvec iterators the key is the element index (@c size_t).  For map
 * iterators the key is the actual map key.  Char-pointer keys in maps are
 * handled via @c &key_pair->ptr (SSO); all other types use @c key_pair->ptr
 * directly.
 *
 * @param it  Iterator variable declared with ccol_iter_declare().
 *
 * @return @c const @c KeyT* to the current key.
 */
#define ccol_iter_key_ptr(it)                                               \
  ({                                                                        \
    const typeof(*it##__ccol_iter_key_type_var) *_key;                      \
    if ((it)->_direct_ptr || !is_char_ptr(*it##__ccol_iter_key_type_var)) { \
      _key = (typeof(_key))((it)->key_pair->ptr);                           \
    } else {                                                                \
      _key = (typeof(_key))(&(it)->key_pair->ptr);                          \
    }                                                                       \
    _key;                                                                   \
  })

/**
 * @brief Get a typed pointer to the iterator's value (current element).
 *
 * Mutations through this pointer are reflected in the underlying container.
 *
 * @param it  Iterator variable declared with ccol_iter_declare().
 *
 * @return @c ValT* to the current value / element.
 */
#define ccol_iter_val_ptr(it)                                               \
  ({                                                                        \
    typeof(*it##__ccol_iter_val_type_var) *_val;                            \
    if ((it)->_direct_ptr || !is_char_ptr(*it##__ccol_iter_val_type_var)) { \
      _val = (typeof(_val))((it)->val_pair->ptr);                           \
    } else {                                                                \
      _val = (typeof(_val))(&(it)->val_pair->ptr);                          \
    }                                                                       \
    _val;                                                                   \
  })

/**
 * @brief Destroy a unified iterator and set its pointer to NULL.
 *
 * @param it  Iterator variable declared with ccol_iter_declare().
 */
#define ccol_iter_destroy(it) \
  do {                        \
    if (it) {                 \
      (it)->_free_fn(it);     \
      (it) = NULL;            \
    }                         \
  } while (0)

/** @brief Sentinel value representing the end of iteration (NULL iterator). */
#define ccol_end NULL

/* ========================================================================== */
/*                    GENERIC ccol_begin / ccol_for_each                      */
/* ========================================================================== */

/* All three wrappers take void* so every __builtin_choose_expr branch is
 * type-compatible regardless of which container type is passed; only the
 * selected branch is ever executed. */
static inline __attribute__((always_inline)) cmap_iterator *__ccol_cvec_begin(
    void *v, char **err) {
  return cvector_begin_iter((struct cvector *)v, err);
}
static inline __attribute__((always_inline)) cmap_iterator *__ccol_chmap_begin(
    void *v, char **err) {
  return chashmap_begin_iter((struct chashmap *)v, err);
}
static inline __attribute__((always_inline)) cmap_iterator *__ccol_cbmap_begin(
    void *v, char **err) {
  return cbmap_begin_iter((struct cbinarymap *)v, err);
}

/**
 * @brief Begin iteration over any supported container.
 *
 * Selects the right @c begin_iter at compile time via @c _Generic on the type
 * of @p container.  Supported types: @c cvec, @c chmap, @c cbmap.
 *
 * Returns @c NULL without error when the container is empty.  Calls
 * @c fatal_err() on allocation failure.
 *
 * @param container  A @c cvec, @c chmap, or @c cbmap variable.
 *
 * @return @c cmap_iterator* positioned at the first element, or @c NULL.
 *
 * @note Pair with @c ccol_iter_declare(container, it) to get typed accessors.
 */
#define ccol_begin(container)                                        \
  ({                                                                 \
    char *_ccol_err = NULL;                                          \
    cmap_iterator *_ccol_it = __builtin_choose_expr(                 \
        _Generic((container), struct cvector *: 1, default: 0),      \
        __ccol_cvec_begin((container), &_ccol_err),                  \
        __builtin_choose_expr(                                       \
            _Generic((container), struct chashmap *: 1, default: 0), \
            __ccol_chmap_begin((container), &_ccol_err),             \
            __ccol_cbmap_begin((container), &_ccol_err)));           \
    if (!_ccol_it && _ccol_err) {                                    \
      fatal_err("ccol_begin('%s'): %s", #container, _ccol_err);      \
    }                                                                \
    _ccol_it;                                                        \
  })

/**
 * @brief Iterate over all elements of any supported container.
 *
 * Declares a @c cmap_iterator* variable @p it with RAII cleanup, then loops
 * from @c ccol_begin() to the end using @c ccol_iter_next().  @p it gives
 * typed access via @c ccol_iter_key_ptr(it) and @c ccol_iter_val_ptr(it).
 *
 * @param container  A @c cvec, @c chmap, or @c cbmap variable.
 * @param it         Name for the iterator variable.
 * @param ...        Compound statement executed per element.
 *
 * @note Commas inside multi-argument function calls in the body are safe.
 * @note Do NOT modify the container inside the body.
 *
 * Example:
 * @code
 * ccol_for_each(my_vec, it, {
 *   printf("[%zu] = %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
 * });
 * @endcode
 */
#define ccol_for_each(container, it, ...)                                   \
  do {                                                                      \
    ccol_iter_declare(container, it);                                       \
    for (it = ccol_begin(container); it != NULL; it = ccol_iter_next(it)) { \
      __VA_ARGS__                                                           \
    }                                                                       \
  } while (0)
