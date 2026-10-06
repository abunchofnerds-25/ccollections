# csort: stable sorting

`csort` is the sort of the library. It is a mergesort. Thus it has three
important properties:

- **It is stable.** Elements that are equal keep their initial order. For
  example, Ada and Chen both have the score 91, and Ada is first. After the
  sort, Ada is first. The `qsort` of the C library does not give this
  guarantee.
- **It is O(n log n) for all inputs.** No input makes it slow.
- **It has no recursion.** It works bottom-up, with loops. Thus a very large
  input cannot cause a stack overflow.

The cost is a temporary buffer that is as large as the data (O(n) more
memory). The sort allocates this buffer and frees it on each call.

Usually you use `csort` through a vector, with `cvec_sort` or
`cvector_sort_with_comparison_proc`. See the [cvector guide](cvector.md).
Use `csort_sort` directly to sort a plain C array. You can also use it on
any storage that you can index, for example a ring buffer.

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

`cvec_sort` selects a comparison from the element type. This works for all
integer and floating-point types, for enums and for `char *` strings. For a
struct, write a comparator. Then call `cvector_sort_with_comparison_proc`.

## Write a comparator

A comparator has the same form as the comparator that `qsort` takes. It
gets pointers to two elements. It gives a negative number, zero or a
positive number.

```c
int by_price(const void *a, const void *b) {
    const Item *x = a, *y = b;
    return (x->price > y->price) - (x->price < y->price);
}
```

The `(x > y) - (x < y)` form is safer than `x - y`. The `x - y` form
overflows for large integers, and it does not work for floating point. To
sort in descending order, interchange `x` and `y`.

## Sort a plain array

`csort_sort` does not assume that the elements are in a C array. You give
it a *getter*. A getter is a function that gives a pointer to the element at
an index. For an ordinary array, the getter is one line:

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
6. An allocator for the temporary buffer. Give `NULL` for the default
   allocator. See [Memory management](memory.md).

`csort_sort` gives `true` when it is successful. It gives `false` only when
it cannot sort. This occurs when it cannot allocate the temporary buffer,
or when the sizes are out of range. In that case, the collection does not
change. When there are two or more elements, the getter and the comparator
must not be `NULL`.

## The default comparisons

`csort_get_default_comparison_proc(x)` gives the comparison that
`cvec_sort` uses for the type of `x`. It reads only the type. It never
evaluates `x`. You can give this comparison to `csort_sort` yourself:

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

It gives `NULL` for a type that has no default comparison. Examples are a
struct, or a fixed-size `char` array such as `char name[16]`.

Two rules make the default comparisons safe for data that is not clean:

- **Floating point:** a NaN goes after all numbers. A NaN is equal only to
  a different NaN. Thus a NaN never changes the order of the real numbers.
- **Strings:** a `NULL` `char *` goes before all strings. It is equal only
  to a different `NULL`. Thus you can sort a list of optional names without
  special code.

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

Urgent jobs print first. Jobs with the same priority must print in the
order in which they arrived. The sort is stable. Thus a comparator on the
priority only is enough. You do not need a second comparison on the
time of arrival.

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

`invoice.pdf` prints before `contract.pdf`. The three jobs with priority 1
keep their order.

## Example: sort by two keys

Because the sort is stable, you can also sort by more than one key with
no added work. Sort by the minor key first. Then sort by the major key. The
second sort keeps the first order in each group. This program shows
employees by department, and by name in each department.

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

With the getter, `csort_sort` can work on storage that is not a simple
array. A ring buffer keeps its oldest element at `head`. After the end of
its array, it continues at the start. A getter that changes a logical index
into a slot index lets the sort put the elements in order. The sort does not
copy the elements out first.

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
    /* Six values that continue at the start of the array. The logical
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

- **The default comparisons do not use a `char` array field as a string.**
  A struct with a `char name[16]` member needs your own comparator. A vector
  of `char[16]` also needs your own comparator. For these types,
  `cvec_sort` does not compile.
- **`char *` is a string.** A vector of `char *`, `unsigned char *` or
  `uint8_t *` sorts by `strcmp`. If these pointers point to binary buffers,
  write a comparator that compares the data correctly.
- **The sort allocates memory.** Each call allocates a temporary buffer that
  has the size of the data. If the allocation fails, the vector macros stop
  the program, and `csort_sort` gives `false`.
- **The pointers from the getter must stay valid** for the full sort.

## Reference

[csort_sort(3)](../man/csort/csort_sort.3),
[csort_get_default_comparison_proc(3)](../man/csort/csort_get_default_comparison_proc.3),
[cvec_sort(3)](../man/cvector/cvec_sort.3),
[cvector_sort_with_comparison_proc(3)](../man/cvector/cvector_sort_with_comparison_proc.3)

Related guides:
[Vectors](cvector.md),
[Memory management](memory.md)
