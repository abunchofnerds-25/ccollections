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

#include "citerators.h"
#include "common.h"
#include "csort.h"

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// A generic dynamic array of fixed-size elements. It starts with room for 4
// elements, doubles when it's full and halves when less than 1/4 is in use,
// never going below 4. The struct is opaque and cvec is the handle.
typedef struct cvector cvector;

// The handle of a vector of fixed-size elements, a pointer to an opaque
// cvector. Make one with cvec_construct() or cvector_create() and free it with
// cvec_destroy() or cvector_destroy().
typedef cvector *cvec;

// Creates an empty vector of elem_size-byte elements with room for 4, using
// your own allocator. elem_size must be more than 0 and at most SIZE_MAX / 4.
// mmgmt_procs is the allocator table, or NULL for malloc and free; all four of
// its functions must be set, and the vector keeps its own copy of the table.
// On failure it gives NULL and, when err isn't NULL, a library-owned error
// string in *err. Destroy the vector with cvector_destroy().
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs *mmgmt_procs,
                         char **err);

// Creates an empty vector of elem_size-byte elements with room for 4, using
// malloc and free. elem_size must be more than 0 and at most SIZE_MAX / 4. On
// failure it gives NULL and, when err isn't NULL, a library-owned error string
// in *err. Destroy the vector with cvector_destroy().
static inline __attribute__((always_inline)) cvec
cvector_create(size_t elem_size, char **err) {
  return cvector_create_full(elem_size, NULL, err);
}

// Gives the allocator of the vector, or NULL when it uses the default one, so
// you can allocate related memory the same way. Asserts on a NULL v.
ccol_memmgmt_procs *cvector_get_mprocs(cvec v);

// Internal: Frees the vector; it's the body of cvector_destroy(), which also
// clears your pointer. Use the macro instead.
void cvector__destroy(cvec v);

// Frees the vector and its buffer and sets v to NULL, so no stale pointer to
// the freed vector stays behind. v must be a modifiable lvalue and is evaluated
// once; a NULL v does nothing. Memory that the elements point to isn't freed
// for you. It's a statement and gives no value.
#define cvector_destroy(v)    \
  ccol__cvector_destroy_impl( \
      v, ccol__uniq(ccol__cvector_destroy_slot, __COUNTER__))

// Internal: The body of cvector_destroy(). It takes the address of v once into
// slot, which comes from ccol__uniq() so destroy macros can nest without
// -Wshadow warnings.
#define ccol__cvector_destroy_impl(v, slot) \
  do {                                      \
    __typeof__(v) *slot = &(v);             \
    if (*slot) {                            \
      cvector__destroy(*slot);              \
      *slot = NULL;                         \
    }                                       \
  } while (0)

// Copies *new_elem to the end of the vector, doubling the capacity when it's
// full; it's the raw append for code that must not abort. It gives
// ccol_success, ccol_invalid_args for a NULL new_elem, ccol_container_full at
// ccol_max_elem_count, or ccol_not_enough_memory when the growth fails.
// new_elem may point into v's own buffer (from cvector_at() or
// cvector_data_ptr()); a growth doesn't break that. Asserts on a NULL v.
ccol_retval cvector_push_back(cvec v, const void *new_elem);

// Removes the last element and copies it to *target_elem; it's the raw pop for
// code that must not abort, and the only pop for an array element type. It
// gives ccol_success, ccol_invalid_args for a NULL target_elem, or
// ccol_container_empty. The capacity halves when less than 1/4 of it is in
// use. Asserts on a NULL v.
ccol_retval cvector_pop_back(cvec v, void *target_elem);

// Gives a pointer to the element at index, or NULL when index is out of
// range, so you can read or change an element in place without aborting on a
// bad index. The pointer is valid until the size of the vector changes.
// Asserts on a NULL v.
void *cvector_at(cvec v, size_t index);

// Gives the number of elements; the struct is opaque, so this is how you read
// it. Asserts on a NULL v.
size_t cvector_size(cvec v);

// Removes every element and tries to shrink the capacity back to 4, so you can
// reuse a vector without creating a new one. When that realloc fails the
// capacity stays, but the count is 0 anyway. Elements aren't freed one by
// one. Asserts on a NULL v.
void cvector_reset(cvec v);

// Makes room for at least new_capacity_count elements, rounded up to a power
// of two (4 at least), so when you know the final size the pushes up to it
// don't reallocate. It never shrinks, and gives false when the request is
// above ccol_max_elem_count or the realloc fails. Asserts on a NULL v.
bool cvector_reserve(cvec v, size_t new_capacity_count);

// Appends elem_count elements from arr_ptr with at most one reallocation,
// which is cheaper than a push per element. An elem_count of 0 gives true and
// does nothing. It gives false, with v unchanged, for a NULL arr_ptr, on an
// overflow, above ccol_max_elem_count, or when memory runs out. arr_ptr may
// point into v's own buffer, and the source and destination may overlap; such
// an aliasing range must stay inside v's reserved capacity or the call gives
// false and leaves v unchanged. Asserts on a NULL v.
bool cvector_append_array(cvec v, const void *arr_ptr, size_t elem_count);

// Appends every element of v_from to v_to (v_from can be v_to), so joining two
// vectors needs no loop. It gives false when memory runs out or the result is
// above ccol_max_elem_count. Asserts when either vector is NULL or their
// element sizes differ.
bool cvector_append_cvector(cvec v_to, cvec v_from);

// Gives the internal buffer, for code that expects a plain C array. Don't free
// it, and don't keep it across a call that can change the size of the vector.
// Asserts on a NULL v.
void *cvector_data_ptr(cvec v);

// Internal: Destroys *cv and sets it to NULL. The _scoped macros attach it
// with ccol__destructor, so the vector goes away at the end of its scope.
static inline void cvector__destroy_cleanup(cvec *cv) {
  if (cv && *cv) {
    cvector__destroy(*cv);
    *cv = NULL;
  }
}

// cvector_begin_iter(), the iterator of a vector, is declared in citerators.h.

// Gives the index of the first element equal to *elem, or ccol_invalid_size
// when nothing matches or elem is NULL. cmp decides equality (it gives 0 for
// equal elements); with a NULL cmp the comparison is memcmp over the element
// size, so padding bytes of a struct count. Asserts on a NULL v.
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc cmp);

// Declares a vector handle v whose elements have the given type, without
// creating the vector. The typed cvec_ macros learn the element type from this
// declaration, so declare v with it before you use them. v is uninitialized;
// create the vector with cvec_init() or cvec_init_mp() and free it with
// cvec_destroy(). It's a declaration.
#define cvec_declare(v, type)                                       \
  size_t *v##__ccol_key_type_var __attribute__((unused));           \
  __typeof__(type) *v##__ccol_val_type_var __attribute__((unused)); \
  cvec v

// Declares a vector handle v whose elements have the given type, set to NULL,
// for the typed cvec_ macros. Create the vector with cvec_init() or
// cvec_init_mp(). Whatever vector v holds is destroyed when v goes out of
// scope, on every way out, so don't destroy it yourself and don't return it.
// It's a declaration.
#define cvec_declare_scoped(v, type)                                       \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL;           \
  __typeof__(type) *v##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cvec v ccol__destructor(cvector__destroy_cleanup) = NULL

// Makes the typed cvec_ macros work on a vector v that was declared in another
// scope, such as a function parameter of type cvec. type must be the element
// type the vector was created with. It changes nothing about v itself. It's a
// declaration.
#define cvec_redeclare(v, type)                                  \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL; \
  __typeof__(type) *v##__ccol_val_type_var __attribute__((unused)) = NULL

// Creates an empty vector for a v declared with cvec_declare() or
// cvec_declare_scoped(), using malloc and free, and assigns it to v. The
// element size comes from the declared type, so you never pass sizeof. A
// failed creation prints the reason and ends the program. It's a statement.
#define cvec_init(v)                                                      \
  do {                                                                    \
    char *ccol__cvec_err = NULL;                                          \
    v = cvector_create(sizeof(*v##__ccol_val_type_var), &ccol__cvec_err); \
    if (!(v)) {                                                           \
      ccol__fatal_err("cvec_init('%s'): %s", #v,                          \
                      ccol__cvec_err ? ccol__cvec_err : "unknown error"); \
    }                                                                     \
  } while (0)

// Creates an empty vector for a v declared with cvec_declare() or
// cvec_declare_scoped() and assigns it to v. mprocs is the allocator table, or
// NULL for malloc and free; the vector keeps its own copy. The element size
// comes from the declared type. A failed creation (an allocator with a NULL
// function included) prints the reason and ends the program. It's a statement.
#define cvec_init_mp(v, mprocs)                                           \
  do {                                                                    \
    char *ccol__cvec_err = NULL;                                          \
    v = cvector_create_full(sizeof(*v##__ccol_val_type_var), (mprocs),    \
                            &ccol__cvec_err);                             \
    if (!(v)) {                                                           \
      ccol__fatal_err("cvec_init_mp('%s'): %s", #v,                       \
                      ccol__cvec_err ? ccol__cvec_err : "unknown error"); \
    }                                                                     \
  } while (0)

// Declares v and creates an empty vector of the given element type in it,
// using malloc and free; it's the usual way to start a vector for the typed
// cvec_ macros. A failed creation prints the reason and ends the program. Free
// the vector with cvec_destroy(). It's a declaration followed by a statement.
#define cvec_construct(v, type) \
  cvec_declare(v, type);        \
  cvec_init(v)

// Declares v and creates an empty vector of the given element type in it,
// using malloc and free, for the typed cvec_ macros. The vector is destroyed
// when v goes out of scope, on every way out, so don't destroy it yourself and
// don't return it. A failed creation prints the reason and ends the program.
#define cvec_construct_scoped(v, type) \
  cvec_declare_scoped(v, type);        \
  cvec_init(v)

// Declares v and creates an empty vector of the given element type in it, for
// the typed cvec_ macros. mprocs is the allocator table, or NULL for malloc and
// free; the vector keeps its own copy. A failed creation prints the reason and
// ends the program. Free the vector with cvec_destroy().
#define cvec_construct_mp(v, type, mprocs) \
  cvec_declare(v, type);                   \
  cvec_init_mp(v, mprocs)

// Declares v and creates an empty vector of the given element type in it, for
// the typed cvec_ macros. mprocs is the allocator table, or NULL for malloc and
// free; the vector keeps its own copy. The vector is destroyed when v goes out
// of scope, on every way out, so don't destroy it yourself and don't return
// it. A failed creation prints the reason and ends the program.
#define cvec_construct_mp_scoped(v, type, mprocs) \
  cvec_declare_scoped(v, type);                   \
  cvec_init_mp(v, mprocs)

// Frees the vector and its buffer and sets v to NULL. v must be a modifiable
// lvalue and is evaluated once; a NULL v does nothing. Memory that the
// elements point to isn't freed for you. It's a statement.
#define cvec_destroy(v) cvector_destroy(v)

// Internal: Names v's element type without top-level qualifiers, so the macros
// can write to a temporary of it even for a const element type. An array type
// stays as it is. It's a type, and it doesn't evaluate v.
#define cvec__unqual_elem_type(v) ccol__unqual_type(*v##__ccol_val_type_var)

// Internal: A constant expression that is true when v's element type is an
// array, which the macros must handle apart because C can't assign an array.
// The comma operator decays an array to a pointer and leaves every other type
// as it is. It doesn't evaluate v.
#define cvec__elem_is_array(v)             \
  (!__builtin_types_compatible_p(          \
      __typeof__(*v##__ccol_val_type_var), \
      __typeof__((void)0, *v##__ccol_val_type_var)))

// Internal: Gives elem for __typeof__ and sizeof on the array path of
// cvec__elem_load, and 0 for any other element type. It's needed because elem
// may be a bit-field there, which neither operator accepts; the value is never
// used.
#define cvec__elem_operand(v, elem) \
  __builtin_choose_expr(cvec__elem_is_array(v), (elem), 0)

// Internal: Declares tmp of v's element type and stores elem into it, with elem
// evaluated once; cvec_push and cvec_find need a typed copy so the value is
// converted and never reinterpreted. A plain element is assigned, as in
// `T tmp = elem;`. For an array element type elem must be an array or a string
// literal: tmp is zeroed and gets elem's bytes up to the smaller size. Both
// arms of each __builtin_choose_expr must compile for every element type,
// which is why arg is a const void * on the non-array path.
#define cvec__elem_load(v, elem, tmp, arg)                                  \
  _Static_assert(                                                           \
      !cvec__elem_is_array(v) ||                                            \
          !__builtin_types_compatible_p(                                    \
              __typeof__(cvec__elem_operand(v, elem)),                      \
              __typeof__((void)0, cvec__elem_operand(v, elem))),            \
      "a vector whose element type is an array takes an array or a string " \
      "literal as the element value");                                      \
  cvec__unqual_elem_type(v) tmp;                                            \
  __typeof__(__builtin_choose_expr(cvec__elem_is_array(v),                  \
                                   ((void)0, cvec__elem_operand(v, elem)),  \
                                   (const void *)0)) arg;                   \
  *__builtin_choose_expr(cvec__elem_is_array(v), &arg, &tmp) = (elem);      \
  __builtin_choose_expr(                                                    \
      cvec__elem_is_array(v),                                               \
      (void)(memset(&tmp, 0, sizeof(tmp)),                                  \
             memcpy(&tmp, (const void *)arg,                                \
                    sizeof(cvec__elem_operand(v, elem)) < sizeof(tmp)       \
                        ? sizeof(cvec__elem_operand(v, elem))               \
                        : sizeof(tmp))),                                    \
      (void)0)

// Appends new_elem to the end of v, doubling the capacity when it's full.
// new_elem is a value, not a pointer: Any expression (a literal, a computed
// value, a struct that a function returns), evaluated once and converted to
// v's element type by assignment, so a float pushed into an int vector
// truncates as in `int x = f;` and a type with no implicit conversion doesn't
// compile. For an array element type new_elem is an array or a string literal.
// new_elem may read from v's own buffer. With GCC 11 or later the stored copy
// of a struct has zero padding bytes, which cvec_find relies on. A failure
// (ccol_max_elem_count reached, or no memory) prints the reason and ends the
// program. It's a statement.
#define cvec_push(v, new_elem)                                       \
  ccol__cvec_push_impl(v, (new_elem),                                \
                       ccol__uniq(ccol__cvec_push_val, __COUNTER__), \
                       ccol__uniq(ccol__cvec_push_arg, __COUNTER__), \
                       ccol__uniq(ccol__cvec_push_r, __COUNTER__))

// Internal: The body of cvec_push. Its temporaries get unique names from the
// public macro so the public macros can nest.
#define ccol__cvec_push_impl(v, new_elem, cvec__push_val, cvec__push_arg, \
                             cvec__push_r)                                \
  do {                                                                    \
    cvec__elem_load(v, new_elem, cvec__push_val, cvec__push_arg);         \
    ccol__clear_padding(&cvec__push_val);                                 \
    ccol_retval cvec__push_r =                                            \
        cvector_push_back((v), (const void *)&cvec__push_val);            \
    if (cvec__push_r != ccol_success) {                                   \
      ccol__fatal_err("cvec_push('%s'): r: %d (%s)", #v, cvec__push_r,    \
                      ccol_retval_to_str(cvec__push_r));                  \
    }                                                                     \
  } while (0)

// Removes the last element of v and evaluates to it as a value of the element
// type. The capacity halves when less than 1/4 of it is in use. Popping from
// an empty vector prints an error and ends the program, so check cvec_size()
// first when the vector can be empty. An array element type is refused at
// compile time, since C can't give an array as a value; pop such an element
// into a buffer with cvector_pop_back().
#define cvec_pop(v)                                                   \
  ccol__cvec_pop_impl(v, ccol__uniq(ccol__cvec_pop_tmp, __COUNTER__), \
                      ccol__uniq(ccol__cvec_pop_r, __COUNTER__))

// Internal: The body of cvec_pop, a statement expression whose value is the
// popped element.
#define ccol__cvec_pop_impl(v, cvec__pop_tmp, cvec__pop_r)                 \
  ({                                                                       \
    _Static_assert(!cvec__elem_is_array(v),                                \
                   "cvec_pop cannot give an array element as a value; "    \
                   "pop it into a buffer with cvector_pop_back()");        \
    cvec__unqual_elem_type(v) cvec__pop_tmp;                               \
    ccol_retval cvec__pop_r = cvector_pop_back((v), &cvec__pop_tmp);       \
    if (cvec__pop_r != ccol_success) {                                     \
      ccol__fatal_err(                                                     \
          "cvec_pop('%s'): r: %d (%s)%s", #v, cvec__pop_r,                 \
          ccol_retval_to_str(cvec__pop_r),                                 \
          cvec__pop_r == ccol_container_empty ? "; vector is empty" : ""); \
    }                                                                      \
    cvec__pop_tmp;                                                         \
  })

// Internal: Gives ptr, or stops the program with an out-of-bounds message when
// it's NULL. It keeps cvec_at's diagnostic in one place, and reads the element
// count only on that failure path, so a good lookup costs just the cvector_at()
// call.
static inline __attribute__((always_inline)) void *cvec__at_checked(
    void *ptr, cvec v, const char *vec_name, size_t index) {
  if (__builtin_expect(!ptr, 0)) {
    ccol__fatal_err("cvec_at('%s'): index %lu out of bounds (size: %lu)",
                    vec_name, (unsigned long)index,
                    (unsigned long)cvector_size(v));
  }
  return ptr;
}

// Evaluates to the element at index as an lvalue of the element type, so you
// can read it or assign to it with no cast: cvec_at(v, 0) = 5. index is
// evaluated once, so cvec_at(v, i++) is safe. An out-of-range index prints the
// index and the size and ends the program; use cvec_at_ptr() to get NULL
// instead. The address of the element is valid until the size of v changes.
#define cvec_at(v, index) \
  ccol__cvec_at_impl(v, (index), ccol__uniq(ccol__cvec_at_index, __COUNTER__))

// Internal: The body of cvec_at. index goes into a local first, otherwise a
// side effect in it would happen twice.
#define ccol__cvec_at_impl(v, index, cvec__at_index)           \
  (*(__typeof__(*v##__ccol_val_type_var) *)({                  \
    size_t cvec__at_index = (index);                           \
    cvec__at_checked(cvector_at((v), cvec__at_index), (v), #v, \
                     cvec__at_index);                          \
  }))

// Evaluates to a pointer of the element type to the element at index, or NULL
// when index is out of range, for code that checks the index instead of
// ending the program. The pointer is valid until the size of v changes.
#define cvec_at_ptr(v, index) \
  ((__typeof__(*v##__ccol_val_type_var) *)(cvector_at((v), (index))))

// Evaluates to the number of elements in v, as a size_t. Asserts on a NULL v.
#define cvec_size(v) cvector_size((v))

// Removes every element of v and tries to shrink the capacity back to 4, so you
// can reuse the vector; when that shrink fails the capacity stays, but the
// count is 0 anyway. Memory that the elements point to isn't freed for you.
// Asserts on a NULL v.
#define cvec_reset(v) cvector_reset((v))

// Makes room in v for at least new_capacity_count elements, rounded up to a
// power of two, so the pushes up to that size don't reallocate. It never
// shrinks. new_capacity_count is evaluated once. A request above
// ccol_max_elem_count or a failed allocation prints an error and ends the
// program. It's a statement.
#define cvec_reserve(v, new_capacity_count)        \
  ccol__cvec_reserve_impl(v, (new_capacity_count), \
                          ccol__uniq(ccol__cvec_reserve_count, __COUNTER__))

// Internal: The body of cvec_reserve, which keeps the count in a local.
#define ccol__cvec_reserve_impl(v, new_capacity_count, cvec__reserve_count) \
  do {                                                                      \
    size_t cvec__reserve_count = (new_capacity_count);                      \
    if (!cvector_reserve((v), cvec__reserve_count)) {                       \
      ccol__fatal_err(                                                      \
          "cvec_reserve('%s'): failed to reserve %lu elements; out of "     \
          "memory?",                                                        \
          #v, (unsigned long)cvec__reserve_count);                          \
    }                                                                       \
  } while (0)

// Appends elem_count elements from the array arr_ptr to v with at most one
// reallocation. elem_count is evaluated once, and 0 does nothing. arr_ptr may
// point into v's own buffer as long as the range stays inside v's capacity. A
// NULL arr_ptr, an overflow, a result above ccol_max_elem_count or a failed
// allocation prints an error and ends the program. It's a statement.
#define cvec_append_array(v, arr_ptr, elem_count) \
  ccol__cvec_append_array_impl(                   \
      v, (arr_ptr), (elem_count),                 \
      ccol__uniq(ccol__cvec_append_array_count, __COUNTER__))

// Internal: The body of cvec_append_array, which keeps the count in a local.
#define ccol__cvec_append_array_impl(v, arr_ptr, elem_count,                \
                                     cvec__append_array_count)              \
  do {                                                                      \
    size_t cvec__append_array_count = (elem_count);                         \
    if (!cvector_append_array((v), (arr_ptr), cvec__append_array_count)) {  \
      ccol__fatal_err(                                                      \
          "cvec_append_array('%s'): failed to append %lu elements; out of " \
          "memory?",                                                        \
          #v, (unsigned long)cvec__append_array_count);                     \
    }                                                                       \
  } while (0)

// Appends every element of v_from to v_to; v_from may be v_to itself. A failed
// allocation or a result above ccol_max_elem_count prints an error and ends
// the program, and a NULL vector or different element sizes assert. It's a
// statement.
#define cvec_append_cvec(v_to, v_from)                                         \
  do {                                                                         \
    if (!cvector_append_cvector((v_to), (v_from))) {                           \
      ccol__fatal_err("cvec_append_cvec('%s' <- '%s'): out of memory?", #v_to, \
                      #v_from);                                                \
    }                                                                          \
  } while (0)

// Evaluates to the internal buffer of v as a void *, for code that expects a
// plain C array. Don't free it, and don't keep it across a call that can
// change the size of v. Asserts on a NULL v.
#define cvec_data_ptr(v) cvector_data_ptr((v))

// Internal: Sorts v in place with a stable mergesort over the buffer; the sort
// macros call it because they can't see inside the opaque struct. The
// temporary buffer comes from v's allocator. It gives false, with v unchanged,
// when that allocation fails.
bool cvector__sort(cvec v, ccol_comparison_proc comparison_proc);

// Sorts v in place with a stable mergesort and your comparator (O(n log n)
// time, O(n) space), for element types with no default order or when you want
// a different one. Each argument is evaluated once. A NULL v, a NULL
// comparator or a failed allocation stops the program; on failure v is left
// unsorted and unchanged. It's a statement.
#define cvec_sort_with(v, comparison_proc)                         \
  do {                                                             \
    __typeof__(v) cvec__sort_v = (v);                              \
    if (!cvec__sort_v) {                                           \
      ccol__fatal_err("cvec_sort_with('%s'): vector is NULL", #v); \
    }                                                              \
    ccol_comparison_proc cvec__sort_cmp = (comparison_proc);       \
    if (!cvec__sort_cmp) {                                         \
      ccol__fatal_err(                                             \
          "cvec_sort_with('%s'): comparison_proc is "              \
          "NULL; pass a real comparison procedure",                \
          #v);                                                     \
    }                                                              \
    if (!cvector__sort(cvec__sort_v, cvec__sort_cmp)) {            \
      ccol__fatal_err(                                             \
          "cvec_sort_with('%s'): out of memory; vector "           \
          "left unsorted",                                         \
          #v);                                                     \
    }                                                              \
  } while (0)

// Sorts v in place with a stable mergesort and the default order of its
// element type, so the common types need no comparator: Integers, floats (NaN
// last), enums, bool (false first) and char pointers (strcmp order, NULL
// first). A type with no default order (a struct, a union, another pointer, a
// char array) is a compile error that points you to
// cvec_sort_with(). A failed allocation of the temporary
// buffer prints an error and ends the program, with v unchanged.
#define cvec_sort(v)                                                         \
  do {                                                                       \
    _Static_assert(                                                          \
        csort__has_default_comparison_proc(*(v##__ccol_val_type_var)),       \
        "cvec_sort(" #v                                                      \
        "): this vector's element type has no default comparison procedure " \
        "(a struct, a union, a pointer to anything other than char and a "   \
        "fixed-size char array all have none). Sort this vector with "       \
        "cvec_sort_with(" #v                                                 \
        ", your_comparison_proc) and an explicit comparison procedure "      \
        "instead.");                                                         \
    ccol_comparison_proc ccol__cvec_sort_cmp =                               \
        csort_get_default_comparison_proc(*(v##__ccol_val_type_var));        \
    cvec_sort_with(v, ccol__cvec_sort_cmp);                                  \
  } while (0)

// Evaluates to the index of the first element of v equal to elem, or
// ccol_invalid_size when none matches. elem is a value, not a pointer: Any
// expression, evaluated once and converted to the element type. The comparison
// is the default order of the element type, so -0.0 equals 0.0 and a char * is
// compared as a string (don't use it on pointers to binary buffers). A type
// with no default order falls back to byte-wise equality, which works for
// padded structs only when both copies have zero padding (GCC 11 and later
// clears it here and in cvec_push). Asserts on a NULL v.
#define cvec_find(v, elem)                                              \
  ccol__cvec_find_impl(v, (elem),                                       \
                       ccol__uniq(ccol__cvec_find_needle, __COUNTER__), \
                       ccol__uniq(ccol__cvec_find_arg, __COUNTER__))

// Internal: The body of cvec_find, a statement expression whose value is the
// index.
#define ccol__cvec_find_impl(v, elem, cvec__find_needle, cvec__find_arg) \
  ({                                                                     \
    cvec__elem_load(v, elem, cvec__find_needle, cvec__find_arg);         \
    ccol__clear_padding(&cvec__find_needle);                             \
    cvector_find(                                                        \
        (v), (const void *)&cvec__find_needle,                           \
        csort_get_default_comparison_proc(*(v##__ccol_val_type_var)));   \
  })

#pragma GCC visibility pop
