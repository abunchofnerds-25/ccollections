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
 * @brief Unified iteration API for all the c_collections container types.
 *
 * cbstmap.h, chashmap.h and cvector.h include this header automatically.
 * Code that includes one single container header therefore gets the whole
 * unified iterator API, and it needs no other include. ccollections.h also
 * exports this header again, for a caller that needs all three containers
 * together.
 *
 * Public API:
 *   ccol_begin(container)              - begin iterator (dispatched on type)
 *   ccol_end                           - end sentinel (NULL)
 *   ccol_for_each(container, it, body) - range-for loop
 *   ccol_iter_declare(container, it)   - typed iterator declaration (RAII)
 *   ccol_iter_next(it)                 - move on and give the next iterator
 *   ccol_iter_key_ptr(it)              - typed const pointer to current key
 *   ccol_iter_val_ptr(it)              - typed pointer to current value
 *   ccol_iter_destroy(it)              - explicit early destruction
 */

#include "common.h"

/* Everything declared from here to the end of this header is part of the
 * public Application Binary Interface (ABI) of libccollections. The shared
 * library exports all of it. The library is built with
 * -fvisibility=hidden. A function or object that is not inside one of these
 * blocks stays internal to the library. It is absent from the dynamic
 * symbol table of the library. The application that links against the
 * library cannot interpose it. A symbol with the same name in that
 * application cannot collide with it. */
#pragma GCC visibility push(default)

/* Forward declarations. The _Generic type match needs only a pointer to an
 * incomplete struct. The begin_iter prototypes below need no more than
 * that either. The full struct definitions live in the header of each
 * container. */
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
 * @brief RAII cleanup helper. All the container iterators share it.
 *
 * This function calls the _free_fn of the iterator itself. The iterator
 * therefore uses the correct allocator, whichever container made it.
 */
static inline void ___ccol_iterator_destroy(cmap_iterator **it) {
  if (it && *it) {
    (*it)->_free_fn(*it);
    *it = NULL;
  }
}

/**
 * @brief Declare a unified, type-inferred iterator for any container.
 *
 * This macro works with cvec, chmap and cbmap. All three containers give
 * companion type variables. Their names are @c container##__ccol_key_type_var
 * and @c container##__ccol_val_type_var. This macro reads those two
 * variables. It then makes typed companion variables for the accessors of
 * the iterator @p it.
 *
 * @param container  Container variable. The caller must declare it with a
 *                   @c *_declare or @c *_construct macro first.
 * @param it         Name for the iterator variable.
 *
 * Example:
 * @code
 * ccol_iter_declare(vec, it);
 * for (it = ccol_begin(vec); it; it = ccol_iter_next(it))
 *   printf("%d\n", *ccol_iter_val_ptr(it));
 * @endcode
 */
#define ccol_iter_declare(container, it)                                    \
  __typeof__(*container##__ccol_key_type_var) *it##__ccol_iter_key_type_var \
      __attribute__((unused)) = NULL;                                       \
  __typeof__(*container##__ccol_val_type_var) *it##__ccol_iter_val_type_var \
      __attribute__((unused)) = NULL;                                       \
  cmap_iterator *it _ccol_destructor(___ccol_iterator_destroy) = NULL

/**
 * @brief Move a unified iterator on to the next element.
 *
 * This macro calls the @c _next_fn of the iterator itself, so it needs no
 * container argument. The macro destroys the iterator automatically at the
 * end, and it sets the iterator to NULL.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @return The new value of the iterator. The macro gives NULL after the
 *         last element.
 */
#define ccol_iter_next(it) ((it)->_next_fn(it))

/**
 * @brief Get a typed const pointer to the key of the iterator.
 *
 * For a cvec iterator, the key is the index of the element (a @c size_t).
 * For a map iterator, the key is the real key of the map. A char-pointer
 * key in a map goes through @c &key_pair->ptr, because of the SSO storage.
 * Every other type uses @c key_pair->ptr directly.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @return @c const @c KeyT* to the current key.
 */
#define ccol_iter_key_ptr(it)                                            \
  ({                                                                     \
    const __typeof__(*it##__ccol_iter_key_type_var) *__ccol_iter_k;      \
    if ((it)->_direct_ptr ||                                             \
        !ccol_is_char_ptr(*it##__ccol_iter_key_type_var)) {              \
      __ccol_iter_k = (__typeof__(__ccol_iter_k))((it)->key_pair->ptr);  \
    } else {                                                             \
      __ccol_iter_k = (__typeof__(__ccol_iter_k))(&(it)->key_pair->ptr); \
    }                                                                    \
    __ccol_iter_k;                                                       \
  })

/**
 * @brief Qualify the result of ccol_iter_val_ptr() for the value type.
 *
 * Consider a value type that is a character pointer. The iterator can then
 * name only one object: the accessor slot of the container. The companion
 * size field of that slot describes the string that the slot points at now.
 * A const qualifier on the target keeps the pointer and the size in
 * agreement. The caller can read the stored string. The caller can also
 * edit its bytes in place inside that length. A new value for the pointer
 * is a compile error. Without the const qualifier, a caller can store a
 * pointer that the container did not allocate and did not size. The size of
 * the accessor then describes a different string, and the next read of that
 * element runs past the end of the new string.
 *
 * Every other value type names the element itself. The macro gives such a
 * pointer back unchanged, and the caller can write through it.
 */
#define _ccol_iter_value_ptr_result(it, p)                                     \
  _Generic(*it##__ccol_iter_val_type_var,                                      \
      char *: (__typeof__(*it##__ccol_iter_val_type_var) const *)(p),          \
      const char *: (__typeof__(*it##__ccol_iter_val_type_var) const *)(p),    \
      signed char *: (__typeof__(*it##__ccol_iter_val_type_var) const *)(p),   \
      const signed char *: (__typeof__(*it##__ccol_iter_val_type_var)          \
                                const *)(p),                                   \
      unsigned char *: (__typeof__(*it##__ccol_iter_val_type_var) const *)(p), \
      const unsigned char *: (__typeof__(*it##__ccol_iter_val_type_var)        \
                                  const *)(p),                                 \
      default: (p))

/* The address that holds a character-pointer value in a map entry: the
 * accessor slot itself, &val_pair->ptr, whose type is void *const *. Only a
 * character-pointer value type selects it, and ccol_iter_val_ptr then
 * converts it to ValT const *, which keeps the const of the slot. Every
 * other value type selects val_pair->ptr, so no expression in
 * ccol_iter_val_ptr casts the const away from the slot, and the macro stays
 * clean under -Wcast-qual for every value type. */
#define _ccol_iter_val_slot(it)                    \
  _Generic(*it##__ccol_iter_val_type_var,          \
      char *: &(it)->val_pair->ptr,                \
      const char *: &(it)->val_pair->ptr,          \
      signed char *: &(it)->val_pair->ptr,         \
      const signed char *: &(it)->val_pair->ptr,   \
      unsigned char *: &(it)->val_pair->ptr,       \
      const unsigned char *: &(it)->val_pair->ptr, \
      default: (it)->val_pair->ptr)

/**
 * @brief Get a typed pointer to the value of the iterator, which is the
 *        current element.
 *
 * A change through this pointer also changes the container below it. For a
 * value type that is a character pointer, the target carries a const
 * qualifier. The caller therefore reads and edits the stored string in
 * place, and does not replace it. Use the insert operation or the set
 * operation of the owning container to store a different string.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @return @c ValT* to the current value or element. The macro gives
 *         @c ValT @c const* when @c ValT is a character pointer.
 */
#define ccol_iter_val_ptr(it)                                               \
  ({                                                                        \
    __typeof__(_ccol_iter_value_ptr_result(                                 \
        it, (__typeof__(*it##__ccol_iter_val_type_var) *)0)) __ccol_iter_v; \
    if ((it)->_direct_ptr ||                                                \
        !ccol_is_char_ptr(*it##__ccol_iter_val_type_var)) {                 \
      __ccol_iter_v = (__typeof__(__ccol_iter_v))((it)->val_pair->ptr);     \
    } else {                                                                \
      __ccol_iter_v = (__typeof__(__ccol_iter_v))(_ccol_iter_val_slot(it)); \
    }                                                                       \
    __ccol_iter_v;                                                          \
  })

/**
 * @brief Destroy a unified iterator and set its pointer to NULL.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @note The macro evaluates it exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ccol_iter_destroy(it) \
  _ccol_iter_destroy_impl(it, _ccol_uniq(__ccol_iter_destroy_slot, __COUNTER__))

/* Internal. The body of ccol_iter_destroy. slot is a name from _ccol_uniq(),
 * so the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_iter_destroy_impl(it, slot) \
  do {                                    \
    __typeof__(it) *slot = &(it);         \
    if (*slot) {                          \
      (*slot)->_free_fn(*slot);           \
      *slot = NULL;                       \
    }                                     \
  } while (0)

/** @brief Sentinel value for the end of an iteration (a NULL iterator). */
#define ccol_end NULL

/* ========================================================================== */
/*                    GENERIC ccol_begin / ccol_for_each                      */
/* ========================================================================== */

/* All three wrappers take a void*. Every __builtin_choose_expr branch is
 * therefore compatible in type, for every container type that a caller
 * passes. Only the selected branch runs. */
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
 * @brief Begin an iteration over any supported container.
 *
 * This macro selects the correct @c begin_iter at compile time. It uses
 * @c _Generic on the type of @p container. The supported types are
 * @c cvec, @c chmap and @c cbmap.
 *
 * The macro gives @c NULL, and no error, when the container is empty. It
 * calls @c ccol_fatal_err() when an allocation fails.
 *
 * @param container  A @c cvec, @c chmap or @c cbmap variable.
 *
 * @return @c cmap_iterator* at the first element, or @c NULL.
 *
 * @note Use @c ccol_iter_declare(container, it) with this macro. That pair
 * gives you typed accessors.
 */
/* The selection has no default association on purpose: a handle of any
 * other type is a compile error, never a call into another container's
 * begin function. */
#define ccol_begin(container)                                              \
  ({                                                                       \
    char *_ccol_err = NULL;                                                \
    cmap_iterator *_ccol_it = _Generic((container),                        \
        struct cvector *: __ccol_cvec_begin,                               \
        struct chashmap *: __ccol_chmap_begin,                             \
        struct cbinarymap *: __ccol_cbmap_begin)((container), &_ccol_err); \
    if (!_ccol_it && _ccol_err) {                                          \
      ccol_fatal_err("ccol_begin('%s'): %s", #container, _ccol_err);       \
    }                                                                      \
    _ccol_it;                                                              \
  })

/**
 * @brief Iterate over all the elements of any supported container.
 *
 * This macro declares a @c cmap_iterator* variable @p it with RAII cleanup.
 * It then loops from @c ccol_begin() to the end with @c ccol_iter_next().
 * @p it gives typed access through @c ccol_iter_key_ptr(it) and
 * @c ccol_iter_val_ptr(it).
 *
 * @param container  A @c cvec, @c chmap or @c cbmap variable.
 * @param it         Name for the iterator variable.
 * @param ...        Compound statement that the macro runs for each
 *                   element.
 *
 * @note A comma inside a function call with more than one argument is safe
 * in the body.
 * @note Do NOT change the container inside the body.
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

#pragma GCC visibility pop
