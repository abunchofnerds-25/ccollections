#include <stdio.h>
#include <time.h>
#include "csort.h"
#include "cvector.h"
#include <tau/tau.h>

#include "test_helper.h"

TAU_MAIN() // sets up Tau (+ main function)

TEST(csort, cvector_integer_qsort_ex)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int elem_size = sizeof(int);
  int i;

  srand(seed);

  cvector *cvec = cvector_create(elem_size, &err_str);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(int){rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  csort_qsort_ex(cvec, cvector_elem_count(cvec), elem_size, (csort_item_getter_t)cvector_at, int_comparer);

  for (i = 1; i < num_sample; i++)
  {
    REQUIRE_LE(*(int *)cvector_at(cvec, i - 1), *(int *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

int double_compare(void *first, void *second)
{
  double f = *(double *)first;
  double s = *(double *)second;

  printf("%f - %f = %f\n", f, s, f - s);

  return (*(double *)first) - (*(double *)second);
}

TEST(csort, cvector_double_qsort_ex)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int elem_size = sizeof(double);
  int i;

  srand(seed);

  cvector *cvec = cvector_create(elem_size, &err_str);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(double){(double)rand() / rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  csort_qsort_ex(cvec, cvector_elem_count(cvec), elem_size, (csort_item_getter_t)cvector_at, double_comparer);

  for (i = 1; i < num_sample; i++)
  {
    REQUIRE_LE(*(double *)cvector_at(cvec, i - 1), *(double *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_string_qsort_ex)
{
  const unsigned int seed = time(NULL);
  const int num_sample = 10;
  const int max_str_lenght = 100;

  char rand_strings[num_sample][max_str_lenght];
  char *err_str = NULL;
  int elem_size = sizeof(char **);
  int i;

  srand(seed);

  cvector *cvec = cvector_create(elem_size, &err_str);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    create_random_str(rand_strings[i], (int)((double)rand() / RAND_MAX * max_str_lenght));
    cvector_push_back(cvec, &(char *){rand_strings[i]});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  csort_qsort_ex(cvec, cvector_elem_count(cvec), elem_size, (csort_item_getter_t)cvector_at, string_comparer);

  {
    char *first, *second;
    for (i = 1; i < num_sample; i++)
    {
      first = *(char **)cvector_at(cvec, i - 1);
      second = *(char **)cvector_at(cvec, i);
      REQUIRE_TRUE(strcmp(first, second) < 0);
    }
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, null_collection_check_qsort_ex)
{
  csort_qsort_ex(NULL, 100, 200, (csort_item_getter_t)cvector_at, int_comparer);
}

TEST(csort, cvector_sort_with_wrong_collection_size_qsort_ex)
{
  int dataset[] = {9, 4, 3, 6, 8, 5, 1, 2, 0, 7};
  const int num_sample = sizeof(dataset) / sizeof(*dataset);
  char *err_str = NULL;
  int elem_size = sizeof(int);
  int i;

  cvector *cvec = cvector_create(elem_size, &err_str);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(int){dataset[i]});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  csort_qsort_ex(cvec, cvector_elem_count(cvec) - 5, elem_size, (csort_item_getter_t)cvector_at, int_comparer);

  {
    int unsorted_count = 0;
    for (i = 1; i < num_sample; i++)
    {
      if (*(int *)cvector_at(cvec, i - 1) > *(int *)cvector_at(cvec, i))
      {
        unsorted_count++;
      }
    }
    REQUIRE_GT(unsorted_count, 0);

  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, array_integer_sort_qsort_ex)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);
  int arr[num_sample];

  int elem_size = sizeof(int);
  int i;

  srand(seed);

  for (i = 0; i < num_sample; i++)
  {
    arr[i] = rand();
  }


  csort_qsort_ex(arr, num_sample, elem_size, int_array_getter, int_comparer);

  for (i = 1; i < num_sample; i++)
  {
    REQUIRE_LE(arr[i - 1], arr[i]);
  }
}


TEST(csort, dev_test)
{
  // char char_arr[5];
  // short short_arr[5];
  int int_arr[5];
  // long long_arr[5];

  size_t size = __get_elem_size_v4(int_arr);
  printf("size: %ld\n", size);

}