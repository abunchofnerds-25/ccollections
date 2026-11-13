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

// Creates an iterator at the first element of v, for walking a vector without
// the typed macros. The key is the element's index (a size_t) and the value
// points straight into the buffer. An empty or NULL v gives NULL with *err set
// to NULL, so a vector that was never created iterates as an empty one; a
// failed allocation gives NULL with a library-owned error string in *err.
// Changing the vector invalidates the iterator. ccol_iter_next() frees it
// after the last element; call ccol_iter_destroy() to stop early.
cmap_iterator *cvector_begin_iter(struct cvector *v, char **err);

// Creates an iterator at the first entry of chm, for walking a hash map
// without the typed macros. An empty or NULL map gives NULL with *err set to
// NULL; a failed allocation gives NULL with a library-owned error string in
// *err. The order isn't sorted and depends on the key and value types, as
// chashmap_begin_iter(3) describes. Changing the map invalidates the
// iterator. ccol_iter_next() frees it after the last entry; call
// ccol_iter_destroy() to stop early.
cmap_iterator *chashmap_begin_iter(struct chashmap *chm, char **err);

// Creates an iterator at the smallest key of cbm, for walking an ordered map in
// ascending key order without the typed macros. An empty or NULL map gives
// NULL with *err set to NULL; a failed allocation gives NULL with a
// library-owned error string in *err. Changing the map invalidates the
// iterator. ccol_iter_next() frees it after the last entry; call
// ccol_iter_destroy() to stop early.
cmap_iterator *cbstmap_begin_iter(struct cbstmap *cbm, char **err);

// The unified iterator API

// Internal: Frees *it and sets it to NULL; the cleanup handler of
// ccol_iter_declare(), so an iterator left by an early break isn't leaked. It
// frees through the iterator's own free function, so the right allocator is
// used.
static inline void ccol__iterator_destroy_cleanup(cmap_iterator **it) {
  if (it && *it) {
    (*it)->ccol__free_fn(*it);
    *it = NULL;
  }
}

// Declares the iterator variable it, set to NULL, for a cvec, chmap or cbmap
// container that a _declare, _construct or _redeclare macro declared, so that
// ccol_iter_key_ptr() and ccol_iter_val_ptr() give pointers of the right
// types. An iterator left in it when it goes out of scope is freed, so a
// break out of the loop leaks nothing. It's a declaration.
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

// Internal: Calls the iterator's own next function and gives its result; the
// body of ccol_iter_next(), written as a function so the argument is evaluated
// once.
static inline cmap_iterator *ccol__iter_next(cmap_iterator *it) {
  return it->ccol__next_fn(it);
}

// Moves the iterator it to the next element and evaluates to it, for the step
// of a loop; it is evaluated once. After the last element it frees the
// iterator and gives NULL, so assign the result back: It = ccol_iter_next(it).
#define ccol_iter_next(it) ccol__iter_next(it)

// Evaluates to a const pointer to the current key, typed after the key type of
// the container: A const size_t * to the index for a cvec, a const KeyT * for
// a map. For a char * key it points to the stored char *, so *ptr is the
// string. it must come from ccol_iter_declare(). Valid until the iterator
// moves or the container changes.
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

// Internal: Converts p to the result type of ccol_iter_val_ptr(). For a
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

// Internal: Evaluates to the address of the current value for
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
// value type it's ValT const *, so *ptr is the stored string: Edit its bytes
// in place, or use the container's insert to store another one. it must come
// from ccol_iter_declare(). Valid until the iterator moves or the container
// changes.
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
// it must be a modifiable lvalue and is evaluated once; a NULL it does
// nothing. Don't call it after ccol_iter_next() gave NULL, since the iterator
// is already freed then. It's a statement.
#define ccol_iter_destroy(it) \
  ccol__iter_destroy_impl(it, ccol__uniq(ccol__iter_destroy_slot, __COUNTER__))

// Internal: The body of ccol_iter_destroy(). It takes the address of it once
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

// The value of an iterator past the end (NULL), for code that compares an
// iterator against it.
#define ccol_end NULL

// ccol_begin and ccol_for_each

// Internal: Call the begin function of each container; ccol_begin() selects
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

// Starts an iteration over a cvec, chmap or cbmap; one spelling works for
// every container. Evaluates to an iterator at the first element, or NULL for
// an empty or NULL container. A failed allocation prints the reason and ends
// the program. Any other handle type is a compile error. Assign the result to
// an iterator from ccol_iter_declare(), and step with ccol_iter_next().
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

// Declares the iterator it and runs the body (the last argument) once for
// every element of a cvec, chmap or cbmap, the shortest way to write a whole
// loop. Inside the body use ccol_iter_key_ptr(it) and ccol_iter_val_ptr(it).
// Commas in the body are fine, and a break leaks nothing. Don't change the
// container inside the body. A failed allocation at the start prints the
// reason and ends the program.
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
