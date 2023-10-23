#pragma once

#include <common.h>

static void create_random_str(char *dest, size_t length) __attribute__((unused));
static void create_random_str(char *dest, size_t length) {
  char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  while (length-- > 0) {
      size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
      *dest++ = charset[index];
  }
  *dest = '\0';
}

static void *int_array_getter(void *col, uint32_t index) __attribute__((unused));
static void *int_array_getter(void *col, uint32_t index)
{
    return &((int *)col)[index];
}