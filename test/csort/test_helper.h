#pragma once

#include <common.h>


int int_comparer(void *first, void* second)
{                                                       
    return (*(int *)first) - (*(int *)second);        
}

int double_comparer(void *first, void* second)
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

int string_comparer(void *first, void* second)
{                
  return strcmp(*(char **)first, *(char **)second);
}

void create_random_str(char *dest, size_t length) {
    char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    while (length-- > 0) {
        size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
        *dest++ = charset[index];
    }
    *dest = '\0';
}

void *int_array_getter(void *col, uint32_t index)
{
    return &((int *)col)[index];
}