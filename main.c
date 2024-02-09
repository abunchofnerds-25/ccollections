#include <cbstmap.h>
#include <chashmap.h>
#include <cvector.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void use_nested_types_hmap_of_cvecs() {
  chmap_construct(hmap, char *, cvec);
  for (int i = 0; i < 11; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "%d", i);
    cvec_construct(v, int);
    cvec_push(v, i);
    cvec_push_rvalue(v, i * i);
    chmap_insert(hmap, key, v);
  }

  for (int i = 0; i < 11; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "%d", i);
    cvec *v = chmap_get_ptr(hmap, key);
    cvec_push_rvalue(*v, i * i * i);
  }

  chmap_iter_declare(hmap, it);
  for (it = chmap_begin(hmap); it != NULL; it = chmap_iter_next(it)) {
    cvec v = *chmap_iter_val_ptr(it);
    cvec_enable_local_macros(v, int);
    int size = cvec_size(v);
    for (int i = 0; i < size; ++i) {
      int k = -1;
      sscanf(*chmap_iter_key_ptr(it), "%d", &k);
      assert(cvec_at(v, i) == pow(k, i + 1));
    }
    cvec_destroy(v);
  }

  chmap_destroy(hmap);
}

void use_chmap() {
  // chmap_declare(hmap, char*, int) = NULL;
  // chmap_init(hmap);
  chmap_construct(hmap, char *, int);

  int number = 10;
  chmap_insert(hmap, "ten", number);

  for (int i = 0; i < 10; ++i) {
    char key_buf[16] = {0};
    snprintf(key_buf, sizeof(key_buf), "%d", i);
    chmap_insert(hmap, key_buf, i);
  }

  for (int i = 11; i < 21; ++i) {
    char key_buf[16] = {0};
    snprintf(key_buf, sizeof(key_buf), "%d", i);
    char *buf_ptr = key_buf;
    chmap_insert(hmap, buf_ptr, i);
  }

  int elem_count = 0;

  chmap_iter_declare(hmap, it);
  for (it = chmap_begin(hmap); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "ten") == 0) {
      assert(*chmap_iter_val_ptr(it) == 10);
    } else {
      char expected_key[16];
      snprintf(expected_key, sizeof(expected_key), "%d",
               *chmap_iter_val_ptr(it));
      assert(strcmp(*chmap_iter_key_ptr(it), expected_key) == 0);
    }
    ++elem_count;
  }

  assert(elem_count == 21);

  chmap_destroy(hmap);
}

void use_cbmap() {
  // cbmap_declare(bmap, int, int) = NULL;
  // cbmap_init(bmap);
  cbmap_construct(bmap, int, int);

  for (int key = 1; key <= 10; ++key) {
    int val = key * key;
    cbmap_insert(bmap, key, val);
  }

  for (int key = 20; key >= 11; --key) {
    int val = key * key;
    cbmap_insert(bmap, key, val);
  }

  for (int key = -10; key <= 0; key += 2) {
    int key1 = key + 1;
    int val1 = key1 * key1;
    cbmap_insert(bmap, key1, val1);

    int val = key * key;
    cbmap_insert(bmap, key, val);
  }

  for (int key = 1; key <= 10; ++key) {
    int val = cbmap_get(bmap, key);
    assert(val == (key * key));
  }

  for (int key = 20; key >= 11; --key) {
    int val = cbmap_get(bmap, key);
    assert(val == (key * key));
  }

  for (int key = -10; key < 0; ++key) {
    int val = cbmap_get(bmap, key);
    assert(val == (key * key));
  }

  cbmap_iter_declare(bmap, iter);
  for (iter = cbmap_begin(bmap); iter != NULL; iter = cbmap_iter_next(iter)) {
    assert(*cbmap_iter_val_ptr(iter) ==
           ((*cbmap_iter_key_ptr(iter)) * (*cbmap_iter_key_ptr(iter))));
  }

  for (int i = -30; i <= 10; ++i) {
    cbmap_remove(bmap, i);
  }

  cbmap_destroy(bmap);
}

void use_cvec() {
  cvec_construct(vec, int);

  for (int i = 1; i <= 10; ++i) {
    cvec_push(vec, i);
    // i*i is a temporary memory area on the stack, so we use '_rvalue' macro
    cvec_push_rvalue(vec, i * i);
  }

  int size = cvec_size(vec);
  for (int i = 0; i < (size - 1); i += 2) {
    assert((cvec_at(vec, i) * cvec_at(vec, i)) == cvec_at(vec, i + 1));
  }

  cvec_destroy(vec);
}

int main() {
  use_chmap();
  use_cbmap();
  use_cvec();
  use_nested_types_hmap_of_cvecs();
  return 0;
}
