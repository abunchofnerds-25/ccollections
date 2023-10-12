#include <stdio.h>
#include <time.h>
#include "csort.h"
#include "cvector.h"
#include <tau/tau.h>

// #include "test_helper.h"

TAU_MAIN() // sets up Tau (+ main function)

TEST(csort, cvector_integer_qsort_ex)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, int);
  cvec_init(cvec);
  // cvector *cvec = cvector_create(elem_size, &err_str);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(int){rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  // csort_qsort_ex(cvec, cvector_elem_count(cvec), elem_size, (csort_item_getter_t)cvector_at, int_comparer);
  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++)
  {
    REQUIRE_LE(*(int *)cvector_at(cvec, i - 1), *(int *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

////////////////////////////////////
// TEST(csort, cvector_string_qsort_ex)
// {
//   const unsigned int seed = time(NULL);
//   const int num_sample = 10;
//   const int max_str_lenght = 100;

//   char rand_strings[num_sample][max_str_lenght];
//   char *err_str = NULL;
//   int elem_size = sizeof(char **);
//   int i;

//   srand(seed);

//   cvector *cvec = cvector_create(elem_size, &err_str);

//   REQUIRE_NE((void *)cvec, NULL);
//   REQUIRE_EQ((void *)err_str, NULL);

//   for (i = 0; i < num_sample; i++)
//   {
//     create_random_str(rand_strings[i], (int)((double)rand() / RAND_MAX * max_str_lenght));
//     cvector_push_back(cvec, &(char *){rand_strings[i]});
//   }

//   REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

//   csort_qsort_ex(cvec, cvector_elem_count(cvec), elem_size, (csort_item_getter_t)cvector_at, string_comparer);

//   {
//     char *first, *second;
//     for (i = 1; i < num_sample; i++)
//     {
//       first = *(char **)cvector_at(cvec, i - 1);
//       second = *(char **)cvector_at(cvec, i);
//       REQUIRE_TRUE(strcmp(first, second) < 0);
//     }
//   }

//   cvector_destroy(cvec);
//   REQUIRE_EQ((void *)cvec, NULL);
// }
///////////////////////////////////

TEST(csort, csort_default_get_comparer_test)
{
  csort_item_comparer_t comparer = NULL;
  
  cvec_enable_local_macros(charVecType, char);
  comparer = __csort_default_get_comparer(*charVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_char_comparer);

  cvec_enable_local_macros(shortVecType, short);
  comparer = __csort_default_get_comparer(*shortVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_short_comparer);

  cvec_enable_local_macros(intVecType, int);
  comparer = __csort_default_get_comparer(*intVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_int_comparer);

  // TODO: Add rest

  cvec_enable_local_macros(pCharVecType, char*);
  comparer = __csort_default_get_comparer(*pCharVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_string_comparer);

}