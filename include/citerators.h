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
 * cbstmap.h, chashmap.h and cvector.h include this header for you, so
 * including any one container header already gives you the whole unified
 * iterator API.
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

/* Everything this header declares from here to the end belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. The library is built with -fvisibility=hidden,
 * so a function or an object outside these blocks stays internal: it isn't
 * in the dynamic symbol table, an application linking against the library
 * can't interpose it, and a symbol of the same name in that application
 * can't collide with it. */
#pragma GCC visibility push(default)

/* Forward declarations. The _Generic type match only needs a pointer to an
 * incomplete struct, and so do the begin_iter prototypes below; each
 * container's header has the full struct definition. The prototypes are
 * here and nowhere else because ccol_begin() names all three in every
 * translation unit that uses it, and a second declaration in a container's
 * header would fail a build with -Wredundant-decls -Werror. */
struct cvector;
struct chashmap;
struct cbinarymap;

/**
 * @brief Create an iterator positioned at the first element.
 *
 * Gives a @c cmap_iterator* whose @c key_pair->ptr points to the internal
 * index field and whose @c val_pair->ptr points straight into the vector's
 * buffer. The @c _direct_ptr flag is set to true, so the unified accessor
 * macros (@c ccol_iter_key_ptr and @c ccol_iter_val_ptr) skip the map SSO
 * path and give typed pointers directly into the buffer.
 *
 * @param v    Vector to iterate
 * @param err  Optional pointer that receives an error string on failure
 *
 * @return Pointer to a @c cmap_iterator, or NULL when the vector is empty or
 *         when the allocation fails.
 *
 * @note ccol_iter_next() destroys the iterator when it reaches the
 *       end. Call ccol_iter_destroy() to stop before the end.
 * @note Changing the vector during the iteration invalidates the iterator.
 * @note A NULL v is treated like an empty vector: you get NULL, not an
 * error. This is on purpose and it's the same NULL tolerance that
 * chashmap_begin_iter() and cbmap_begin_iter() have. A container field can
 * stay uninitialized because nothing was ever inserted into it, and the
 * caller can iterate such a field directly without a NULL guard of its own.
 *
 * @see ccol_begin
 * @see ccol_iter_next
 */
cmap_iterator *cvector_begin_iter(struct cvector *v, char **err);

/**
 * @brief Start an iteration over the hash map
 *
 * Makes an iterator that points at the first element. A separate-chaining
 * map iterates in reverse insertion order through its doubly-linked list,
 * and an open-addressing map iterates in slot order.
 *
 * @param chm The hash map to iterate over
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the iterator. NULL comes back when the map is empty or
 * when an allocation fails
 *
 * @note Separate chaining: the iteration goes in reverse insertion order, so
 *       the element inserted last comes first. It walks the doubly-linked
 *       list
 * @note Open-addressing: the iteration goes in slot order, not in insertion
 * order
 * @note Destroy the iterator with ccol_iter_destroy() to stop early. It
 * also destroys itself at the end of the iteration
 * @note Changing the map during the iteration makes the iterator invalid
 * @note A NULL chm is treated like an empty map: you get NULL, and that
 * isn't an error. It's on purpose, because a map field that the program
 * only creates when it needs it can stay uninitialized while nothing goes
 * into it, and the caller can iterate such a field directly without a NULL
 * guard of its own first
 * @note NULL for an empty map is not an error
 *
 * @see ccol_begin (macro wrapper)
 * @see ccol_iter_next
 * @see ccol_iter_destroy
 */
cmap_iterator *chashmap_begin_iter(struct chashmap *chm, char **err);

/**
 * @brief Begin in-order iteration over the BST map
 *
 * Creates an iterator at the leftmost node, which holds the smallest key.
 * The iterator visits the keys in ascending sorted order by walking the
 * tree in order (left, root, right).
 *
 * @param cbm BST map to iterate over
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the iterator, or NULL if the map is empty or if an
 * allocation fails
 *
 * @note The keys come out in sorted order, from an in-order walk
 * @note The iterator keeps its path through the tree on a stack
 * @note The iterator destroys itself at the end of the walk; to stop early,
 * destroy it with ccol_iter_destroy()
 * @note Changing the map during the walk makes the iterator invalid
 * @note A NULL cbm is treated like an empty map: you get NULL, and that
 * isn't an error. We do this on purpose so that a map field that stays
 * uninitialized because nothing was put into it can be iterated directly,
 * without a NULL guard of the caller's own first
 * @note NULL for an empty map is not an error
 *
 * @see ccol_begin (macro wrapper)
 * @see ccol_iter_next
 * @see ccol_iter_destroy
 */
cmap_iterator *cbmap_begin_iter(struct cbinarymap *cbm, char **err);

/* ========================================================================== */
/*                    UNIFIED ITERATOR API                                    */
/* ========================================================================== */

/**
 * @brief RAII cleanup helper that all the container iterators share.
 *
 * It calls the iterator's own _free_fn, so the iterator is freed with the
 * right allocator whichever container made it.
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
 * Works with cvec, chmap and cbmap. All three give companion type variables
 * named @c container##__ccol_key_type_var and
 * @c container##__ccol_val_type_var, and this macro reads them to make typed
 * companion variables for the accessors of the iterator @p it.
 *
 * @param container  Container variable, which the caller must first declare
 *                   with a @c *_declare or @c *_construct macro.
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

/* The body of ccol_iter_next(). It's a function so that the macro's argument
 * is evaluated exactly once. Reach it through ccol_iter_next(). */
static inline cmap_iterator *_ccol_iter_next(cmap_iterator *it) {
  return it->_next_fn(it);
}

/**
 * @brief Move a unified iterator on to the next element.
 *
 * Calls the iterator's own @c _next_fn, so no container argument is needed.
 * At the end it destroys the iterator and gives NULL. The argument
 * is evaluated exactly once.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @return The iterator's new value, which is NULL after the last element.
 */
#define ccol_iter_next(it) _ccol_iter_next(it)

/**
 * @brief Get a typed const pointer to the iterator's key.
 *
 * For a cvec iterator the key is the element's index (a @c size_t), and for
 * a map iterator it's the map's real key. Because of the SSO storage, a
 * char-pointer key in a map goes through @c &key_pair->ptr while every other
 * type uses @c key_pair->ptr directly.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @return @c const @c KeyT* to the current key.
 */
#define ccol_iter_key_ptr(it)                                            \
  ({                                                                     \
    const __typeof__(*it##__ccol_iter_key_type_var) *__ccol_iter_k;      \
    if ((it)->_direct_ptr ||                                             \
        !_ccol_is_char_ptr(*it##__ccol_iter_key_type_var)) {             \
      __ccol_iter_k = (__typeof__(__ccol_iter_k))((it)->key_pair->ptr);  \
    } else {                                                             \
      __ccol_iter_k = (__typeof__(__ccol_iter_k))(&(it)->key_pair->ptr); \
    }                                                                    \
    __ccol_iter_k;                                                       \
  })

/**
 * @brief Qualify the result of ccol_iter_val_ptr() for the value type.
 *
 * When the value type is a character pointer, the only object the iterator
 * can name is the container's accessor slot, whose companion size field
 * describes the string the slot points at right now. A const qualifier on
 * the target keeps the pointer and the size in agreement. The caller can
 * read the stored string and edit its bytes in place within that length,
 * but assigning a new pointer is a compile error. Without the const a caller
 * could store a pointer the container neither allocated nor sized, the
 * accessor's size would then describe some other string, and the next read
 * of that element would run past the end of the new string.
 *
 * Every other value type names the element itself, so such a pointer comes
 * back unchanged and the caller can write through it.
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

/* In a map entry, the address that holds a character-pointer value is the
 * accessor slot itself, &val_pair->ptr, whose type is void *const *. Only a
 * character-pointer value type selects it, and ccol_iter_val_ptr then
 * converts it to ValT const *, which keeps the slot's const. Every other
 * value type selects val_pair->ptr. So no expression in ccol_iter_val_ptr
 * casts the const away from the slot, and the macro stays clean under
 * -Wcast-qual for every value type. */
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
 * @brief Get a typed pointer to the iterator's value, which is the current
 *        element.
 *
 * Writing through this pointer changes the underlying container too. For a
 * character-pointer value type the target carries a const qualifier, so you
 * can read and edit the stored string in place but you can't replace it; to
 * store a different string, use the owning container's insert or set
 * operation.
 *
 * @param it  Iterator variable that ccol_iter_declare() declares.
 *
 * @return @c ValT* to the current value or element, or @c ValT @c const*
 *         when @c ValT is a character pointer.
 */
#define ccol_iter_val_ptr(it)                                               \
  ({                                                                        \
    __typeof__(_ccol_iter_value_ptr_result(                                 \
        it, (__typeof__(*it##__ccol_iter_val_type_var) *)0)) __ccol_iter_v; \
    if ((it)->_direct_ptr ||                                                \
        !_ccol_is_char_ptr(*it##__ccol_iter_val_type_var)) {                \
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
 * @note it is evaluated exactly once. It must be a modifiable lvalue, such
 * as a variable or an element of an array.
 */
#define ccol_iter_destroy(it) \
  _ccol_iter_destroy_impl(it, _ccol_uniq(__ccol_iter_destroy_slot, __COUNTER__))

/* Internal: the body of ccol_iter_destroy. slot is a name from _ccol_uniq(),
 * so the macro can nest inside the argument of another destroy macro and
 * still be -Wshadow clean. The argument is evaluated exactly once. */
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

/* All three wrappers take a void*, so every __builtin_choose_expr branch is
 * type-compatible for any container type a caller passes. Only the selected
 * branch runs. */
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
 * Picks the right @c begin_iter at compile time with @c _Generic on the type
 * of @p container. The supported types are @c cvec, @c chmap and @c cbmap.
 *
 * An empty container gives @c NULL and no error, and a failed allocation
 * calls @c _ccol_fatal_err().
 *
 * @param container  A @c cvec, @c chmap or @c cbmap variable.
 *
 * @return @c cmap_iterator* at the first element, or @c NULL.
 *
 * @note Pair this macro with @c ccol_iter_declare(container, it), which is
 * what gives you typed accessors.
 */
/* There's no default association on purpose: a handle of any other type is
 * a compile error, never a call into some other container's begin
 * function. */
#define ccol_begin(container)                                              \
  ({                                                                       \
    char *_ccol_err = NULL;                                                \
    cmap_iterator *_ccol_it = _Generic((container),                        \
        struct cvector *: __ccol_cvec_begin,                               \
        struct chashmap *: __ccol_chmap_begin,                             \
        struct cbinarymap *: __ccol_cbmap_begin)((container), &_ccol_err); \
    if (!_ccol_it && _ccol_err) {                                          \
      _ccol_fatal_err("ccol_begin('%s'): %s", #container, _ccol_err);      \
    }                                                                      \
    _ccol_it;                                                              \
  })

/**
 * @brief Iterate over all the elements of any supported container.
 *
 * Declares a @c cmap_iterator* variable @p it with RAII cleanup and loops
 * from @c ccol_begin() to the end with @c ccol_iter_next(). Inside the loop
 * @p it gives typed access through @c ccol_iter_key_ptr(it) and
 * @c ccol_iter_val_ptr(it).
 *
 * @param container  A @c cvec, @c chmap or @c cbmap variable.
 * @param it         Name for the iterator variable.
 * @param ...        Compound statement to run for each element.
 *
 * @note A comma inside a function call with several arguments is safe in
 * the body.
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
