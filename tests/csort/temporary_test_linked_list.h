#pragma once

#include <stdio.h>
#include <string.h>
#include <common.h>
#include <csort.h>

typedef struct temp_test_ll_node temp_test_ll_node;
typedef struct temp_test_ll_node* ptr_temp_test_ll_node;

typedef struct temp_test_linked_list temp_test_linked_list;
typedef struct temp_test_linked_list* ptr_temp_test_linked_list;

static void temp_test_ll_push(ptr_temp_test_linked_list list, int data);
static ptr_temp_test_ll_node temp_test_ll_get(ptr_temp_test_linked_list list, uint32_t index);
static ptr_temp_test_ll_node temp_test_ll_last(ptr_temp_test_linked_list list);

struct temp_test_ll_node
{
    int data;
    ptr_temp_test_ll_node Prev;
    ptr_temp_test_ll_node Next;

};

struct temp_test_linked_list
{
    ptr_temp_test_ll_node Head;
};




static void temp_test_ll_push(ptr_temp_test_linked_list list, int data) __attribute__((unused));
static void temp_test_ll_push(ptr_temp_test_linked_list list, int data)
{
    ptr_temp_test_ll_node newNode = (ptr_temp_test_ll_node)malloc(sizeof(temp_test_ll_node));
    memset(newNode, 0, sizeof(temp_test_ll_node));
    newNode->data = data;

    ptr_temp_test_ll_node lastItem = temp_test_ll_last(list);

    if (lastItem) {
        lastItem->Next = newNode;
        newNode->Prev = lastItem;
    } else {
        list->Head = newNode;
    }
}


static ptr_temp_test_ll_node temp_test_ll_get(ptr_temp_test_linked_list list, uint32_t index)
{
    ptr_temp_test_ll_node current = list->Head;

    for (uint32_t i = 0; i < index; i++) {
        current = current->Next;
    }

    return current;
}

static ptr_temp_test_ll_node temp_test_ll_last(ptr_temp_test_linked_list list)
{
    ptr_temp_test_ll_node prevNode = NULL;
    ptr_temp_test_ll_node node = list->Head;

    while (node) {
        prevNode = node;
        node = node->Next;
    }

    return prevNode;
}

static uint32_t temp_test_ll_count(ptr_temp_test_linked_list list) __attribute__((unused));
static uint32_t temp_test_ll_count(ptr_temp_test_linked_list list)
{
    ptr_temp_test_ll_node node = list->Head;
    uint32_t item_counter = 0;
    while (node) {
        item_counter++;
        node = node->Next;
    }
    return item_counter;
}

static void temp_test_ll_print(ptr_temp_test_linked_list list) __attribute__((unused));
static void temp_test_ll_print(ptr_temp_test_linked_list list)
{
    ptr_temp_test_ll_node node = list->Head;
    printf("[");
    
    while (node) {
        printf("%d", node->data);
        if (node->Next) {
            printf(", ");
        }
        node = node->Next;
    }
    
    printf("]\n");
}

static void temp_test_ll_print_detailed(ptr_temp_test_linked_list list) __attribute__((unused));
static void temp_test_ll_print_detailed(ptr_temp_test_linked_list list)
{
    ptr_temp_test_ll_node node = list->Head;
    printf("[\n");
    uint32_t index = 0;
    while (node) {
        printf("[%d] :\t{\tdata : %d\t | \tPrev: %p\t | \tNext: %p } \t [Addr:%p]", index++, node->data, node->Prev, node->Next, node);
        if (node->Next) {
            printf(",\n");
        }
        node = node->Next;
    }
    
    printf("\n]\n");
}

static ptr_temp_test_ll_node temp_test_ll_node_iterate_forward(ptr_temp_test_ll_node node, int iteration_count) __attribute__((unused));
static ptr_temp_test_ll_node temp_test_ll_node_iterate_forward(ptr_temp_test_ll_node node, int iteration_count) 
{

    ptr_temp_test_ll_node returnNode = NULL;

    if (iteration_count < 0) {
        while (node) {
            returnNode = node;
            node = node->Next;
        }
    }
    else {
        while (iteration_count--)
        {
            returnNode = node->Next;
            if (!returnNode) return NULL;
        }
    }

    return returnNode;
}

static ptr_temp_test_ll_node temp_test_ll_node_iterate_backward(ptr_temp_test_ll_node node, int iteration_count) __attribute__((unused));
static ptr_temp_test_ll_node temp_test_ll_node_iterate_backward(ptr_temp_test_ll_node node, int iteration_count)
{

    ptr_temp_test_ll_node returnNode = NULL;

    if (iteration_count < 0) {
        while (node) {
            returnNode = node;
            node = node->Prev;
        }
    }
    else {
        while (iteration_count--)
        {
            returnNode = node->Prev;
            if (!returnNode) return NULL;
        }
    }

    return returnNode;
}


// static void temp_test_ll_print_node(char *pre_str, ptr_temp_test_ll_node node, char *post_str)
// {
//     if (!pre_str) {
//         pre_str = "";
//     }

//     if (!post_str) {
//         post_str = "";
//     }

//     printf("%s{\tdata : %d\t | \tPrev: %p\t | \tNext: %p } \t [Addr:%p]%s", pre_str, node->data, node->Prev, node->Next, node, post_str);
// }



static void *temp_test_linked_list_getter_wrapper(void *collection, uint32_t index) __attribute__((unused));
static void *temp_test_linked_list_getter_wrapper(void *collection, uint32_t index)
{
    return temp_test_ll_get((ptr_temp_test_linked_list)collection, index);
}

static int temp_test_linked_list_comparer(const void *first, const void *second) __attribute__((unused));
static int temp_test_linked_list_comparer(const void *first, const void *second)
{
  const ptr_temp_test_ll_node firstNode = (const ptr_temp_test_ll_node)first;
  const ptr_temp_test_ll_node secondNode = (const ptr_temp_test_ll_node)second;
  // return firstNode->data -secondNode->data;
  return csort_get_default_comparer(firstNode->data)(&(firstNode->data), &(secondNode->data));
}


static void temp_test_linked_list_swapper(void *first, void *second, uint32_t elem_size __attribute__((unused))) __attribute__((unused));
static void temp_test_linked_list_swapper(void *first, void *second, uint32_t elem_size __attribute__((unused)))
{
  ptr_temp_test_ll_node firstNode = (ptr_temp_test_ll_node)first;
  ptr_temp_test_ll_node secondNode = (ptr_temp_test_ll_node)second;

    if (!firstNode || !secondNode || firstNode == secondNode) {
      return;
    }

    if (firstNode->Next == secondNode) {
        
        if (firstNode->Prev) {
          firstNode->Prev->Next = secondNode;
        }
        
        if (secondNode->Next) {
          secondNode->Next->Prev = firstNode;
        }

        firstNode->Next = secondNode->Next;
        secondNode->Next = firstNode;

        secondNode->Prev = firstNode->Prev;
        firstNode->Prev = secondNode;
    }
    else if (secondNode->Next == firstNode) {
        temp_test_linked_list_swapper(second, first, elem_size);
    }
    else {
      ptr_temp_test_ll_node tmpNode;
        if (firstNode->Prev) {
          firstNode->Prev->Next = secondNode;
        }

        if (secondNode->Prev) {
          secondNode->Prev->Next = firstNode;
        }

        if (firstNode->Next) {
          firstNode->Next->Prev = secondNode;
        }

        if (secondNode->Next) {
          secondNode->Next->Prev = firstNode;
        }

        tmpNode = firstNode->Next;
        firstNode->Next = secondNode->Next;
        secondNode->Next = tmpNode;

        tmpNode = firstNode->Prev;
        firstNode->Prev = secondNode->Prev;
        secondNode->Prev = tmpNode;
    }
}


static void temp_test_linked_list_swapper_v2(void *first, void *second, uint32_t elem_size) __attribute__((unused));
static void temp_test_linked_list_swapper_v2(void *first, void *second, uint32_t elem_size)
{
  ptr_temp_test_ll_node firstNode = (ptr_temp_test_ll_node)first;
  ptr_temp_test_ll_node secondNode = (ptr_temp_test_ll_node)second;

    if (!firstNode || !secondNode || firstNode == secondNode) {
      return;
    }
    
    csort_default_swapper(&firstNode->data, &secondNode->data, elem_size);
}
