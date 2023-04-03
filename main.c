#include <cbstmap.h>
#include <chashmap.h>
#include <cvector.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void use_chmap() {
  // chmap_declare(hmap, char*, int) = NULL;
  // chmap_init(hmap);
  chmap_construct(hmap, char*, int);

  chmap_insert(hmap, "ten", (int){10});

  for (int i = 0; i < 10; ++i) {
    char key_buf[6] = {0};
    snprintf(key_buf, sizeof(key_buf), "%d", i);
    chmap_insert(hmap, key_buf, i);
  }

  chmap_iter_declare(hmap, it);
  for (it = chmap_begin(hmap); it != NULL; it = chmap_iter_next(it)) {
    fprintf(stderr, "%s: %d\n", *chmap_iter_key_ptr(it),
            *chmap_iter_val_ptr(it));
  }

  chmap_destroy(hmap);
}

void use_cbmap() {
  // cbmap_declare(bmap, int, int) = NULL;
  // cbmap_init(bmap);
  cbmap_construct(bmap, int, int);

  for (int key = 1; key <= 10; ++key) {
    int val = key * key;
    fprintf(stderr, "inserting %d\n", key);
    cbmap_insert(bmap, key, val);
  }

  for (int key = 20; key >= 11; --key) {
    int val = key * key;
    fprintf(stderr, "inserting %d\n", key);
    cbmap_insert(bmap, key, val);
  }

  for (int key = -10; key <= 0; key += 2) {
    int key1 = key + 1;
    int val1 = key1 * key1;
    fprintf(stderr, "inserting %d\n", key1);
    cbmap_insert(bmap, key1, val1);

    int val = key * key;
    fprintf(stderr, "inserting %d\n", key);
    cbmap_insert(bmap, key, val);
  }

  for (int key = 1; key <= 10; ++key) {
    int val = cbmap_get(bmap, key);
    fprintf(stderr, "%d -> %d\n-------------\n", key, val);
  }

  for (int key = 20; key >= 11; --key) {
    int val = cbmap_get(bmap, key);
    fprintf(stderr, "%d -> %d\n-------------\n", key, val);
  }

  for (int key = -10; key < 0; ++key) {
    int val = cbmap_get(bmap, key);
    fprintf(stderr, "%d -> %d\n-------------\n", key, val);
  }

  cbmap_iter_declare(bmap, iter);
  for (iter = cbmap_begin(bmap); iter != NULL; iter = cbmap_iter_next(iter)) {
    fprintf(stderr, "%3d : %3d\n", *cbmap_iter_key_ptr(iter),
            *cbmap_iter_val_ptr(iter));
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
    fprintf(stdout, "vec[%02d]: %02d  -  vec[%02d]: %02d\n", i, cvec_at(vec, i),
            i + 1, cvec_at(vec, i + 1));
  }

  cvec_destroy(vec);
}

int main() {
  use_chmap();
  use_cbmap();
  use_cvec();
  return 0;
}
