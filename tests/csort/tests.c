#include <stdio.h>
#include <time.h>
#include <csort.h>
#include <cvector.h>
#include <tau/tau.h>

#include "test_helper.h"

TAU_MAIN() // sets up Tau (+ main function)

TEST(csort, cvector_integer_sort)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, int);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(int){rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++)
  {
    REQUIRE_LE(*(int *)cvector_at(cvec, i - 1), *(int *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_double_sort)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, double);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(double){((double)rand() / RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++)
  {
    REQUIRE_LE(*(double *)cvector_at(cvec, i - 1), *(double *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_string_sort_with_known_values)
{
  const int num_sample = 10;
  char test_strings[10][100] = {
    "hVqL8eY9hAfJ2ZyGvKpT7u5wN1bXk06RzDlI",
    "wz2A4TgB1fV0o9c8Qe5jZyH7NkWv6pUdxXlJl0L",
    "D3kRzVbYF4wJ5qW7uN0sTp1L3H2m8oXG6CZa5Bp",
    "kQb9ZfL6Dp0sW8XnV3mJ1rYw7E5qH2P9LgFZ5sT0",
    "Af56V7dL9Rg0pXJz3WmYq1hTkB2c8Z9yN0sD4FvK1",
    "N2Q4Fz1Jk8Wv0RmH5gXpYdT3L7V9nJwU6qBc9y2V",
    "p8Lz7yF5w2m9rG0YqD6XnQW1b5Vj4kT3oH2KsZy7A",
    "jL0V1BzG8oK7q6mF9X4H3tW2N5rYwP9Vb0uY2L8N",
    "k3G8pV0YwT9L7mJ1qZ5v0W2bN1cH9Xg3R6K4zQ2Y",
    "p6A3t9V1fWqL0rH8Z7X2j9yF5b0G5cL2m3WqY0k9Z"
};

char sorted_compare_values[10][100] = {
  "Af56V7dL9Rg0pXJz3WmYq1hTkB2c8Z9yN0sD4FvK1",
  "D3kRzVbYF4wJ5qW7uN0sTp1L3H2m8oXG6CZa5Bp",
  "N2Q4Fz1Jk8Wv0RmH5gXpYdT3L7V9nJwU6qBc9y2V",
  "hVqL8eY9hAfJ2ZyGvKpT7u5wN1bXk06RzDlI",
  "jL0V1BzG8oK7q6mF9X4H3tW2N5rYwP9Vb0uY2L8N",
  "k3G8pV0YwT9L7mJ1qZ5v0W2bN1cH9Xg3R6K4zQ2Y",
  "kQb9ZfL6Dp0sW8XnV3mJ1rYw7E5qH2P9LgFZ5sT0",
  "p6A3t9V1fWqL0rH8Z7X2j9yF5b0G5cL2m3WqY0k9Z",
  "p8Lz7yF5w2m9rG0YqD6XnQW1b5Vj4kT3oH2KsZy7A",
  "wz2A4TgB1fV0o9c8Qe5jZyH7NkWv6pUdxXlJl0L"
};

  char *err_str = NULL;
  int i;


  cvec_declare(cvec, char*);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    cvector_push_back(cvec, &(char *){test_strings[i]});
  }
  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 0; i < num_sample; i++)
  {
    REQUIRE_TRUE(strcmp(*(char **)cvector_at(cvec, i), sorted_compare_values[i]) == 0);
  }

  {
    const char *first, *second;
    for (i = 1; i < num_sample; i++)
    {
      first = *(const char **)cvector_at(cvec, i - 1);
      second = *(const char **)cvector_at(cvec, i);
      REQUIRE_TRUE(strcmp(first, second) < 0);
    }
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_string_sort_with_random_values)
{
  const int num_sample = 10;
  const int max_str_lenght = 100;
  char rand_strings[10][100];

  char *err_str = NULL;
  int i;


  cvec_declare(cvec, char*);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++)
  {
    create_random_str(rand_strings[i], (int)((double)rand() / RAND_MAX * max_str_lenght));
    cvector_push_back(cvec, &(char *){rand_strings[i]});
  }
  
  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  {
    const char *first, *second;
    for (i = 1; i < num_sample; i++)
    {
      first = *(const char **)cvector_at(cvec, i - 1);
      second = *(const char **)cvector_at(cvec, i);
      REQUIRE_TRUE(strcmp(first, second) < 0);
    }
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, csort_default_get_comparer_test)
{
  csort_item_comparer_t comparer = NULL;
  
  cvec_enable_local_macros(charVecType, char);
  comparer = csort_get_default_comparer(*charVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_char_comparer);

  cvec_enable_local_macros(shortVecType, short);
  comparer = csort_get_default_comparer(*shortVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_short_comparer);

  cvec_enable_local_macros(intVecType, int);
  comparer = csort_get_default_comparer(*intVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_int_comparer);

  // TODO: Add rest

  cvec_enable_local_macros(pCharVecType, char*);
  comparer = csort_get_default_comparer(*pCharVecType__cvec_type_var);
  REQUIRE_EQ((void *)comparer, (void *)csort_default_string_comparer);

}

#include "temporary_test_linked_list.h"
//// sizeof(NODE) temp_test_linked_list_swapper
// TEST(csort, default_swap)
// {
//   const int num_sample = 10;
//   const unsigned int seed = time(NULL);
//   int i;
//   int known_problematic_data_set[] = {1126865224, 1994478411, 2016342778, 874381483, 75213923, 1680913135, 1458973114, 1493654116, 63842372, 1335153754};

//   temp_test_linked_list list = {0};
//   ptr_temp_test_linked_list ptr_list = &list;

//   srand(seed);

//   for (i = 0; i < num_sample; i++)
//   {
//     temp_test_ll_push(ptr_list, rand());
//   }

//   temp_test_ll_print(ptr_list);


//   csort_sort(
//     ptr_list, 
//     temp_test_ll_count(ptr_list), 
//     sizeof(temp_test_ll_node),
//     (csort_item_getter_t)temp_test_linked_list_getter_wrapper,
//     (csort_item_comparer_t)temp_test_linked_list_comparer,
//     temp_test_linked_list_swapper
//   );
//   temp_test_ll_print(ptr_list);
  
//   ptr_list->Head = temp_test_ll_node_iterate_backward(ptr_list->Head, -1);
//   // temp_test_ll_print_detailed(ptr_list);
//   temp_test_ll_print(ptr_list);

//   printf("count: %d\n", temp_test_ll_count(ptr_list));
// }

//// sizeof(NODE) temp_test_linked_list_swapper
// TEST(csort, test_for_temp_test_linked_list_swapper)
// {
//   temp_test_linked_list list = {0};
//   ptr_temp_test_linked_list ptr_list = &list;

//   temp_test_ll_push(ptr_list, 10);
//   temp_test_ll_push(ptr_list, 5);
//   temp_test_ll_push(ptr_list, 20);
//   temp_test_ll_push(ptr_list, 15);

//   csort_sort(
//     ptr_list, 
//     temp_test_ll_count(ptr_list), 
//     sizeof(temp_test_ll_node),
//     (csort_item_getter_t)temp_test_linked_list_getter_wrapper,
//     (csort_item_comparer_t)temp_test_linked_list_comparer,
//     temp_test_linked_list_swapper
//   );
  
//   ptr_list->Head = temp_test_ll_node_iterate_backward(ptr_list->Head, -1);
//   temp_test_ll_print_detailed(ptr_list);

//   printf("count: %d\n", temp_test_ll_count(ptr_list));
// }

//// sizeof(int) temp_test_linked_list_swapper_v2
// TEST(csort, test_for_temp_test_linked_list_swapper_v2)
// {
//   temp_test_linked_list list = {0};
//   ptr_temp_test_linked_list ptr_list = &list;

//   temp_test_ll_push(ptr_list, 10);
//   temp_test_ll_push(ptr_list, 5);
//   temp_test_ll_push(ptr_list, 20);
//   temp_test_ll_push(ptr_list, 15);

//   csort_sort(
//     ptr_list, 
//     temp_test_ll_count(ptr_list), 
//     sizeof(int),
//     (csort_item_getter_t)temp_test_linked_list_getter_wrapper,
//     (csort_item_comparer_t)temp_test_linked_list_comparer,
//     temp_test_linked_list_swapper_v2
//   );
  
//   // ptr_list->Head = temp_test_ll_node_iterate_backward(ptr_list->Head, -1);
//   temp_test_ll_print_detailed(ptr_list);

//   printf("count: %d\n", temp_test_ll_count(ptr_list));
// }


// sizeof(int) temp_test_linked_list_swapper_v2
TEST(csort, rand_test_for_temp_test_linked_list_swapper_v2)
{
  const int num_sample = 10;
  const unsigned int seed = time(NULL);
  int i;
  int known_problematic_data_set[] = {1126865224, 1994478411, 2016342778, 874381483, 75213923, 1680913135, 1458973114, 1493654116, 63842372, 1335153754};

  temp_test_linked_list list = {0};
  ptr_temp_test_linked_list ptr_list = &list;

  srand(seed);

  for (i = 0; i < num_sample; i++)
  {
    // temp_test_ll_push(ptr_list, rand());
    temp_test_ll_push(ptr_list, known_problematic_data_set[i]);
  }

  temp_test_ll_print(ptr_list);


  csort_sort(
    ptr_list, 
    temp_test_ll_count(ptr_list), 
    sizeof(int),
    (csort_item_getter_t)temp_test_linked_list_getter_wrapper,
    (csort_item_comparer_t)temp_test_linked_list_comparer,
    temp_test_linked_list_swapper_v2
  );
  
  // ptr_list->Head = temp_test_ll_node_iterate_backward(ptr_list->Head, -1);
  // temp_test_ll_print_detailed(ptr_list);
  temp_test_ll_print(ptr_list);

  printf("count: %d\n", temp_test_ll_count(ptr_list));
}

