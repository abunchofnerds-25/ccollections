# cvector: a growable array

`cvector` is a dynamic array: you push elements onto its end, it grows
automatically, and you can read any element by its index in constant time.
It replaces the hand-written `realloc(arr, ++n * sizeof(*arr))` kind of
code.

Use a vector when:

- you do not know in advance how many elements you will have;
- you want the elements next to each other in memory, for example so that
  you can pass them to a C function that takes a plain array, or sort them;
- you add and remove elements mostly at the end.

Choose a different module when you look elements up by a key
([chashmap](chashmap.md) or [cbstmap](cbstmap.md)), or when you often insert
and remove elements in the middle.

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

Compile the program with `-std=gnu11`, because the macros use GNU C
extensions, and link it with `-lccollections`:

```sh
gcc -std=gnu11 scores.c -lccollections -o scores
```

The program prints `78`, `82` and `100`.

## How the macros know the element type

`cvec_construct(scores, int)` does two things: it creates the vector, and it
declares a hidden companion variable that records the element type `int`.
All the other `cvec_*` macros read that companion, which is how `cvec_at`
gives you an `int` that you can assign to and how `cvec_push` can convert
its argument for you.

The element type can be any type: a number, a pointer, a struct, a function
pointer type such as `void (*)(int)`, or even an array type such as
`char[32]`.

There are several ways to create a vector:

| You want | Use |
|---|---|
| Create it now | `cvec_construct(v, T)` |
| Declare now, create later | `cvec_declare(v, T)` then `cvec_init(v)` |
| Destroy it automatically at the end of the scope | `cvec_construct_scoped(v, T)` |
| Use your own allocator | `cvec_construct_mp(v, T, &procs)` |

The scoped forms are useful in a function with more than one `return` path,
because the vector is destroyed on every path out of the function. See [the
design guide](design.md) for how the lifecycle macros work together across
the library, and [Memory management](memory.md) for custom allocators.

## Add and remove elements

`cvec_push` adds a value to the end of the vector. The value can be a
variable, a literal or any expression, and the macro converts it to the
element type exactly as a C assignment would: an `int` pushed into a
`double` vector becomes a `double`, and a `float` pushed into an `int`
vector is truncated. The macro never reinterprets the bytes as a different
type.

```c
cvec_construct(ratios, double);
int whole = 3;
cvec_push(ratios, whole);              /* stored as 3.0 */
cvec_push(ratios, whole / 2.0);        /* stored as 1.5 */
```

You can push a struct in the same way, including the return value of a
function:

```c
cvec_push(points, make_point(3, 4));
```

`cvec_pop(v)` removes the last element and returns it by value.

To add many elements in one step, `cvec_append_array(v, arr, n)` copies `n`
elements from a plain array, and `cvec_append_cvec(dst, src)` adds all the
elements of another vector. Both macros copy bytes, so the source must hold
elements of the vector's element type.

When you know roughly how many elements you will add, call
`cvec_reserve(v, n)` first: it grows the storage once, and the pushes that
follow do not need to reallocate. `cvec_reset(v)` removes all the elements
and keeps the vector ready for reuse.

## Read elements

| You want | Use |
|---|---|
| The element at `i`, as an lvalue | `cvec_at(v, i)` |
| A pointer to it, or `NULL` when `i` is out of range | `cvec_at_ptr(v, i)` |
| The number of elements | `cvec_size(v)` |
| The index of the first element equal to a value | `cvec_find(v, value)` |
| A plain pointer to the whole array | `cvec_data_ptr(v)` |

`cvec_at` stops the program when the index is out of range, whereas
`cvec_at_ptr` returns `NULL` for the same index. Use `cvec_at_ptr` when the
index comes from input that you do not trust.

`cvec_find` returns `ccol_invalid_size` when no element equals the value. It
compares numbers by value, `char *` elements as strings, and structs byte by
byte; the [cvec_find(3)](../man/cvector/cvec_find.3) page has the details.
You can also call `cvector_find(v, &value, cmp)` with your own comparator.

You can also walk a vector with the library's common iterator, which gives
you the index and a pointer to each element:

```c
ccol_for_each(scores, it, {
    printf("[%zu] = %d\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});
```

The [iterators guide](citerators.md) describes that API.

## Sort a vector

`cvec_sort(v)` sorts in ascending order using the default comparison for the
element type, which works for all integer and floating-point types, for
enums, and for `char *` strings (in `strcmp` order). The sort is stable, so
equal elements keep their initial relative order.

For any other type, such as a struct, write a comparator and call
`cvector_sort_with_comparison_proc`. The comparator returns a negative
number, zero or a positive number, just like the one that `qsort` takes:

```c
int by_age_desc(const void *a, const void *b) {
    const Person *x = a, *y = b;
    return (y->age > x->age) - (y->age < x->age);
}

cvector_sort_with_comparison_proc(people, by_age_desc);
```

`cvec_sort` on a vector of structs deliberately does not compile, and the
compiler error tells you to use `cvector_sort_with_comparison_proc` instead.
The [csort guide](csort.md) has more about the default comparators.

## Give a vector to a function

A `cvec` is a handle, that is, a pointer, so passing it to a function is
cheap and the function works on the same vector. The function cannot see the
hidden type companion, however, so it must start with `cvec_redeclare`,
which states the element type again:

```c
double average(cvec values) {
    cvec_redeclare(values, double);
    double sum = 0;
    for (size_t i = 0; i < cvec_size(values); i++)
        sum += cvec_at(values, i);
    return cvec_size(values) ? sum / cvec_size(values) : 0.0;
}
```

The type you give must be the real element type; the compiler cannot check
this for you.

## When an error must not stop the program

When an allocation fails, the `cvec_*` macros stop the program through
`ccol_fatal_err()`, so ordinary code needs no error checks. If you need to
recover from the failure instead, call the raw functions underneath the
macros, which take a pointer to the element and return a status code:

```c
int value = 42;
if (cvector_push_back(v, &value) != ccol_success) {
    /* handle the failure */
}
```

The raw layer contains `cvector_push_back`, `cvector_pop_back`,
`cvector_reserve`, `cvector_append_array` and the other functions listed in
the Reference section below. `cvector_create` and `cvector_create_full`
create a vector and return `NULL` when they fail. A vector created this way
has no type companion, so give it one with `cvec_redeclare` before you use
the macros on it.

## Example: a paged leaderboard

The program sorts a list of students by score, highest first, and then
prints the result one page at a time. Because the sort is stable, students
with the same score keep the order in which they were added.

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

Ada comes before Chen and Dana comes before Fay, which is the order in which
the program added them.

## Example: statistics over a stream of numbers

The program reads numbers until the end of its input (in this example, a
fixed string takes the place of standard input) and then shows the mean, the
median and the 95th percentile. A vector is the right container here,
because the count is not known in advance and the median needs the values in
sorted order.

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

A vector of `char *` holds pointers, not the strings themselves. In this
example the vector owns copies of the names it keeps, so the program frees
each copy before it destroys the vector. Because `cvec_find` and `cvec_sort`
treat `char *` elements as strings, removing duplicates is easy.

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

- **Pointers into a vector can move.** `cvec_data_ptr`, `cvec_at_ptr` and
  `&cvec_at(v, i)` point into the vector's storage, which a push, an append,
  a reserve, a pop or a reset can move, so fetch the pointer again after
  such a call. Pushing a value read from the same vector, as in
  `cvec_push(v, cvec_at(v, 0))`, is safe.
- **The vector does not free the memory that its elements point to.** For a
  vector of pointers to heap memory, free each element before
  `cvec_destroy`.
- **Destroy the vector through one handle only.** `cvec_destroy(v)` sets `v`
  to `NULL`, so a second call on the same variable does nothing. A copy of
  the handle in another variable is not set to `NULL`, however, and a
  destroy through that copy frees the memory a second time.
- **Do not change a vector while you iterate over it** with the iterator
  API; changing it inside an index loop that you control is fine.
- **Thread safety.** A vector deliberately has no internal lock. When more
  than one thread uses the same vector, protect it with your own lock. See
  [Concurrency](concurrency.md).

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
