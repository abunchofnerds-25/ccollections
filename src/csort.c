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


  
// void csort_swap(void *i, void *j, uint32_t elem_size)
// {
//     unsigned char *ci = i;
//     unsigned char *cj = j;
//     unsigned char tmp;

//     for (uint32_t byte_iter = 0; byte_iter < elem_size; ++byte_iter)
//     {
//         tmp = ci[byte_iter];
//         ci[byte_iter] = cj[byte_iter];
//         cj[byte_iter] = tmp;
//     }
// }

// int csort_qsort_partition(void *col, int low, int high, size_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer)   
// {                                                                        
//   int j;                                                                  
//   int i = low;                                                            
//   void *pivot = getter(col, high);
//   void *pj;

//   for (j = low; j < high; j++){
//     pj = getter(col, j);
    
//     if (comparer(pj, pivot) < 0) {    
//       csort_swap(getter(col, i), pj, elem_size);
//       i++;                                                            
//     }                                                                   
//   }     
//   csort_swap(getter(col, i), getter(col, high), elem_size);                                                              
                                                                          
//   return i;                                                                      
// }

void csort_qsort_recursion(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer)          
{   
  if (!col) {
    return;
  }

    if (low < high) {                                                             
      int pivot =                                                                 
        csort_qsort_partition(col, low, high, elem_size, getter, comparer);        
        csort_qsort_recursion(col, low, pivot - 1, elem_size, getter, comparer);   
        csort_qsort_recursion(col, pivot + 1, high, elem_size, getter, comparer);   
    }                                                                             
}