#include <memops.h>

#define SMALL_CHUNKS_SIZE 8

typedef struct s3 {
  uint16_t _0_;
  uint8_t _1_;
} __attribute__((packed)) s3;

typedef struct s5 {
  uint32_t _0_;
  uint8_t _1_;
} __attribute__((packed)) s5;

typedef struct s6 {
  uint32_t _0_;
  uint16_t _1_;
} __attribute__((packed)) s6;

typedef struct s7 {
  uint32_t _0_;
  uint16_t _1_;
  uint8_t _2_;
} __attribute__((packed)) s7;

inline __attribute__((always_inline)) void mem_cpy_small(void* dst,
                                                         const void* src,
                                                         size_t n) {
  // Handle common small sizes with direct assignments
  switch (n) {
    case 0:
      return;
    case 1:
      *(uint8_t*)dst = *(uint8_t*)src;
      return;
    case 2:
      *(uint16_t*)dst = *(uint16_t*)src;
      return;
    case 3: {
      s3* d = (s3*)dst;
      const s3* s = (const s3*)src;
      d->_0_ = s->_0_;
      d->_1_ = s->_1_;
      return;
    }
    case 4:
      *(uint32_t*)dst = *(uint32_t*)src;
      return;
    case 5: {
      s5* d = (s5*)dst;
      const s5* s = (const s5*)src;
      d->_0_ = s->_0_;
      d->_1_ = s->_1_;
      return;
    }
    case 6: {
      s6* d = (s6*)dst;
      const s6* s = (const s6*)src;
      d->_0_ = s->_0_;
      d->_1_ = s->_1_;
      return;
    }
    case 7: {
      s7* d = (s7*)dst;
      const s7* s = (const s7*)src;
      d->_0_ = s->_0_;
      d->_1_ = s->_1_;
      d->_2_ = s->_2_;
      return;
    }
    case 8:
      *(uint64_t*)dst = *(uint64_t*)src;
      return;
  }
}

inline __attribute__((always_inline)) void mem_cpy(void* dst, const void* src,
                                                   size_t n) {
  if (n <= SMALL_CHUNKS_SIZE) {
    mem_cpy_small(dst, src, n);
    return;
  }

  memcpy(dst, src, n);
}

// Direct inlined small zero implementations
inline inline __attribute__((always_inline)) void mem_zero_small(void* dst,
                                                                 size_t n) {
  // Handle common small sizes with direct assignments
  switch (n) {
    case 0:
      return;
    case 1:
      *(uint8_t*)dst = 0;
      return;
    case 2:
      *(uint16_t*)dst = 0;
      return;
    case 3: {
      s3* d = (s3*)dst;
      d->_0_ = 0;
      d->_1_ = 0;
      return;
    }
    case 4:
      *(uint32_t*)dst = 0;
      return;
    case 5: {
      s5* d = (s5*)dst;
      d->_0_ = 0;
      d->_1_ = 0;
      return;
    }
    case 6: {
      s6* d = (s6*)dst;
      d->_0_ = 0;
      d->_1_ = 0;
      return;
    }
    case 7: {
      s7* d = (s7*)dst;
      d->_0_ = 0;
      d->_1_ = 0;
      d->_2_ = 0;
      return;
    }
    case 8:
      *(uint64_t*)dst = 0;
      return;
  }
}

void mem_zero(void* dst, size_t n) {
  if (n <= SMALL_CHUNKS_SIZE) {
    mem_zero_small(dst, n);
    return;
  }

  memset(dst, 0, n);
}
