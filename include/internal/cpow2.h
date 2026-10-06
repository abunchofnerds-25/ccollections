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
 * @file cpow2.h
 * @brief INTERNAL ONLY. The power-of-two rounding that the containers use to
 *        size their own storage.
 *
 * No program uses this library through this function. Every caller is a
 * container of this library, growing or reserving its own backing store:
 * cvector, cstring and chashmap. A user of the library reaches the behavior
 * through cvector_reserve, cstring_reserve, chmap_create and chmap_reset, and
 * never through this name.
 *
 * This header is internal. It carries no visibility block. make install does
 * not install it. Its symbol is absent from the dynamic symbol table of the
 * shared library.
 */

#ifndef CCOL_CPOW2_H
#define CCOL_CPOW2_H

#include <common.h>
#include <stddef.h>

/**
 * @brief Find the nearest power of two that is greater than or equal to input
 *
 * It returns the smallest power of two that is >= input, computed from one
 * count of the leading zeros of input - 1. If input is larger than the largest
 * power of two that a size_t on this architecture can hold, it returns
 * ccol_invalid_size to show an error.
 *
 * @param input The value to round up to a power of two
 * @return The nearest power of two >= input, or ccol_invalid_size if input is
 *         too large
 *
 * @note The cost is constant: a subtraction, a count of leading zeros and a
 *       shift
 * @note It returns 1 for an input of 0 or 1
 * @note It returns ccol_invalid_size if input is above the largest power of
 *       two that a size_t holds
 *
 * Example:
 * @code
 * _ccol_find_nearest_gte_power_of_two(5)   -> 8
 * _ccol_find_nearest_gte_power_of_two(16)  -> 16
 * _ccol_find_nearest_gte_power_of_two(100) -> 128
 * _ccol_find_nearest_gte_power_of_two(UINT64_MAX) -> ccol_invalid_size
 * @endcode
 */
size_t _ccol_find_nearest_gte_power_of_two(size_t input);

#endif /* CCOL_CPOW2_H */
