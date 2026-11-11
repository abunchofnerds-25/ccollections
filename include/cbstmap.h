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

// An ordered map on an AVL tree. Insert, delete and lookup are O(log n), every
// walk is iterative with a fixed-size stack (no recursion, no heap), and an
// iterator gives the keys in ascending order. The default order is a signed or
// unsigned integer comparison, a numeric comparison for floating keys, a byte
// comparison for strings and memcmp for any other key type; you can pass your
// own comparator.
//
// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// The map is opaque and cbmap is the handle.
typedef struct cbstmap cbstmap;

typedef cbstmap *cbmap;

// Creates a map for keys of key_type, which picks the default order and the
// width a fixed-width key must have. mmgmt_procs is your allocator (NULL for
// malloc and free) and custom_comparison_proc your comparator (NULL for the
// default); a comparator gets pointers to the key bytes. On failure it gives
// NULL and, when err isn't NULL, an error string in *err.
//
// ccol_char compares as the platform's char; ccol_signed_char and the other
// signed types compare signed, the unsigned types and ccol_pointer unsigned.
// Floating keys compare by value, with NaN above every other key and equal to
// every NaN, so a NaN key can be found and deleted. ccol_string compares bytes.
// ccol_other_types (a struct, say) compares with memcmp, padding and pointer
// members included, so give a struct key a comparator.
cbmap cbstmap_create_full(ccol_data_type key_type,
                          ccol_memmgmt_procs *mmgmt_procs,
                          ccol_comparison_proc custom_comparison_proc,
                          char **err);

// Creates a map with the default allocator and the default order; the short
// form of cbstmap_create_full().
static inline __attribute__((always_inline)) cbmap
cbstmap_create(ccol_data_type key_type, char **err) {
  return cbstmap_create_full(key_type, NULL, NULL, err);
}

// Creates a map that allocates through mmgmt_procs and uses the default order;
// the short form of cbstmap_create_full().
static inline __attribute__((always_inline)) cbmap cbstmap_create_mp(
    ccol_data_type key_type, ccol_memmgmt_procs *mmgmt_procs, char **err) {
  return cbstmap_create_full(key_type, mmgmt_procs, NULL, err);
}

// Creates a map ordered by custom_comparison_proc with the default allocator;
// the short form of cbstmap_create_full(). The comparator decides only the
// order; key_type decides the key width and alignment.
static inline __attribute__((always_inline)) cbmap
cbstmap_create_ch(ccol_data_type key_type,
                  ccol_comparison_proc custom_comparison_proc, char **err) {
  return cbstmap_create_full(key_type, NULL, custom_comparison_proc, err);
}

// Gives the number of entries. Asserts on a NULL cbm.
size_t cbstmap_elem_count(cbmap cbm);

// Frees every node, key and value with an iterative post-order walk and leaves
// an empty map you can keep using. It gives ccol_success. Asserts on a NULL
// cbm.
ccol_retval cbstmap_reset(cbmap cbm);

// Copies the key and the value into the map. When the key is already there only
// the value is replaced, reallocated if its size changed, and it gives
// ccol_key_already_present. Other results: ccol_success, ccol_container_full at
// ccol_max_elem_count, ccol_not_enough_memory, and ccol_invalid_args for a NULL
// pair, a NULL ptr with a non-zero size, a key whose size doesn't match a
// fixed-width key type, or a key of size 0 on a ccol_string map (a string key
// counts its NUL). A size of 0 is fine for any value and for a ccol_other_types
// key. Asserts on a NULL cbm.
ccol_retval cbstmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair);

// Copies the value of key_pair into target_buf, whose size must equal the
// stored value's size exactly. It gives ccol_success, ccol_key_not_found, or
// ccol_invalid_args for a NULL key_pair, a NULL ptr with a non-zero size, a
// NULL target_buf with a non-zero size, a size mismatch, a key whose size
// doesn't match a fixed-width key type, or a size-0 key on a ccol_string map.
// Asserts on a NULL cbm.
ccol_retval cbstmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size);

// Fills your val_pair with the address and size of the stored value. You may
// read and change those bytes in place, but don't free them; to replace a value
// call cbstmap_insert_elem(). The pointer stays valid until the next insert or
// delete; a lookup never moves anything. It gives ccol_success,
// ccol_key_not_found, or ccol_invalid_args for the same key problems as
// cbstmap_get_elem_copy() or a NULL val_pair. On every failure a non-NULL
// val_pair gets {NULL, 0}. Asserts on a NULL cbm.
ccol_retval cbstmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 cmap_pair *val_pair);

// Internal: hands the string arm of cbmap_get_ptr() the accessor the map keeps
// for a value, whose ptr field is the stored char *. *val_accessor is NULL on
// failure. Use cbmap_get_ptr() instead.
ccol_retval cbstmap__get_val_accessor(cbmap cbm, const cmap_pair *key_pair,
                                      const cmap_pair **val_accessor);

// Removes key_pair, frees its key and value and rebalances. A node with two
// children is replaced by the smallest key on its right when the right side is
// at least as tall, else by the largest key on its left. It gives ccol_success,
// ccol_key_not_found, or ccol_invalid_args for the same key problems as
// cbstmap_get_elem_copy(). Asserts on a NULL cbm.
ccol_retval cbstmap_delete_elem(cbmap cbm, const cmap_pair *key_pair);

// Internal: frees an iterator, at the end of a walk or when a loop leaves early
// (ccol_iter_destroy()). cbstmap_begin_iter() is declared in citerators.h,
// because ccol_begin() needs all three containers' functions.
void cbstmap__iterator_destroy(cmap_iterator *iter);

// Internal: frees the map and everything in it. Use cbstmap_destroy() instead.
void cbstmap__destroy(cbmap cbm);

// Frees every node, key and value and sets cbm to NULL. cbm must be a
// modifiable lvalue and is evaluated once; NULL is fine.
#define cbstmap_destroy(cbm)  \
  ccol__cbstmap_destroy_impl( \
      cbm, ccol__uniq(ccol__cbstmap_destroy_slot, __COUNTER__))

// Internal: the body of cbstmap_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define ccol__cbstmap_destroy_impl(cbm, slot) \
  do {                                        \
    __typeof__(cbm) *slot = &(cbm);           \
    cbstmap__destroy(*slot);                  \
    *slot = NULL;                             \
  } while (0)

// Internal: destroys *cbm and sets it to NULL. It's the cleanup function the
// _scoped forms attach with ccol__destructor.
static inline void cbstmap__destroy_cleanup(cbmap *cbm) {
  if (cbm && *cbm) {
    cbstmap__destroy(*cbm);
    *cbm = NULL;
  }
}

// Declares the companion type variables for a map created in another scope, so
// the typed macros work on it there. An array key or value type is refused at
// compile time; wrap the array in a struct.
#define cbmap_redeclare(hm_name, KeyT, ValT)                               \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                           \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

// Declares a map variable and its companion type variables, all left
// uninitialized. Call one of the cbmap_init macros before use.
#define cbmap_declare(hm_name, KeyT, ValT)                                \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                          \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)); \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)); \
  cbmap hm_name

// cbmap_declare() for a map that is destroyed at the end of its scope; the
// handle starts as NULL.
#define cbmap_declare_scoped(hm_name, KeyT, ValT)                          \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                           \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) = \
      NULL;                                                                \
  cbmap hm_name ccol__destructor(cbstmap__destroy_cleanup) = NULL

// The cbmap_init macros create a declared map, taking the key type from the
// companion variable. The _mp form takes an allocator, _cc a comparator and
// _full both. Each stops the program when the creation fails.
#define cbmap_init(hm_name)                                                 \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL, \
        NULL, &ccol__cbmap_err);                                            \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_init_mp(hm_name, mmgmt_procs)                                 \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        (mmgmt_procs), NULL, &ccol__cbmap_err);                             \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_init_cc(hm_name, custom_comparison_proc)                      \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL, \
        (custom_comparison_proc), &ccol__cbmap_err);                        \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_init_full(hm_name, mmgmt_procs, custom_comparison_proc)       \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        (mmgmt_procs), (custom_comparison_proc), &ccol__cbmap_err);         \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// The cbmap_construct macros declare and create a map in one step, with the
// same _mp, _cc and _full variants as cbmap_init; the _scoped forms destroy the
// map at the end of its scope. Each stops the program on failure. A struct key
// gets the memcmp order, so use a _cc form for it.
#define cbmap_construct(hm_name, KeyT, ValT)                                \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL, \
        NULL, &ccol__cbmap_err);                                            \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_construct_scoped(hm_name, KeyT, ValT)                         \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name ccol__destructor(cbstmap__destroy_cleanup) = NULL;          \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL, \
        NULL, &ccol__cbmap_err);                                            \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_construct_mp(hm_name, KeyT, ValT, mmgmt_procs)                \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        (mmgmt_procs), NULL, &ccol__cbmap_err);                             \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_construct_mp_scoped(hm_name, KeyT, ValT, mmgmt_procs)         \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name ccol__destructor(cbstmap__destroy_cleanup) = NULL;          \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        (mmgmt_procs), NULL, &ccol__cbmap_err);                             \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_construct_cc(hm_name, KeyT, ValT, custom_comparison_proc)     \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL, \
        (custom_comparison_proc), &ccol__cbmap_err);                        \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_construct_cc_scoped(hm_name, KeyT, ValT, custom_comparison_proc) \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                               \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =     \
      NULL;                                                                    \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =     \
      NULL;                                                                    \
  cbmap hm_name ccol__destructor(cbstmap__destroy_cleanup) = NULL;             \
  do {                                                                         \
    char *ccol__cbmap_err = NULL;                                              \
    hm_name = cbstmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,    \
        (custom_comparison_proc), &ccol__cbmap_err);                           \
    if (!hm_name) {                                                            \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,           \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error");    \
    }                                                                          \
  } while (0)

#define cbmap_construct_full(hm_name, KeyT, ValT, mmgmt_procs,              \
                             custom_comparison_proc)                        \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        (mmgmt_procs), (custom_comparison_proc), &ccol__cbmap_err);         \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define cbmap_construct_full_scoped(hm_name, KeyT, ValT, mmgmt_procs,       \
                                    custom_comparison_proc)                 \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  cbmap hm_name ccol__destructor(cbstmap__destroy_cleanup) = NULL;          \
  do {                                                                      \
    char *ccol__cbmap_err = NULL;                                           \
    hm_name = cbstmap_create_full(                                          \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        (mmgmt_procs), (custom_comparison_proc), &ccol__cbmap_err);         \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create BST map '%s': %s", #hm_name,        \
                      ccol__cbmap_err ? ccol__cbmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Destroys the map and sets cbm to NULL; the typed name of cbstmap_destroy().
#define cbmap_destroy(cbm) cbstmap_destroy(cbm)

// Inserts or updates key with val and stops the program on any failure other
// than ccol_key_already_present. key and val are converted to the declared key
// and value types as a plain assignment would convert them, so a float never
// lands in an int map as its bit pattern; a char pointer type keeps the type of
// the expression, so a const char * needs no cast. A NULL char pointer key or
// value isn't a string and stops the program with ccol_invalid_args. Any
// expression is fine and each one is evaluated once.
#define cbmap_insert(hm_name, key, ccol__cbmap_val)                            \
  ccol__cbmap_insert_impl(hm_name, (key), (ccol__cbmap_val),                   \
                          ccol__uniq(ccol__cbmap_insert_key_tmp, __COUNTER__), \
                          ccol__uniq(ccol__cbmap_insert_val_tmp, __COUNTER__), \
                          ccol__uniq(ccol__cbmap_insert_kp, __COUNTER__),      \
                          ccol__uniq(ccol__cbmap_insert_vp, __COUNTER__),      \
                          ccol__uniq(ccol__cbmap_insert_r, __COUNTER__))

// Internal: the body of cbmap_insert(). Its temporaries come from ccol__uniq()
// so it can nest in any public macro. They have the declared types (through
// ccol__declared_or_own_type()), so a narrower or wider expression is converted
// and the node always holds sizeof(ValT) bytes, and they live for the whole
// call, which ccol__populate_cmap_pair() needs. The key's padding is cleared
// because the order is byte-wise for struct keys. A pair with a NULL ptr can
// only come from a NULL char pointer and is refused before the raw call.
#define ccol__cbmap_insert_impl(hm_name, key, ccol__cbmap_val,                 \
                                ccol__cbmap_key_tmp, ccol__cbmap_val_tmp,      \
                                ccol__cbmap_kp, ccol__cbmap_vp, ccol__cbmap_r) \
  do {                                                                         \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        ccol__cbmap_key_tmp = (key);                                           \
    ccol__clear_padding(&ccol__cbmap_key_tmp);                                 \
    ccol__declared_or_own_type(hm_name##__ccol_val_type_var,                   \
                               (ccol__cbmap_val)) ccol__cbmap_val_tmp =        \
        (ccol__cbmap_val);                                                     \
    cmap_pair *ccol__cbmap_kp = &(cmap_pair){};                                \
    cmap_pair *ccol__cbmap_vp = &(cmap_pair){};                                \
    ccol__populate_cmap_pair(ccol__cbmap_kp, ccol__cbmap_key_tmp);             \
    ccol__populate_cmap_pair(ccol__cbmap_vp, ccol__cbmap_val_tmp);             \
    ccol_retval ccol__cbmap_r =                                                \
        (!ccol__cbmap_kp->ptr || !ccol__cbmap_vp->ptr)                         \
            ? ccol_invalid_args                                                \
            : cbstmap_insert_elem(hm_name, ccol__cbmap_kp, ccol__cbmap_vp);    \
    if (ccol__cbmap_r != ccol_success &&                                       \
        ccol__cbmap_r != ccol_key_already_present) {                           \
      ccol__dump_key_to_stderr(ccol__cbmap_kp->ptr, ccol__cbmap_kp->size);     \
      ccol__fatal_err("cbmap_insert('%s'): r: %d (%s)", #hm_name,              \
                      ccol__cbmap_r, ccol_retval_to_str(ccol__cbmap_r));       \
    }                                                                          \
  } while (0)

// Removes key and gives the ccol_retval of cbstmap_delete_elem(); it doesn't
// stop the program on ccol_key_not_found. A NULL char pointer key gives
// ccol_invalid_args.
#define cbmap_remove(hm_name, key)                                             \
  ccol__cbmap_remove_impl(hm_name, (key),                                      \
                          ccol__uniq(ccol__cbmap_remove_key_tmp, __COUNTER__), \
                          ccol__uniq(ccol__cbmap_remove_kp, __COUNTER__),      \
                          ccol__uniq(ccol__cbmap_remove_r, __COUNTER__))

// Internal: the body of cbmap_remove(). The key goes through the declared key
// type, as in cbmap_insert().
#define ccol__cbmap_remove_impl(hm_name, key, ccol__cbmap_key_tmp,           \
                                ccol__cbmap_kp, ccol__cbmap_r)               \
  ({                                                                         \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))          \
        ccol__cbmap_key_tmp = (key);                                         \
    ccol__clear_padding(&ccol__cbmap_key_tmp);                               \
    cmap_pair *ccol__cbmap_kp = &(cmap_pair){};                              \
    ccol__populate_cmap_pair(ccol__cbmap_kp, ccol__cbmap_key_tmp);           \
    ccol_retval ccol__cbmap_r =                                              \
        !ccol__cbmap_kp->ptr ? ccol_invalid_args                             \
                             : cbstmap_delete_elem(hm_name, ccol__cbmap_kp); \
    ccol__cbmap_r;                                                           \
  })

// Gives the value of key, or for a string value the stored char *. It stops the
// program when the key is missing, when the stored size doesn't match the
// declared value type, and for a NULL char pointer key.
#define cbmap_get(hm_name, key)                                          \
  ccol__cbmap_get_impl(hm_name, (key),                                   \
                       ccol__uniq(ccol__cbmap_get_key_tmp, __COUNTER__), \
                       ccol__uniq(ccol__cbmap_get_val, __COUNTER__),     \
                       ccol__uniq(ccol__cbmap_get_kp, __COUNTER__),      \
                       ccol__uniq(ccol__cbmap_get_vp, __COUNTER__),      \
                       ccol__uniq(ccol__cbmap_get_r, __COUNTER__))

// Internal: the body of cbmap_get(). A key stored through the declared-type
// conversion is found only by a lookup that makes the same conversion.
#define ccol__cbmap_get_impl(hm_name, key, ccol__cbmap_key_tmp,               \
                             ccol__cbmap_val, ccol__cbmap_kp, ccol__cbmap_vp, \
                             ccol__cbmap_r)                                   \
  ({                                                                          \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))           \
        ccol__cbmap_key_tmp = (key);                                          \
    ccol__clear_padding(&ccol__cbmap_key_tmp);                                \
    __typeof__(*hm_name##__ccol_val_type_var) *ccol__cbmap_val = NULL;        \
    cmap_pair *ccol__cbmap_kp = &(cmap_pair){};                               \
    cmap_pair ccol__cbmap_vp = {NULL, 0};                                     \
    ccol__populate_cmap_pair(ccol__cbmap_kp, ccol__cbmap_key_tmp);            \
    ccol_retval ccol__cbmap_r =                                               \
        !ccol__cbmap_kp->ptr                                                  \
            ? ccol_invalid_args                                               \
            : cbstmap_get_elem_ref(hm_name, ccol__cbmap_kp, &ccol__cbmap_vp); \
    if (ccol__cbmap_r != ccol_success) {                                      \
      ccol__dump_key_to_stderr(ccol__cbmap_kp->ptr, ccol__cbmap_kp->size);    \
      ccol__fatal_err("cbmap_get('%s'): r: %d (%s)", #hm_name, ccol__cbmap_r, \
                      ccol_retval_to_str(ccol__cbmap_r));                     \
    }                                                                         \
    if (ccol__is_char_ptr(*hm_name##__ccol_val_type_var)) {                   \
      ccol__cbmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)&(ccol__cbmap_vp.ptr); \
    } else if (ccol__cbmap_vp.size != sizeof(*ccol__cbmap_val)) {             \
      ccol__fatal_err(                                                        \
          "cbmap_get('%s'): value size mismatch - stored: %lu bytes, "        \
          "requested: %lu bytes; wrong type or missing cbmap_redeclare()?",   \
          #hm_name, (unsigned long)ccol__cbmap_vp.size,                       \
          (unsigned long)sizeof(*ccol__cbmap_val));                           \
    } else {                                                                  \
      ccol__cbmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)(ccol__cbmap_vp.ptr);  \
    }                                                                         \
    *ccol__cbmap_val;                                                         \
  })

// Internal: gives p unchanged, except for a map with string values, where the
// target becomes const. The only char * object the map holds for a value is the
// ptr field of its accessor, and storing another pointer there would leave the
// accessor's size describing the old string; later reads would then overrun the
// new buffer. You can read the char * and edit the string bytes within its
// length; cbmap_insert() replaces a string.
#define ccol__cbmap_value_ptr_result(hm_name, p)                               \
  _Generic(*hm_name##__ccol_val_type_var,                                      \
      char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),          \
      const char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),    \
      signed char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),   \
      const signed char *: (__typeof__(*hm_name##__ccol_val_type_var)          \
                                const *)(p),                                   \
      unsigned char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p), \
      const unsigned char *: (__typeof__(*hm_name##__ccol_val_type_var)        \
                                  const *)(p),                                 \
      default: (p))

// Gives a pointer to the value of key, or NULL when the key is missing or is a
// NULL char pointer. You can change the value in place until the next insert or
// delete. For a string value the result is char *const *: read the string and
// edit its bytes within the stored length, but replace it only with
// cbmap_insert(). It stops the program when the stored size doesn't match the
// declared value type.
#define cbmap_get_ptr(hm_name, key)                                         \
  ccol__cbmap_get_ptr_impl(                                                 \
      hm_name, (key), ccol__uniq(ccol__cbmap_get_ptr_key_tmp, __COUNTER__), \
      ccol__uniq(ccol__cbmap_get_ptr_val, __COUNTER__),                     \
      ccol__uniq(ccol__cbmap_get_ptr_kp, __COUNTER__),                      \
      ccol__uniq(ccol__cbmap_get_ptr_vp, __COUNTER__),                      \
      ccol__uniq(ccol__cbmap_get_ptr_acc, __COUNTER__),                     \
      ccol__uniq(ccol__cbmap_get_ptr_r, __COUNTER__))

// Internal: the body of cbmap_get_ptr(). The value pair has no initializer
// because the lookup fills it on every path and it's read only after a success.
// For a string value the result points at the accessor's ptr field; the cast
// through uintptr_t drops the library's const without a -Wcast-qual warning in
// the caller's build, and the cast to the type of
// ccol__cbmap_value_ptr_result() adds const back.
#define ccol__cbmap_get_ptr_impl(                                              \
    hm_name, key, ccol__cbmap_key_tmp, ccol__cbmap_val, ccol__cbmap_kp,        \
    ccol__cbmap_vp, ccol__cbmap_acc, ccol__cbmap_r)                            \
  ({                                                                           \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        ccol__cbmap_key_tmp = (key);                                           \
    ccol__clear_padding(&ccol__cbmap_key_tmp);                                 \
    __typeof__(*hm_name##__ccol_val_type_var) *ccol__cbmap_val = NULL;         \
    cmap_pair *ccol__cbmap_kp = &(cmap_pair){};                                \
    cmap_pair ccol__cbmap_vp;                                                  \
    const cmap_pair *ccol__cbmap_acc = NULL;                                   \
    ccol_retval ccol__cbmap_r;                                                 \
    ccol__populate_cmap_pair(ccol__cbmap_kp, ccol__cbmap_key_tmp);             \
    if (ccol__cbmap_kp->ptr &&                                                 \
        ccol__is_char_ptr(*hm_name##__ccol_val_type_var)) {                    \
      ccol__cbmap_r = cbstmap__get_val_accessor(hm_name, ccol__cbmap_kp,       \
                                                &ccol__cbmap_acc);             \
      if (ccol__cbmap_r == ccol_success) {                                     \
        ccol__cbmap_val = (__typeof__(*hm_name##__ccol_val_type_var)           \
                               *)(uintptr_t)&ccol__cbmap_acc->ptr;             \
      }                                                                        \
    } else if (ccol__cbmap_kp->ptr) {                                          \
      ccol__cbmap_r =                                                          \
          cbstmap_get_elem_ref(hm_name, ccol__cbmap_kp, &ccol__cbmap_vp);      \
      if (ccol__cbmap_r == ccol_success) {                                     \
        if (ccol__cbmap_vp.size != sizeof(*ccol__cbmap_val)) {                 \
          ccol__fatal_err(                                                     \
              "cbmap_get_ptr('%s'): value size mismatch - stored: %lu "        \
              "bytes, requested: %lu bytes; wrong type or missing "            \
              "cbmap_redeclare()?",                                            \
              #hm_name, (unsigned long)ccol__cbmap_vp.size,                    \
              (unsigned long)sizeof(*ccol__cbmap_val));                        \
        }                                                                      \
        ccol__cbmap_val =                                                      \
            (__typeof__(*hm_name##__ccol_val_type_var) *)(ccol__cbmap_vp.ptr); \
      }                                                                        \
    }                                                                          \
    (__typeof__(ccol__cbmap_value_ptr_result(                                  \
        hm_name,                                                               \
        (__typeof__(*hm_name##__ccol_val_type_var) *)0)))(ccol__cbmap_val);    \
  })

// cbmap_size() evaluates to the number of entries and cbmap_reset() empties the
// map; the typed names of cbstmap_elem_count() and cbstmap_reset().
#define cbmap_size(cbm) cbstmap_elem_count((cbm))

#define cbmap_reset(cbm) cbstmap_reset((cbm))

#pragma GCC visibility pop
