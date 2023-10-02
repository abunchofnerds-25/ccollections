#pragma once

#include <common.h>


static int int_comparer(void *first, void* second)
{                                                       
    return (*(int *)first) - (*(int *)second);        
}

static int double_comparer(void *first, void* second)
{
  double f = *(double *)first;                                           
  double s = *(double *)second;                                           

  if (f < s) {
    return -1;
  }
  else if (f > s) {
    return 1;
  }

  return 0;
}

static int string_comparer(void *first, void* second)
{                
  return strcmp(*(char **)first, *(char **)second);
}

static void create_random_str(char *dest, size_t length) {
    char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    while (length-- > 0) {
        size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
        *dest++ = charset[index];
    }
    *dest = '\0';
}

static void *int_array_getter(void *col, uint32_t index)
{
    return &((int *)col)[index];
}



///////////////////////// MACRO TESTS

#define __get_elem_type(col)  \
  _Generic((col),             \
    int*: int,                \
    default: typeof(*(col))   \
  )

#define __get_ccol_elem_size(col)                 \
  _Generic((col),                                 \
    cvec: sizeof(typeof(*col##__cvec_type_var)),  \
    default: 0                                    \
  )

#define __is_ccol_type(col) \
  _Generic((col),           \
    cvec: true,             \
    default: false          \
  )

#define __get_elem_size_v1(col)                      \
  _Generic((col),                                 \
    cvec: sizeof(typeof(*col##__cvec_type_var)),  \
    default: sizeof(typeof(*(col)))               \
  )

#define __get_elem_size_v2(col)           \
({                                        \
  size_t size = 0;                        \
  if (__is_ccol_type(col)) {              \
    size = __get_ccol_elem_size(col);     \
  }                                       \
  else {                                  \
    size = sizeof(typeof(*(col)));        \
  }                                       \
  size;                                   \
})


#define __size_of(col)          \
  _Generic((col),               \
    cvec: col##__cvec_type_var, \
    default: col                \
  )


#define __get_elem_size_v3(col) sizeof(typeof(*__size_of(col)))

#define __get_post_fix(col) \
  _Generic((col),           \
    cvec: __cvec_type_var, \
    default: ""              \
  )

#define __get_elem_size_v4(col) sizeof(typeof(*(col##__get_post_fix(col))))