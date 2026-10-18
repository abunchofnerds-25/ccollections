# cvector: a vector

`cvector` is a dynamic array. You push elements onto its end, it grows by
itself, and you can read any element by its index in constant time. It's
what you'd otherwise write by hand with `realloc(arr, ++n * sizeof(*arr))`.

A vector is the right choice when you don't know in advance how many
elements you will have, when you want them next to each other in memory (to
pass them to a C function that takes a plain array, or to sort them), and
when you add and remove them mostly at the end. If you look elements up by
a key, use [chashmap](chashmap.md) or [cbstmap](cbstmap.md) instead, and a
vector is also a poor fit when you often insert and remove in the middle.

```c
#include <ccollections/cvector.h>
```

## A first example

```c
#include <stdio.h>
#include <ccollections/cvector.h>

int main(void) {
    cvec_construct(scores, int);        /* an empty vector of int */

    cvec_push(scores, 95);              /* push literals ... */
    cvec_push(scores, 82);
    int late = 78;
    cvec_push(scores, late);            /* ... or variables */

    cvec_at(scores, 0) = 100;           /* cvec_at is an lvalue */
    cvec_sort(scores);                  /* ascending, stable */

    for (size_t i = 0; i < cvec_size(scores); i++)
        printf("%d\n", cvec_at(scores, i));

    cvec_destroy(scores);               /* frees it and sets scores to NULL */
    return 0;
}
```

The macros use GNU C extensions, so compile with `-std=gnu11`, and link
with `-lccollections`:

```sh
gcc -std=gnu11 scores.c -lccollections -o scores
```

The program prints `78`, `82` and `100`.

## How the macros know the element type

`cvec_construct(scores, int)` creates the vector and also declares a hidden
companion variable that records the element type `int`. Every other `cvec_*`
macro reads that companion, and that's how `cvec_at` can give you an `int`
you can assign to and how `cvec_push` knows what to convert its argument
to.

Any type works as the element type: a number, a pointer, a struct, a
function pointer type such as `void (*)(int)`, even an array type such as
`char[32]`. A vector can be created in a few ways:

| You want | Use |
|---|---|
| Create it now | `cvec_construct(v, T)` |
| Declare now, create later | `cvec_declare(v, T)` then `cvec_init(v)` |
| Destroy it automatically at the end of the scope | `cvec_construct_scoped(v, T)` |
| Use your own allocator | `cvec_construct_mp(v, T, &procs)` |

In a function with more than one `return` the scoped forms are the better
choice, since the vector is destroyed on every way out of it. [The design guide](design.md)
explains how the lifecycle macros fit together across the library, and
[Memory management](memory.md) covers custom allocators.

## Add and remove elements

`cvec_push` adds a value to the end of the vector. The value can be a
variable, a literal or any other expression, and the macro converts it to
the element type the same way a C assignment would, so an `int` pushed into
a `double` vector becomes a `double` and a `float` pushed into an `int`
vector is truncated. It never reinterprets the bytes as another type.

```c
cvec_construct(ratios, double);
int whole = 3;
cvec_push(ratios, whole);              /* stored as 3.0 */
cvec_push(ratios, whole / 2.0);        /* stored as 1.5 */
```

A struct goes in the same way, and so does the return value of a function:

```c
cvec_push(points, make_point(3, 4));
```

`cvec_pop(v)` removes the last element and returns it by value. C can't
return an array by value, so for a vector of an array type like `char[32]`
the compiler refuses `cvec_pop` and you pop into a buffer with
`cvector_pop_back(v, buf)` instead.

To add many elements at once, `cvec_append_array(v, arr, n)` copies `n`
elements from a plain array and `cvec_append_cvec(dst, src)` adds all the
elements of another vector. Both copy bytes, which means the source has to
hold elements of the vector's own element type.

If you know roughly how many elements are coming, call `cvec_reserve(v, n)`
first. It grows the storage once and the pushes after it don't have to
reallocate. `cvec_reset(v)` removes every element and leaves the vector
ready to be used again.

## Read elements

| You want | Use |
|---|---|
| The element at `i`, as an lvalue | `cvec_at(v, i)` |
| A pointer to it, or `NULL` when `i` is out of range | `cvec_at_ptr(v, i)` |
| The number of elements | `cvec_size(v)` |
| The index of the first element equal to a value | `cvec_find(v, value)` |
| A plain pointer to the whole array | `cvec_data_ptr(v)` |

An index out of range stops the program in `cvec_at`, while `cvec_at_ptr`
returns `NULL` for it, so use `cvec_at_ptr` when the index comes from input
you don't trust.

`cvec_find` returns `ccol_invalid_size` when no element equals the value.
Numbers are compared by value, `char *` elements as strings and structs
byte by byte (the [cvec_find(3)](../man/cvector/cvec_find.3) page has the
details), and `cvector_find(v, &value, cmp)` lets you pass your own
comparator.

A vector can also be walked with the library's common iterator, which hands
you the index and a pointer to each element:

```c
ccol_for_each(scores, it, {
    printf("[%zu] = %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});
```

That API is described in the [iterators guide](citerators.md).

## Sort a vector

`cvec_sort(v)` sorts in ascending order with the default comparison for the
element type. There is one for the integer types, for `bool` (`false`
before `true`), for the floating-point types, for enums and for `char *`
strings (in `strcmp` order). The sort is stable, so equal elements keep the
relative order they had.

Any other type, a struct for example, needs a comparator that you pass to
`cvector_sort_with_comparison_proc`. It returns a negative number, zero or
a positive number, like the one `qsort` takes:

```c
int by_age_desc(const void *a, const void *b) {
    const Person *x = a, *y = b;
    return (y->age > x->age) - (y->age < x->age);
}

cvector_sort_with_comparison_proc(people, by_age_desc);
```

`cvec_sort` on a vector of structs doesn't compile on purpose, and the
error message points you to `cvector_sort_with_comparison_proc`. There's
more about the default comparators in the [csort guide](csort.md).

## Give a vector to a function

A `cvec` is a handle, which is just a pointer, so passing it to a function
is cheap and the function works on the same vector. What the function can't
see is the hidden type companion, so it has to start with `cvec_redeclare`
and state the element type again:

```c
double average(cvec values) {
    cvec_redeclare(values, double);
    double sum = 0;
    for (size_t i = 0; i < cvec_size(values); i++)
        sum += cvec_at(values, i);
    return cvec_size(values) ? sum / cvec_size(values) : 0.0;
}
```

That type must be the real element type, and the compiler has no way to
check it for you.

## When an error must not stop the program

When an allocation fails the `cvec_*` macros stop the program through
`_ccol_fatal_err()`, which is why ordinary code needs no error checks. If
you have to recover from the failure, call the raw functions under the
macros instead. Inside library code they're the right choice anyway, because
a library shouldn't stop its caller's program. They take a pointer to the
element and return a status code:

```c
int value = 42;
if (cvector_push_back(v, &value) != ccol_success) {
    /* handle the failure */
}
```

The raw layer has `cvector_push_back`, `cvector_pop_back`,
`cvector_reserve`, `cvector_append_array` and the rest of the functions in
the Reference section below. `cvector_create` and `cvector_create_full`
create a vector and return `NULL` on failure. A vector made this way has no
type companion, so give it one with `cvec_redeclare` before you use the
macros on it.

## Example: a paged leaderboard

This program sorts students by score, highest first, and prints the result
one page at a time. Students with the same score keep the order they were
added in, because the sort is stable: Ada stays ahead of Chen and Dana ahead
of Fay.

```c
#include <stdio.h>
#include <ccollections/cvector.h>

typedef struct {
    char name[16];
    int  score;
} Student;

static int by_score_desc(const void *a, const void *b) {
    const Student *x = a, *y = b;
    return (y->score > x->score) - (y->score < x->score);
}

static void print_page(cvec ranked, size_t page, size_t page_size) {
    cvec_redeclare(ranked, Student);
    size_t start = page * page_size;
    for (size_t i = start; i < start + page_size; i++) {
        Student *s = cvec_at_ptr(ranked, i);
        if (!s)
            break;                      /* past the last student */
        printf("%2zu. %-8s %d\n", i + 1, s->name, s->score);
    }
}

int main(void) {
    static const Student roster[] = {
        {"Ada", 91}, {"Brian", 78}, {"Chen", 91}, {"Dana", 85},
        {"Emil", 62}, {"Fay", 85}, {"Gus", 99},
    };

    cvec_construct_scoped(ranked, Student);
    cvec_append_array(ranked, roster, sizeof(roster) / sizeof(roster[0]));
    cvector_sort_with_comparison_proc(ranked, by_score_desc);

    for (size_t page = 0; page * 3 < cvec_size(ranked); page++) {
        printf("page %zu\n", page + 1);
        print_page(ranked, page, 3);
    }
    return 0;                           /* ranked is destroyed here */
}
```

## Example: statistics over a stream of numbers

Here the program reads numbers until its input ends (a fixed string stands
in for standard input) and then shows the mean, the median and the 95th
percentile. A vector fits well because the count isn't known in advance and
the median needs the values sorted.

```c
#include <stdio.h>
#include <stdlib.h>
#include <ccollections/cvector.h>

/* Linear interpolation between the two nearest ranks, so the median of
   an even count is the mean of the two middle values. */
static double percentile(cvec sorted, double p) {
    cvec_redeclare(sorted, double);
    size_t n = cvec_size(sorted);
    double pos = p * (double)(n - 1);
    size_t lo = (size_t)pos;
    size_t hi = lo + 1 < n ? lo + 1 : lo;
    double lo_val = cvec_at(sorted, lo), hi_val = cvec_at(sorted, hi);
    return lo_val + (pos - (double)lo) * (hi_val - lo_val);
}

int main(void) {
    const char *input = "12.5 9 30.25 7 14 18.5 11 95 13 10";

    cvec_construct(samples, double);
    const char *p = input;
    char *end;
    for (double x = strtod(p, &end); end != p; x = strtod(p, &end)) {
        cvec_push(samples, x);
        p = end;
    }
    if (cvec_size(samples) == 0) {
        cvec_destroy(samples);
        return 1;
    }

    double sum = 0;
    for (size_t i = 0; i < cvec_size(samples); i++)
        sum += cvec_at(samples, i);

    cvec_sort(samples);
    printf("n=%zu mean=%.2f median=%.2f p95=%.2f\n", cvec_size(samples),
           sum / cvec_size(samples), percentile(samples, 0.5),
           percentile(samples, 0.95));

    cvec_destroy(samples);
    return 0;
}
```

It prints `n=10 mean=22.02 median=12.75 p95=65.86`.

## Example: a list of owned strings

A vector of `char *` holds pointers and not the strings themselves. In this
example the vector owns copies of the names it keeps, so the program frees
every copy before it destroys the vector. `cvec_find` and `cvec_sort` treat
`char *` elements as strings, which makes dropping duplicates easy.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/cvector.h>

static void add_unique(cvec names, const char *name) {
    cvec_redeclare(names, char *);
    if (cvec_find(names, (char *)name) != ccol_invalid_size)
        return;                         /* the name is in the vector */
    char *copy = strdup(name);
    if (!copy)
        abort();
    cvec_push(names, copy);
}

int main(void) {
    const char *seen[] = {"orange", "apple", "pear", "apple",
                          "kiwi", "orange", "fig"};

    cvec_construct(names, char *);
    for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); i++)
        add_unique(names, seen[i]);

    cvec_sort(names);                   /* strcmp order */
    for (size_t i = 0; i < cvec_size(names); i++)
        printf("%s\n", cvec_at(names, i));

    while (cvec_size(names) > 0)
        free(cvec_pop(names));          /* the vector owns the copies */
    cvec_destroy(names);
    return 0;
}
```

## Good to know

`cvec_data_ptr`, `cvec_at_ptr` and `&cvec_at(v, i)` point into the
vector's storage, and a push, an append, a reserve, a pop or a reset can
move that storage, so fetch the pointer again after any of those calls.
Pushing a value you read from the same vector, as in
`cvec_push(v, cvec_at(v, 0))`, is safe.

The vector doesn't free what its elements point to. If it holds pointers to
heap memory, free each element before `cvec_destroy`.

`cvec_destroy(v)` sets `v` to `NULL`, so calling it again on the same
variable does nothing. A copy of the handle in another variable isn't set
to `NULL` though, and destroying through that copy frees the memory twice,
so destroy a vector through one handle only.

Don't change a vector while you walk it with the iterator API. Changing it
inside an index loop that you control is fine.

A vector has no lock inside it, on purpose. If more than one thread uses
the same vector, protect it with a lock of your own (see
[Concurrency](concurrency.md)).

## Reference

Overview: [cvector(7)](../man/cvector/cvector.7)

Lifecycle:
[cvec_construct(3)](../man/cvector/cvec_construct.3),
[cvec_construct_scoped(3)](../man/cvector/cvec_construct_scoped.3),
[cvec_construct_mp(3)](../man/cvector/cvec_construct_mp.3),
[cvec_construct_mp_scoped(3)](../man/cvector/cvec_construct_mp_scoped.3),
[cvec_declare(3)](../man/cvector/cvec_declare.3),
[cvec_declare_scoped(3)](../man/cvector/cvec_declare_scoped.3),
[cvec_init(3)](../man/cvector/cvec_init.3),
[cvec_init_mp(3)](../man/cvector/cvec_init_mp.3),
[cvec_redeclare(3)](../man/cvector/cvec_redeclare.3),
[cvec_reset(3)](../man/cvector/cvec_reset.3),
[cvec_destroy(3)](../man/cvector/cvec_destroy.3)

Elements:
[cvec_push(3)](../man/cvector/cvec_push.3),
[cvec_pop(3)](../man/cvector/cvec_pop.3),
[cvec_at(3)](../man/cvector/cvec_at.3),
[cvec_at_ptr(3)](../man/cvector/cvec_at_ptr.3),
[cvec_size(3)](../man/cvector/cvec_size.3),
[cvec_find(3)](../man/cvector/cvec_find.3),
[cvec_reserve(3)](../man/cvector/cvec_reserve.3),
[cvec_data_ptr(3)](../man/cvector/cvec_data_ptr.3),
[cvec_append_array(3)](../man/cvector/cvec_append_array.3),
[cvec_append_cvec(3)](../man/cvector/cvec_append_cvec.3)

Sorting: [cvec_sort(3)](../man/cvector/cvec_sort.3),
[cvector_sort_with_comparison_proc(3)](../man/cvector/cvector_sort_with_comparison_proc.3)

Raw functions (they give a status and do not stop the program):
[cvector_create(3)](../man/cvector/cvector_create.3),
[cvector_create_full(3)](../man/cvector/cvector_create_full.3),
[cvector_destroy(3)](../man/cvector/cvector_destroy.3),
[cvector_push_back(3)](../man/cvector/cvector_push_back.3),
[cvector_pop_back(3)](../man/cvector/cvector_pop_back.3),
[cvector_at(3)](../man/cvector/cvector_at.3),
[cvector_elem_count(3)](../man/cvector/cvector_elem_count.3),
[cvector_find(3)](../man/cvector/cvector_find.3),
[cvector_reserve(3)](../man/cvector/cvector_reserve.3),
[cvector_reset(3)](../man/cvector/cvector_reset.3),
[cvector_append_array(3)](../man/cvector/cvector_append_array.3),
[cvector_append_cvector(3)](../man/cvector/cvector_append_cvector.3),
[cvector_data_ptr(3)](../man/cvector/cvector_data_ptr.3),
[cvector_begin_iter(3)](../man/cvector/cvector_begin_iter.3),
[cvector_get_mprocs(3)](../man/cvector/cvector_get_mprocs.3)

Related guides:
[Iterators](citerators.md),
[Sorting](csort.md),
[Strings](cstring.md),
[Design of the type-safe macros](design.md),
[Memory management](memory.md),
[Concurrency](concurrency.md)
