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

#include <common.h>

#define POWERS_OF_TWO_LEN 64
static uint64_t uint64_powers_of_two[POWERS_OF_TWO_LEN] = {
    1ULL,
    2ULL,
    4ULL,
    8ULL,
    16ULL,
    32ULL,
    64ULL,
    128ULL,
    256ULL,
    512ULL,
    1024ULL,
    2048ULL,
    4096ULL,
    8192ULL,
    16384ULL,
    32768ULL,
    65536ULL,
    131072ULL,
    262144ULL,
    524288ULL,
    1048576ULL,
    2097152ULL,
    4194304ULL,
    8388608ULL,
    16777216ULL,
    33554432ULL,
    67108864ULL,
    134217728ULL,
    268435456ULL,
    536870912ULL,
    1073741824ULL,
    2147483648ULL,
    4294967296ULL,
    8589934592ULL,
    17179869184ULL,
    34359738368ULL,
    68719476736ULL,
    137438953472ULL,
    274877906944ULL,
    549755813888ULL,
    1099511627776ULL,
    2199023255552ULL,
    4398046511104ULL,
    8796093022208ULL,
    17592186044416ULL,
    35184372088832ULL,
    70368744177664ULL,
    140737488355328ULL,
    281474976710656ULL,
    562949953421312ULL,
    1125899906842624ULL,
    2251799813685248ULL,
    4503599627370496ULL,
    9007199254740992ULL,
    18014398509481984ULL,
    36028797018963968ULL,
    72057594037927936ULL,
    144115188075855872ULL,
    288230376151711744ULL,
    576460752303423488ULL,
    1152921504606846976ULL,
    2305843009213693952ULL,
    4611686018427387904ULL,
    9223372036854775808ULL};

/* Returns the smallest power of two that is >= _input, using binary search
 * over a precomputed table. The table covers all 64-bit powers of two, so the
 * result is architecture-dependent (capped at the pointer-size maximum).
 * Returns ccol_invalid_size when _input exceeds the largest representable
 * power of two for the current architecture. */
size_t find_nearest_gte_power_of_two(size_t _input) {
  uint64_t input = _input;
  size_t result = 0;

  if (input <= uint64_powers_of_two[0]) {
    result = (size_t)uint64_powers_of_two[0];
    return result;
  }

  int max_index = sizeof(size_t) * 8 - 1;  // Arch dependent

  if (input > uint64_powers_of_two[max_index]) {
    return ccol_invalid_size;
  }

  int left = 0, right = max_index, middle = (left + right) / 2;

  while (true) {
    if (uint64_powers_of_two[middle] == input) {
      // Found it
      result = (size_t)uint64_powers_of_two[middle];
      return result;
    } else if (uint64_powers_of_two[middle] < input) {
      // Go right
      left = middle + 1;
    } else {
      if (middle > 0 && uint64_powers_of_two[middle - 1] < input) {
        // Found it
        result = (size_t)uint64_powers_of_two[middle];
        return result;
      }
      // Go left
      right = middle;
    }

    middle = (left + right) / 2;
  }

  return ccol_invalid_size;
}

#define MAX_ALIGNMENT _Alignof(uint64_t)  // 8 on 64-bit, 4 on 32-bit
#define ALIGNMENT_MASK (MAX_ALIGNMENT - 1)

#define SMALL_CHUNKS_SIZE 32

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

typedef struct s9 {
  uint64_t _0_;
  uint8_t _1_;
} __attribute__((packed)) s9;

typedef struct s10 {
  uint64_t _0_;
  uint16_t _1_;
} __attribute__((packed)) s10;

typedef struct s11 {
  uint64_t _0_;
  uint16_t _1_;
  uint8_t _2_;
} __attribute__((packed)) s11;

typedef struct s12 {
  uint64_t _0_;
  uint32_t _1_;
} __attribute__((packed)) s12;

typedef struct s13 {
  uint64_t _0_;
  uint32_t _1_;
  uint8_t _2_;
} __attribute__((packed)) s13;

typedef struct s14 {
  uint64_t _0_;
  uint32_t _1_;
  uint16_t _2_;
} __attribute__((packed)) s14;

typedef struct s15 {
  uint64_t _0_;
  uint32_t _1_;
  uint16_t _2_;
  uint8_t _3_;
} __attribute__((packed)) s15;

typedef struct s16 {
  uint64_t _0_;
  uint64_t _1_;
} __attribute__((packed)) s16;

typedef struct s17 {
  uint64_t _0_;
  uint64_t _1_;
  uint8_t _2_;
} __attribute__((packed)) s17;

typedef struct s18 {
  uint64_t _0_;
  uint64_t _1_;
  uint16_t _2_;
} __attribute__((packed)) s18;

typedef struct s19 {
  uint64_t _0_;
  uint64_t _1_;
  uint16_t _2_;
  uint8_t _3_;
} __attribute__((packed)) s19;

typedef struct s20 {
  uint64_t _0_;
  uint64_t _1_;
  uint32_t _2_;
} __attribute__((packed)) s20;

typedef struct s21 {
  uint64_t _0_;
  uint64_t _1_;
  uint32_t _2_;
  uint8_t _3_;
} __attribute__((packed)) s21;

typedef struct s22 {
  uint64_t _0_;
  uint64_t _1_;
  uint32_t _2_;
  uint16_t _3_;
} __attribute__((packed)) s22;

typedef struct s23 {
  uint64_t _0_;
  uint64_t _1_;
  uint32_t _2_;
  uint16_t _3_;
  uint8_t _4_;
} __attribute__((packed)) s23;

typedef struct s24 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
} __attribute__((packed)) s24;

typedef struct s25 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint8_t _3_;
} __attribute__((packed)) s25;

typedef struct s26 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint16_t _3_;
} __attribute__((packed)) s26;

typedef struct s27 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint16_t _3_;
  uint8_t _4_;
} __attribute__((packed)) s27;

typedef struct s28 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint32_t _3_;
} __attribute__((packed)) s28;

typedef struct s29 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint32_t _3_;
  uint8_t _4_;
} __attribute__((packed)) s29;

typedef struct s30 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint32_t _3_;
  uint16_t _4_;
} __attribute__((packed)) s30;

typedef struct s31 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint32_t _3_;
  uint16_t _4_;
  uint8_t _5_;
} __attribute__((packed)) s31;

typedef struct s32 {
  uint64_t _0_;
  uint64_t _1_;
  uint64_t _2_;
  uint64_t _3_;
} __attribute__((packed)) s32;

/* Performs a copy of exactly n bytes (1-32) without calling into the C library.
 * Each case uses a packed struct assignment so the compiler emits the minimum
 * number of store instructions. This is always inlined to keep the switch
 * inside the hot path of mem_cpy rather than adding a call frame. */
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
      *(s3*)dst = *(s3*)src;
      return;
    }
    case 4:
      *(uint32_t*)dst = *(uint32_t*)src;
      return;
    case 5: {
      *(s5*)dst = *(s5*)src;
      return;
    }
    case 6: {
      *(s6*)dst = *(s6*)src;
      return;
    }
    case 7: {
      *(s7*)dst = *(s7*)src;
      return;
    }
    case 8:
      *(uint64_t*)dst = *(uint64_t*)src;
      return;
    case 9: {
      *(s9*)dst = *(s9*)src;
      return;
    }
    case 10: {
      *(s10*)dst = *(s10*)src;
      return;
    }
    case 11: {
      *(s11*)dst = *(s11*)src;
      return;
    }
    case 12: {
      *(s12*)dst = *(s12*)src;
      return;
    }
    case 13: {
      *(s13*)dst = *(s13*)src;
      return;
    }
    case 14: {
      *(s14*)dst = *(s14*)src;
      return;
    }
    case 15: {
      *(s15*)dst = *(s15*)src;
      return;
    }
    case 16: {
      *(s16*)dst = *(s16*)src;
      return;
    }
    case 17: {
      *(s17*)dst = *(s17*)src;
      return;
    }
    case 18: {
      *(s18*)dst = *(s18*)src;
      return;
    }
    case 19: {
      *(s19*)dst = *(s19*)src;
      return;
    }
    case 20: {
      *(s20*)dst = *(s20*)src;
      return;
    }
    case 21: {
      *(s21*)dst = *(s21*)src;
      return;
    }
    case 22: {
      *(s22*)dst = *(s22*)src;
      return;
    }
    case 23: {
      *(s23*)dst = *(s23*)src;
      return;
    }
    case 24: {
      *(s24*)dst = *(s24*)src;
      return;
    }
    case 25: {
      *(s25*)dst = *(s25*)src;
      return;
    }
    case 26: {
      *(s26*)dst = *(s26*)src;
      return;
    }
    case 27: {
      *(s27*)dst = *(s27*)src;
      return;
    }
    case 28: {
      *(s28*)dst = *(s28*)src;
      return;
    }
    case 29: {
      *(s29*)dst = *(s29*)src;
      return;
    }
    case 30: {
      *(s30*)dst = *(s30*)src;
      return;
    }
    case 31: {
      *(s31*)dst = *(s31*)src;
      return;
    }
    case 32: {
      *(s32*)dst = *(s32*)src;
      return;
    }
  }
}

/* Fast memcpy wrapper. For small buffers (<= 32 bytes) the alignment of both
 * pointers is checked: if both satisfy uint64_t alignment the inlined
 * struct-assignment fast path is taken; otherwise the C library memcpy handles
 * the misaligned case. Larger buffers always delegate to memcpy. */
void mem_cpy(void* dst, const void* src, size_t n) {
  if (n <= SMALL_CHUNKS_SIZE) {
    // Check if pointers are suitably aligned for fast path
    // Use pointer alignment check, since uint64_t is the largest direct access
    uintptr_t alignment = (uintptr_t)dst | (uintptr_t)src;

    if (alignment & ALIGNMENT_MASK) {  // Not well aligned
      memcpy(dst, src, n);
      return;
    }

    mem_cpy_small(dst, src, n);
    return;
  }

  memcpy(dst, src, n);
}

/* Zeroes exactly n bytes (1-32) without calling into the C library, mirroring
 * the same packed struct technique used by mem_cpy_small. Always inlined. */
inline __attribute__((always_inline)) void mem_zero_small(void* dst, size_t n) {
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
      *(s3*)dst = (s3){0};
      return;
    }
    case 4:
      *(uint32_t*)dst = 0;
      return;
    case 5: {
      *(s5*)dst = (s5){0};
      return;
    }
    case 6: {
      *(s6*)dst = (s6){0};
      return;
    }
    case 7: {
      *(s7*)dst = (s7){0};
      return;
    }
    case 8:
      *(uint64_t*)dst = 0;
      return;
    case 9: {
      *(s9*)dst = (s9){0};
      return;
    }
    case 10: {
      *(s10*)dst = (s10){0};
      return;
    }
    case 11: {
      *(s11*)dst = (s11){0};
      return;
    }
    case 12: {
      *(s12*)dst = (s12){0};
      return;
    }
    case 13: {
      *(s13*)dst = (s13){0};
      return;
    }
    case 14: {
      *(s14*)dst = (s14){0};
      return;
    }
    case 15: {
      *(s15*)dst = (s15){0};
      return;
    }
    case 16: {
      *(s16*)dst = (s16){0};
      return;
    }
    case 17: {
      *(s17*)dst = (s17){0};
      return;
    }
    case 18: {
      *(s18*)dst = (s18){0};
      return;
    }
    case 19: {
      *(s19*)dst = (s19){0};
      return;
    }
    case 20: {
      *(s20*)dst = (s20){0};
      return;
    }
    case 21: {
      *(s21*)dst = (s21){0};
      return;
    }
    case 22: {
      *(s22*)dst = (s22){0};
      return;
    }
    case 23: {
      *(s23*)dst = (s23){0};
      return;
    }
    case 24: {
      *(s24*)dst = (s24){0};
      return;
    }
    case 25: {
      *(s25*)dst = (s25){0};
      return;
    }
    case 26: {
      *(s26*)dst = (s26){0};
      return;
    }
    case 27: {
      *(s27*)dst = (s27){0};
      return;
    }
    case 28: {
      *(s28*)dst = (s28){0};
      return;
    }
    case 29: {
      *(s29*)dst = (s29){0};
      return;
    }
    case 30: {
      *(s30*)dst = (s30){0};
      return;
    }
    case 31: {
      *(s31*)dst = (s31){0};
      return;
    }
    case 32: {
      *(s32*)dst = (s32){0};
      return;
    }
  }
}

/* Fast memset-to-zero wrapper using the same alignment + size dispatch strategy
 * as mem_cpy: small aligned buffers use mem_zero_small, everything else falls
 * through to memset. */
void mem_zero(void* dst, size_t n) {
  if (n <= SMALL_CHUNKS_SIZE) {
    // Check alignment for fast path
    if ((uintptr_t)dst & ALIGNMENT_MASK) {  // Not well aligned
      memset(dst, 0, n);
      return;
    }

    mem_zero_small(dst, n);
    return;
  }

  memset(dst, 0, n);
}
