# csort: stable sorting

`csort` is the sort that the rest of the library uses. It's an iterative,
bottom-up mergesort, so it is stable: if Ada and Chen both score 91 and Ada
comes first, she is still first after the sort, which `qsort` doesn't
promise. It runs in O(n log n) whatever the input looks like, and since it
works with loops instead of recursion a very large input can't overflow the
stack either.

What you pay for this is a temporary buffer as large as the data (O(n)
extra memory), which the sort allocates and frees on every call.

Most of the time you reach `csort` through a vector, with `cvec_sort` or
`cvector_sort_with_comparison_proc` (see the [cvector guide](cvector.md)).
You call `csort_sort` yourself when you want to sort a plain C array, or any
other storage you can index, a ring buffer for example.

```c
#include <ccollections/csort.h>
```

## Sort a vector

```c
#include <stdio.h>
#include <ccollections/cvector.h>

int main(void) {
    cvec_construct(values, double);
    cvec_push(values, 3.14);
    cvec_push(values, 1.41);
    cvec_push(values, 2.71);

    cvec_sort(values);                  /* default comparison for double */

    for (size_t i = 0; i < cvec_size(values); i++)
        printf("%.2f\n", cvec_at(values, i));
    cvec_destroy(values);
    return 0;
}
```

`cvec_sort` picks the comparison from the element type. That works for every
integer type, for `bool` (`false` before `true`), for every floating-point
type, for enums and for `char *` strings. A struct needs a comparator of
your own, which you pass to `cvector_sort_with_comparison_proc`.

## Write a comparator

A comparator looks exactly like the one `qsort` takes. It gets pointers to
two elements and returns a negative number, zero or a positive number.

```c
int by_price(const void *a, const void *b) {
    const Item *x = a, *y = b;
    return (x->price > y->price) - (x->price < y->price);
}
```

Write `(x > y) - (x < y)` and not `x - y`. The subtraction looks shorter,
but it overflows for large integers and gives wrong answers for floating
point, so it isn't worth the saving. If you want a descending order, swap
`x` and `y`.

## Sort a plain array

`csort_sort` doesn't assume the elements sit in a C array. You give it a
*getter* instead, a function that returns a pointer to the element at a
given index, and for an ordinary array that getter is one line:

```c
#include <stdio.h>
#include <ccollections/csort.h>

typedef struct {
    char name[16];
    int  age;
} Person;

static int by_age(const void *a, const void *b) {
    const Person *p = a, *q = b;
    return (p->age > q->age) - (p->age < q->age);
}

static void *person_at(void *collection, size_t index) {
    return &((Person *)collection)[index];
}

int main(void) {
    Person people[] = {{"Alice", 30}, {"Bob", 25}, {"Charlie", 35}};

    if (!csort_sort(people, 3, sizeof(Person), person_at, by_age, NULL))
        return 1;

    for (size_t i = 0; i < 3; i++)
        printf("%s %d\n", people[i].name, people[i].age);
    return 0;
}
```

The arguments are:

1. The collection.
2. The number of elements.
3. The size of one element.
4. The getter.
5. The comparator.
6. An allocator for the temporary buffer, or `NULL` for the default
   allocator (see [Memory management](memory.md)).

`csort_sort` returns `true` on success. It returns `false` only when it
can't sort at all, because the temporary buffer can't be allocated or the
sizes are out of range, and then the collection is left as it was. With two
or more elements the getter and the comparator must not be `NULL`.

## The default comparisons

`csort_get_default_comparison_proc(x)` gives you the comparison that
`cvec_sort` would use for the type of `x`. It only looks at the type and
never evaluates `x`, and you can hand the result to `csort_sort` yourself:

```c
static void *long_at(void *collection, size_t index) {
    return &((long *)collection)[index];
}

long ids[] = {42, 7, 19};
if (!csort_sort(ids, 3, sizeof(ids[0]), long_at,
                csort_get_default_comparison_proc(ids[0]), NULL)) {
    /* not sorted: ids is unchanged */
}
```

For a type that has no default comparison, a struct or a fixed-size `char`
array like `char name[16]`, it returns `NULL`.

The default comparisons are meant to cope with messy data. A NaN sorts
after every number and is equal only to another NaN, so it never disturbs
the order of the real numbers around it. A `NULL` `char *` sorts before
every string and is equal only to another `NULL`, which means you can sort a
list of optional names without any special code.

```c
#include <math.h>
#include <stdio.h>
#include <ccollections/cvector.h>

int main(void) {
    cvec_construct(readings, double);
    double in[] = {2.5, NAN, -1.0, 7.25, NAN, 0.0};
    cvec_append_array(readings, in, 6);
    cvec_sort(readings);                /* -1, 0, 2.5, 7.25, nan, nan */

    cvec_construct(nicknames, char *);
    char *names[] = {"zed", NULL, "amy", "kim", NULL};
    cvec_append_array(nicknames, names, 5);
    cvec_sort(nicknames);               /* NULL, NULL, amy, kim, zed */

    for (size_t i = 0; i < cvec_size(readings); i++)
        printf("%g ", cvec_at(readings, i));
    printf("\n");
    for (size_t i = 0; i < cvec_size(nicknames); i++) {
        const char *n = cvec_at(nicknames, i);
        printf("%s ", n ? n : "(none)");
    }
    printf("\n");

    cvec_destroy(readings);
    cvec_destroy(nicknames);
    return 0;
}
```

## Example: a print queue by priority

Urgent jobs print first, and jobs with the same priority have to print in
the order they arrived. Since the sort is stable, comparing the priority
alone is enough and you don't need a second comparison on the arrival time.

```c
#include <stdio.h>
#include <ccollections/csort.h>

typedef struct {
    int  priority;                      /* higher prints sooner */
    char file[24];
} PrintJob;

static int by_priority_desc(const void *a, const void *b) {
    const PrintJob *x = a, *y = b;
    return (y->priority > x->priority) - (y->priority < x->priority);
}

static void *job_at(void *collection, size_t index) {
    return &((PrintJob *)collection)[index];
}

int main(void) {
    PrintJob queue[] = {
        {1, "notes.txt"}, {5, "invoice.pdf"}, {1, "draft.doc"},
        {3, "slides.pdf"}, {5, "contract.pdf"}, {1, "photo.png"},
    };
    size_t n = sizeof(queue) / sizeof(queue[0]);

    if (!csort_sort(queue, n, sizeof(PrintJob), job_at, by_priority_desc,
                    NULL))
        return 1;

    for (size_t i = 0; i < n; i++)
        printf("p%d %s\n", queue[i].priority, queue[i].file);
    return 0;
}
```

`invoice.pdf` prints before `contract.pdf`, and the three jobs with
priority 1 come out in the order they went in.

## Example: sort by two keys

Stability also gives you sorting by several keys for free. Sort by the minor
key first and then by the major key, and the second sort keeps the order of
the first one inside each group. The program below lists employees by
department, and by name inside each department.

```c
#include <stdio.h>
#include <string.h>
#include <ccollections/cvector.h>

typedef struct {
    char dept[12];
    char name[12];
} Employee;

static int by_name(const void *a, const void *b) {
    return strcmp(((const Employee *)a)->name, ((const Employee *)b)->name);
}

static int by_dept(const void *a, const void *b) {
    return strcmp(((const Employee *)a)->dept, ((const Employee *)b)->dept);
}

int main(void) {
    static const Employee staff[] = {
        {"sales", "Rosa"}, {"eng", "Mia"},   {"ops", "Ken"},
        {"eng", "Abe"},    {"sales", "Ivy"}, {"eng", "Zoe"},
    };

    cvec_construct(list, Employee);
    cvec_append_array(list, staff, sizeof(staff) / sizeof(staff[0]));

    cvector_sort_with_comparison_proc(list, by_name);  /* minor key */
    cvector_sort_with_comparison_proc(list, by_dept);  /* major key */

    for (size_t i = 0; i < cvec_size(list); i++)
        printf("%-6s %s\n", cvec_at(list, i).dept, cvec_at(list, i).name);
    cvec_destroy(list);
    return 0;
}
```

## Example: sort a ring buffer in place

Because of the getter, `csort_sort` can work on storage that isn't a simple
array. A ring buffer keeps its oldest element at `head` and wraps from the
end of its array back to the start, so a getter that maps a logical index
to a slot lets the sort order the elements where they are, without copying
them out first.

```c
#include <stdio.h>
#include <ccollections/csort.h>

#define RING_CAP 8

typedef struct {
    int    slots[RING_CAP];
    size_t head;                        /* index of the oldest element */
    size_t count;
} Ring;

static void *ring_at(void *collection, size_t index) {
    Ring *r = collection;
    return &r->slots[(r->head + index) % RING_CAP];
}

static int ascending(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    /* Six values that wrap around to the start of the array; the logical
       order is 40 10 70 20 60 30. */
    Ring r = {.slots = {20, 60, 30, 0, 0, 40, 10, 70}, .head = 5,
              .count = 6};

    if (!csort_sort(&r, r.count, sizeof(int), ring_at, ascending, NULL))
        return 1;

    for (size_t i = 0; i < r.count; i++)
        printf("%d ", *(int *)ring_at(&r, i));
    printf("\n");
    return 0;
}
```

## Good to know

A `char` array field is not a string for the default comparisons. A
struct with a `char name[16]` member needs your own comparator, and so does
a vector of `char[16]`; `cvec_sort` doesn't compile for those types.

On the other hand every `char *` is a string, and this one is easy to
miss. A vector of `char *`, `unsigned char *` or `uint8_t *` sorts with
`strcmp`, so if those pointers point to binary buffers you have to write a
comparator that compares the data properly.

Every call allocates a temporary buffer the size of the data. When that
allocation fails the vector macros stop the program, while `csort_sort`
returns `false`, so code that has to survive a failed allocation should call
`csort_sort`. And the pointers your getter returns must stay valid for the
whole sort.

## Reference

Overview: [csort(7)](../man/csort/csort.7)

[csort_sort(3)](../man/csort/csort_sort.3),
[csort_get_default_comparison_proc(3)](../man/csort/csort_get_default_comparison_proc.3),
[cvec_sort(3)](../man/cvector/cvec_sort.3),
[cvector_sort_with_comparison_proc(3)](../man/cvector/cvector_sort_with_comparison_proc.3)

Related guides:
[Vectors](cvector.md),
[Memory management](memory.md)
