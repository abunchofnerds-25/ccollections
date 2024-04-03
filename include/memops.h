#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void mem_cpy(void* dst, const void* src, size_t n);

void mem_zero(void* dst, size_t n);
