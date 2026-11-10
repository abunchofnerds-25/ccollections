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

// The one iteration API of every container. cbstmap.h, chashmap.h and
// cvector.h include it for you. ccol_begin() starts an iteration,
// ccol_iter_next() moves on, ccol_iter_key_ptr() and ccol_iter_val_ptr() give
// typed pointers, ccol_iter_destroy() stops early, and ccol_for_each() wraps
// the whole loop.

#include "common.h"

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// Incomplete types are enough for _Generic and for the prototypes below. The
// begin_iter prototypes live only here because ccol_begin() names all three,
// and a second declaration in a container header fails -Wredundant-decls.
struct cvector;
struct chashmap;
struct cbstmap;

// Creates an iterator at the first element of v; ccol_begin() calls it for a
// cvec. Gives NULL for an empty vector or a failed allocation (with *err set).
// The key is the element's index and the value points straight into the
// buffer. A NULL v iterates as an empty vector, like the two map versions, so
// a field that was never created can be iterated without a NULL check.
// Changing the vector invalidates the iterator, and ccol_iter_next() frees it
// at the end; use ccol_iter_destroy() to stop early.
cmap_iterator *cvector_begin_iter(struct cvector *v, char **err);

// Creates an iterator at the first element of chm; ccol_begin() calls it for
// a chmap. Gives NULL for an empty or NULL map or a failed allocation (with
// *err set). A separate-chaining map iterates newest first and an
// open-addressing map in slot order. Changing the map invalidates the
// iterator. It frees itself at the end; use ccol_iter_destroy() to stop
// early.
cmap_iterator *chashmap_begin_iter(struct chashmap *chm, char **err);

// Creates an iterator at the smallest key of cbm, and the walk visits the keys
// in ascending order; ccol_begin() calls it for a cbmap. Gives NULL for an
// empty or NULL map or a failed allocation (with *err set). Changing the map
// invalidates the iterator. It frees itself at the end; use
// ccol_iter_destroy() to stop early.
cmap_iterator *cbstmap_begin_iter(struct cbstmap *cbm, char **err);

// The unified iterator API

// Internal: frees *it and sets it to NULL; the cleanup handler of
// ccol_iter_declare(), so an iterator left by an early break isn't leaked. It
// frees through the iterator's own free function, so the right allocator is
// used.
static inline void ccol__iterator_destroy_cleanup(cmap_iterator **it) {
  if (it && *it) {
    (*it)->ccol__free_fn(*it);
    *it = NULL;
  }
}

// Declares the iterator it for a container declared with a _declare or
// _construct macro, together with the typed companions that
// ccol_iter_key_ptr() and ccol_iter_val_ptr() read to give typed pointers.
// The iterator is freed at the end of the scope.
//
//   ccol_iter_declare(vec, it);
//   for (it = ccol_begin(vec); it; it = ccol_iter_next(it))
//     printf("%d\n", *ccol_iter_val_ptr(it));
#define ccol_iter_declare(container, it)                                    \
  __typeof__(*container##__ccol_key_type_var) *it##__ccol_iter_key_type_var \
      __attribute__((unused)) = NULL;                                       \
  __typeof__(*container##__ccol_val_type_var) *it##__ccol_iter_val_type_var \
      __attribute__((unused)) = NULL;                                       \
  cmap_iterator *it ccol__destructor(ccol__iterator_destroy_cleanup) = NULL

// Internal: calls the iterator's own next function and gives its result; the
// body of ccol_iter_next(), written as a function so the argument is evaluated
// once.
static inline cmap_iterator *ccol__iter_next(cmap_iterator *it) {
  return it->ccol__next_fn(it);
}

// Moves the iterator on and evaluates to its new value, for the step of a
// loop. After the last element it frees the iterator and gives NULL.
#define ccol_iter_next(it) ccol__iter_next(it)

// Evaluates to a const KeyT * to the current key: the index for a cvec, the
// key for a map. A char * key of a map lives in the accessor itself, so its
// address is &key_pair->ptr.
#define ccol_iter_key_ptr(it)                                          \
  ({                                                                   \
    const __typeof__(*it##__ccol_iter_key_type_var) *ccol__iter_k;     \
    if ((it)->ccol__direct_ptr ||                                      \
        !ccol__is_char_ptr(*it##__ccol_iter_key_type_var)) {           \
      ccol__iter_k = (__typeof__(ccol__iter_k))((it)->key_pair->ptr);  \
    } else {                                                           \
      ccol__iter_k = (__typeof__(ccol__iter_k))(&(it)->key_pair->ptr); \
    }                                                                  \
    ccol__iter_k;                                                      \
  })

// Internal: converts p to the result type of ccol_iter_val_ptr(). For a
// character-pointer value type it adds const, because the iterator names the
// accessor slot whose size describes the stored string; you can edit the
// string in place but not store a new pointer the map didn't size. Every
// other type comes back as is.
#define ccol__iter_value_ptr_result(it, p)                                     \
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

// Internal: evaluates to the address of the current value for
// ccol_iter_val_ptr(). For a character-pointer value type it's the accessor
// slot &val_pair->ptr, otherwise val_pair->ptr. No branch casts const away,
// so ccol_iter_val_ptr() is clean under -Wcast-qual.
#define ccol__iter_val_slot(it)                    \
  _Generic(*it##__ccol_iter_val_type_var,          \
      char *: &(it)->val_pair->ptr,                \
      const char *: &(it)->val_pair->ptr,          \
      signed char *: &(it)->val_pair->ptr,         \
      const signed char *: &(it)->val_pair->ptr,   \
      unsigned char *: &(it)->val_pair->ptr,       \
      const unsigned char *: &(it)->val_pair->ptr, \
      default: (it)->val_pair->ptr)

// Evaluates to a ValT * to the current value, for reading or changing it in
// place; writing through it changes the container. For a character-pointer
// value type it's ValT const *: edit the string in place, or use the
// container's insert to store another one.
#define ccol_iter_val_ptr(it)                                              \
  ({                                                                       \
    __typeof__(ccol__iter_value_ptr_result(                                \
        it, (__typeof__(*it##__ccol_iter_val_type_var) *)0)) ccol__iter_v; \
    if ((it)->ccol__direct_ptr ||                                          \
        !ccol__is_char_ptr(*it##__ccol_iter_val_type_var)) {               \
      ccol__iter_v = (__typeof__(ccol__iter_v))((it)->val_pair->ptr);      \
    } else {                                                               \
      ccol__iter_v = (__typeof__(ccol__iter_v))(ccol__iter_val_slot(it));  \
    }                                                                      \
    ccol__iter_v;                                                          \
  })

// Frees the iterator and sets it to NULL, for a loop you leave before the end.
// it must be a modifiable lvalue and is evaluated once.
#define ccol_iter_destroy(it) \
  ccol__iter_destroy_impl(it, ccol__uniq(ccol__iter_destroy_slot, __COUNTER__))

// Internal: the body of ccol_iter_destroy(). It takes the address of it once
// into slot, a name from ccol__uniq(), so destroy macros can nest without
// -Wshadow warnings.
#define ccol__iter_destroy_impl(it, slot) \
  do {                                    \
    __typeof__(it) *slot = &(it);         \
    if (*slot) {                          \
      (*slot)->ccol__free_fn(*slot);      \
      *slot = NULL;                       \
    }                                     \
  } while (0)

// The value of an iterator past the end, for code that compares against it.
#define ccol_end NULL

// ccol_begin and ccol_for_each

// Internal: call the begin function of each container; ccol_begin() selects
// one with _Generic. They take void * so every branch type-checks for any
// container.
static inline __attribute__((always_inline)) cmap_iterator *ccol__cvec_begin(
    void *v, char **err) {
  return cvector_begin_iter((struct cvector *)v, err);
}
static inline __attribute__((always_inline)) cmap_iterator *ccol__chmap_begin(
    void *v, char **err) {
  return chashmap_begin_iter((struct chashmap *)v, err);
}
static inline __attribute__((always_inline)) cmap_iterator *ccol__cbmap_begin(
    void *v, char **err) {
  return cbstmap_begin_iter((struct cbstmap *)v, err);
}

// Starts an iteration over a cvec, chmap or cbmap, picking the begin function
// at compile time so one spelling works for every container. Evaluates to the
// first iterator, or NULL for an empty container; a failed allocation calls
// ccol__fatal_err(). There's no default association, so any other handle type
// is a compile error. Pair it with ccol_iter_declare().
#define ccol_begin(container)                                          \
  ({                                                                   \
    char *ccol__err = NULL;                                            \
    cmap_iterator *ccol__it = _Generic((container),                    \
        struct cvector *: ccol__cvec_begin,                            \
        struct chashmap *: ccol__chmap_begin,                          \
        struct cbstmap *: ccol__cbmap_begin)((container), &ccol__err); \
    if (!ccol__it && ccol__err) {                                      \
      ccol__fatal_err("ccol_begin('%s'): %s", #container, ccol__err);  \
    }                                                                  \
    ccol__it;                                                          \
  })

// Declares it and runs the body for every element of the container, the
// shortest way to write a whole loop. Commas in the body are fine. Don't
// change the container inside the body.
//
//   ccol_for_each(my_vec, it, {
//     printf("[%zu] = %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
//   });
#define ccol_for_each(container, it, ...)                                   \
  do {                                                                      \
    ccol_iter_declare(container, it);                                       \
    for (it = ccol_begin(container); it != NULL; it = ccol_iter_next(it)) { \
      __VA_ARGS__                                                           \
    }                                                                       \
  } while (0)

#pragma GCC visibility pop
