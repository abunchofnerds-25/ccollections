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

// Creates an ordered map for keys of key_type with the default allocator and
// the default order for that type (integers and floating keys by value,
// strings by bytes, any other type with memcmp). key_type also sets the width
// a fixed-width key must have. Returns the map, or NULL with an error string
// in *err when err isn't NULL (never free the string). Destroy it with
// cbstmap_destroy().
static inline __attribute__((always_inline)) cbmap
cbstmap_create(ccol_data_type key_type, char **err) {
  return cbstmap_create_full(key_type, NULL, NULL, err);
}

// Creates an ordered map for keys of key_type that takes all its memory from
// mmgmt_procs, your allocator table (NULL for malloc and free), and uses the
// default order for key_type. The table must stay valid until the map is
// destroyed. Returns the map, or NULL with an error string in *err when err
// isn't NULL (never free the string). Destroy it with cbstmap_destroy().
static inline __attribute__((always_inline)) cbmap cbstmap_create_mp(
    ccol_data_type key_type, ccol_memmgmt_procs *mmgmt_procs, char **err) {
  return cbstmap_create_full(key_type, mmgmt_procs, NULL, err);
}

// Creates an ordered map whose keys are ordered by custom_comparison_proc,
// with the default allocator. The comparator gets pointers to the bytes of two
// keys and returns a negative, zero or positive int; it decides only the
// order, while key_type decides the key width and alignment. Returns the map,
// or NULL with an error string in *err when err isn't NULL (never free the
// string). Destroy it with cbstmap_destroy().
static inline __attribute__((always_inline)) cbmap
cbstmap_create_ch(ccol_data_type key_type,
                  ccol_comparison_proc custom_comparison_proc, char **err) {
  return cbstmap_create_full(key_type, NULL, custom_comparison_proc, err);
}

// Returns the number of key-value pairs in the map. Asserts on a NULL cbm.
size_t cbstmap_size(cbmap cbm);

// Removes and frees every key and value and leaves an empty map that you can
// keep using. It allocates nothing and returns ccol_success. Asserts on a NULL
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

// Finds the value of key_pair without copying it: Fills your val_pair with the
// address and size of the stored value. You may read and change those bytes in
// place, but don't free them; to replace a value call cbstmap_insert_elem().
// The pointer stays valid until the next insert or delete; a lookup never
// moves anything. Returns ccol_success, ccol_key_not_found, or
// ccol_invalid_args for a NULL key_pair or val_pair, a NULL ptr with a
// non-zero size, a key whose size doesn't match a fixed-width key type, or a
// size-0 key on a ccol_string map. On every failure a non-NULL val_pair gets
// {NULL, 0}. Asserts on a NULL cbm.
ccol_retval cbstmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 cmap_pair *val_pair);

// Internal: Hands the string arm of cbmap_get_ptr() the accessor the map keeps
// for a value, whose ptr field is the stored char *. *val_accessor is NULL on
// failure. Use cbmap_get_ptr() instead.
ccol_retval cbstmap__get_val_accessor(cbmap cbm, const cmap_pair *key_pair,
                                      const cmap_pair **val_accessor);

// Removes the entry of key_pair and frees the map's copies of its key and
// value. Returns ccol_success, ccol_key_not_found, or ccol_invalid_args for a
// NULL key_pair, a NULL ptr with a non-zero size, a key whose size doesn't
// match a fixed-width key type, or a size-0 key on a ccol_string map. Treat
// pointers into the map as invalid after a delete. Asserts on a NULL cbm.
ccol_retval cbstmap_remove_elem(cbmap cbm, const cmap_pair *key_pair);

// Internal: Frees an iterator, at the end of a walk or when a loop leaves early
// (ccol_iter_destroy()). cbstmap_begin_iter() is declared in citerators.h,
// because ccol_begin() needs all three containers' functions.
void cbstmap__iterator_destroy(cmap_iterator *iter);

// Internal: Frees the map and everything in it. Use cbstmap_destroy() instead.
void cbstmap__destroy(cbmap cbm);

// Destroys the map: Frees every node, key and value, and sets cbm to NULL so
// the handle can't be used after the free. cbm must be a modifiable lvalue and
// is evaluated once; a NULL handle does nothing. Pointers that the map gave
// you are invalid afterwards.
#define cbstmap_destroy(cbm)  \
  ccol__cbstmap_destroy_impl( \
      cbm, ccol__uniq(ccol__cbstmap_destroy_slot, __COUNTER__))

// Internal: The body of cbstmap_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define ccol__cbstmap_destroy_impl(cbm, slot) \
  do {                                        \
    __typeof__(cbm) *slot = &(cbm);           \
    cbstmap__destroy(*slot);                  \
    *slot = NULL;                             \
  } while (0)

// Internal: Destroys *cbm and sets it to NULL. It's the cleanup function the
// _scoped forms attach with ccol__destructor.
static inline void cbstmap__destroy_cleanup(cbmap *cbm) {
  if (cbm && *cbm) {
    cbstmap__destroy(*cbm);
    *cbm = NULL;
  }
}

// Makes the typed cbmap_* macros usable on a map handle that was created in
// another scope, such as a function parameter or a struct field. It declares
// only the key and value type information that those macros read; it creates
// and destroys nothing. KeyT and ValT must be the types the map was created
// with. An array key or value type is refused at compile time; wrap the array
// in a struct.
#define cbmap_redeclare(hm_name, KeyT, ValT)                               \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                           \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

// Declares an ordered map handle hm_name with key type KeyT and value type
// ValT, without creating a map. The handle is left uninitialized: Create the
// map with cbmap_init(), cbmap_init_mp(), cbmap_init_cc() or cbmap_init_full()
// before any other cbmap_* macro, and destroy it with cbmap_destroy(). An array
// key or value type is refused at compile time; wrap it in a struct.
#define cbmap_declare(hm_name, KeyT, ValT)                                \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                          \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)); \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)); \
  cbmap hm_name

// Declares an ordered map handle hm_name with key type KeyT and value type
// ValT, set to NULL, without creating a map. Create it later with cbmap_init()
// or another cbmap_init_* macro. When the variable goes out of scope the map is
// destroyed by itself; cbmap_destroy() before that is allowed and leaves NULL
// behind. An array key or value type is refused at compile time.
#define cbmap_declare_scoped(hm_name, KeyT, ValT)                          \
  ccol__refuse_array_types("cbmap", KeyT, ValT);                           \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) = \
      NULL;                                                                \
  cbmap hm_name ccol__destructor(cbstmap__destroy_cleanup) = NULL

// Creates the map for a handle that cbmap_declare() or cbmap_declare_scoped()
// declared, with the default allocator and the default order for the declared
// key type. Ends the program with a fatal error when the map can't be created.
// A struct key gets a memcmp order, so give it a comparator with
// cbmap_init_cc().
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

// Creates the map for a handle that cbmap_declare() or cbmap_declare_scoped()
// declared, taking all its memory from mmgmt_procs, your allocator table (NULL
// for the default). The table must stay valid until the map is destroyed. Keys
// get the default order for the declared key type. Ends the program with a
// fatal error when the map can't be created.
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

// Creates the map for a handle that cbmap_declare() or cbmap_declare_scoped()
// declared, with keys ordered by custom_comparison_proc and the default
// allocator. The comparator gets pointers to the bytes of two keys and returns
// a negative, zero or positive int. A struct key needs one. Ends the program
// with a fatal error when the map can't be created.
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

// Creates the map for a handle that cbmap_declare() or cbmap_declare_scoped()
// declared. mmgmt_procs is your allocator table (NULL for the default; it must
// outlive the map) and custom_comparison_proc orders the keys (NULL for the
// default order of the declared key type); it gets pointers to the bytes of
// two keys and returns a negative, zero or positive int. Ends the program with
// a fatal error when the map can't be created.
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map with the default allocator and the default order for
// KeyT. Ends the program with a fatal error when the map can't be created.
// Destroy it with cbmap_destroy(). A struct key gets a memcmp order, so use
// cbmap_construct_cc() for it. An array key or value type is refused at
// compile time.
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map with the default allocator and the default order for
// KeyT. The map is destroyed by itself when hm_name goes out of scope. Ends the
// program with a fatal error when the map can't be created. A struct key gets
// a memcmp order, so use cbmap_construct_cc_scoped() for it.
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map, taking all its memory from mmgmt_procs, your allocator
// table (NULL for the default). The table must stay valid until the map is
// destroyed. Keys get the default order for KeyT. Ends the program with a
// fatal error when the map can't be created. Destroy it with cbmap_destroy().
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map, taking all its memory from mmgmt_procs, your allocator
// table (NULL for the default). The map is destroyed by itself when hm_name
// goes out of scope, and the table must stay valid until then. Keys get the
// default order for KeyT. Ends the program with a fatal error when the map
// can't be created.
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map with keys ordered by custom_comparison_proc. The
// comparator gets pointers to the bytes of two keys and returns a negative,
// zero or positive int. Ends the program with a fatal error when the map can't
// be created. Destroy it with cbmap_destroy().
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map with keys ordered by custom_comparison_proc, which gets
// pointers to the bytes of two keys and returns a negative, zero or positive
// int. The map is destroyed by itself when hm_name goes out of scope. Ends the
// program with a fatal error when the map can't be created.
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map. mmgmt_procs is your allocator table (NULL for the
// default; it must outlive the map) and custom_comparison_proc orders the keys
// (NULL for the default order of KeyT); it gets pointers to the bytes of two
// keys and returns a negative, zero or positive int. Ends the program with a
// fatal error when the map can't be created. Destroy it with cbmap_destroy().
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

// Declares an ordered map handle hm_name for key type KeyT and value type ValT
// and creates the map; it is destroyed by itself when hm_name goes out of
// scope. mmgmt_procs is your allocator table (NULL for the default; it must
// stay valid until then) and custom_comparison_proc orders the keys (NULL for
// the default order of KeyT); it gets pointers to the bytes of two keys and
// returns a negative, zero or positive int. Ends the program with a fatal
// error when the map can't be created.
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

// Destroys the map: Frees every node, key and value, and sets cbm to NULL so
// the handle can't be used after the free. cbm must be a modifiable lvalue and
// is evaluated once; a NULL handle does nothing. Pointers that the map gave
// you (a char * from cbmap_get(), say) are invalid afterwards.
#define cbmap_destroy(cbm) cbstmap_destroy(cbm)

// Inserts key with its value, or replaces the value when the key is already
// there; the map stores its own copies, a string included. It ends the program
// with a fatal error on any failure, such as lack of memory. key and val are
// converted to the declared key and value types as a plain assignment would
// convert them, so a float never lands in an int map as its bit pattern; a
// char pointer type keeps the type of the expression, so a const char * needs
// no cast. A NULL char pointer key or value isn't a string and stops the
// program with ccol_invalid_args. Any expression is fine and each one is
// evaluated once.
#define cbmap_insert(hm_name, key, ccol__cbmap_val)                            \
  ccol__cbmap_insert_impl(hm_name, (key), (ccol__cbmap_val),                   \
                          ccol__uniq(ccol__cbmap_insert_key_tmp, __COUNTER__), \
                          ccol__uniq(ccol__cbmap_insert_val_tmp, __COUNTER__), \
                          ccol__uniq(ccol__cbmap_insert_kp, __COUNTER__),      \
                          ccol__uniq(ccol__cbmap_insert_vp, __COUNTER__),      \
                          ccol__uniq(ccol__cbmap_insert_r, __COUNTER__))

// Internal: The body of cbmap_insert(). Its temporaries come from ccol__uniq()
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

// Removes key and its value from the map and frees the map's copies of both.
// Evaluates to ccol_success, ccol_key_not_found, or ccol_invalid_args for a
// NULL char pointer key. It never ends the program, because a missing key is
// an ordinary outcome. The key is converted to the declared key type, as
// cbmap_insert() converts it.
#define cbmap_remove(hm_name, key)                                             \
  ccol__cbmap_remove_impl(hm_name, (key),                                      \
                          ccol__uniq(ccol__cbmap_remove_key_tmp, __COUNTER__), \
                          ccol__uniq(ccol__cbmap_remove_kp, __COUNTER__),      \
                          ccol__uniq(ccol__cbmap_remove_r, __COUNTER__))

// Internal: The body of cbmap_remove(). The key goes through the declared key
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
                             : cbstmap_remove_elem(hm_name, ccol__cbmap_kp); \
    ccol__cbmap_r;                                                           \
  })

// Evaluates to a copy of the value stored for key, for a key you know is
// there. For a string value it gives the map's own char *, which stays valid
// until that key is replaced or removed or the map is destroyed; don't free
// it. Ends the program with a fatal error when the key is missing, when the
// stored size doesn't match the declared value type, and for a NULL char
// pointer key. Use cbmap_get_ptr() for a lookup that may miss.
#define cbmap_get(hm_name, key)                                          \
  ccol__cbmap_get_impl(hm_name, (key),                                   \
                       ccol__uniq(ccol__cbmap_get_key_tmp, __COUNTER__), \
                       ccol__uniq(ccol__cbmap_get_val, __COUNTER__),     \
                       ccol__uniq(ccol__cbmap_get_kp, __COUNTER__),      \
                       ccol__uniq(ccol__cbmap_get_vp, __COUNTER__),      \
                       ccol__uniq(ccol__cbmap_get_r, __COUNTER__))

// Internal: The body of cbmap_get(). A key stored through the declared-type
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

// Internal: Gives p unchanged, except for a map with string values, where the
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

// Evaluates to a pointer to the value stored for key, or NULL when the key is
// missing or is a NULL char pointer; use it for a lookup that may miss or that
// changes the value in place. The pointer is valid until the next insert or
// delete. For a string value the result is char *const *: Read the string and
// edit its bytes within the stored length, but replace it only with
// cbmap_insert(). Ends the program with a fatal error when the stored size
// doesn't match the declared value type.
#define cbmap_get_ptr(hm_name, key)                                         \
  ccol__cbmap_get_ptr_impl(                                                 \
      hm_name, (key), ccol__uniq(ccol__cbmap_get_ptr_key_tmp, __COUNTER__), \
      ccol__uniq(ccol__cbmap_get_ptr_val, __COUNTER__),                     \
      ccol__uniq(ccol__cbmap_get_ptr_kp, __COUNTER__),                      \
      ccol__uniq(ccol__cbmap_get_ptr_vp, __COUNTER__),                      \
      ccol__uniq(ccol__cbmap_get_ptr_acc, __COUNTER__),                     \
      ccol__uniq(ccol__cbmap_get_ptr_r, __COUNTER__))

// Internal: The body of cbmap_get_ptr(). The value pair has no initializer
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

// Evaluates to the number of key-value pairs in the map, as a size_t. Asserts
// on a NULL handle.
#define cbmap_size(cbm) cbstmap_size((cbm))

// Removes and frees every key and value and leaves an empty map that you can
// keep using. Evaluates to ccol_success. Asserts on a NULL handle.
#define cbmap_reset(cbm) cbstmap_reset((cbm))

#pragma GCC visibility pop
