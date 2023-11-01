/*
MIT License

Copyright (c) 2018 Danis Ozdemir

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

#include <csort.h>
#include <stdint.h>
#include <string.h>

void csort_default_swapper(void *first, void *second, uint32_t elem_size)
{
  unsigned char *ptr_btye_first = (unsigned char *)first;
  unsigned char *ptr_byte_second = (unsigned char *)second;
  unsigned char tmp;

  if (!first || !second || first == second)  { 
    return;
  }

  for (uint32_t byte_iter = 0; byte_iter < (uint32_t)elem_size; ++byte_iter)
  {
    tmp = ptr_btye_first[byte_iter];
    ptr_btye_first[byte_iter] = ptr_byte_second[byte_iter];
    ptr_byte_second[byte_iter] = tmp;
  }
}

void csort_default_pointer_swapper(void *first, void *second, uint32_t elem_size __attribute__((unused)))
{
  if (!first || !second || first == second)  { 
    return;
  }

  void *tmp = first;
  first = second;
  second = tmp;
}

int csort_qsort_partition(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer, csort_item_swapper_t swapper)
{
  int j;
  int i = low;
  void *pivot;
  void *pj;
  if (!getter || !comparer)
  {
    assert(false);
  }

  pivot = getter(col, high);

  for (j = low; j < high; j++)
  {
    pj = getter(col, j);

    if (comparer(pj, pivot) < 0)
    {
      swapper(getter(col, i), pj, elem_size);
      i++;
    }
  }
  swapper(getter(col, i), getter(col, high), elem_size);

  return i;
}

void ___csort_qsort_recursion(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer, csort_item_swapper_t swapper)
{
  if (!col)
  {
    return;
  }

  if (!getter || !comparer) {
    assert(false);
  }

  if (!swapper) {
    swapper = csort_default_swapper;
  }

  if (low < high)
  {
    int pivot =
        csort_qsort_partition(col, low, high, elem_size, getter, comparer, swapper);
        ___csort_qsort_recursion(col, low, pivot - 1, elem_size, getter, comparer, swapper);
        ___csort_qsort_recursion(col, pivot + 1, high, elem_size, getter, comparer, swapper);
  }
}

int csort_default_string_comparer(const void *first, const void *second)
{ 
  return strcmp(*(const char **)first, *(const char **)second);
}

#define ___csort__define_default_integral_comparer(type, name)                                               \
int ___csort__get_default_integral_comparer_name(name)(const void *first, const void *second) {              \
  return (*(const type*)first > *(const type*)second) - (*(const type*)first < *(const type*)second);        \
}

___csort__define_default_integral_comparer(char, char)
___csort__define_default_integral_comparer(short, short)
___csort__define_default_integral_comparer(int, int)
___csort__define_default_integral_comparer(long, long)
___csort__define_default_integral_comparer(long long, long_long)
___csort__define_default_integral_comparer(unsigned char, unsigned_char)
___csort__define_default_integral_comparer(unsigned short, unsigned_short)
___csort__define_default_integral_comparer(unsigned int, unsigned_int)
___csort__define_default_integral_comparer(unsigned long, unsigned_long)
___csort__define_default_integral_comparer(unsigned long long, unsigned_long_long)
___csort__define_default_integral_comparer(float, float)
___csort__define_default_integral_comparer(double, double)
___csort__define_default_integral_comparer(long double, long_double)

#undef __define_default_integral_comparer