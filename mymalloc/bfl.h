#ifndef _BFL_H
#define _BFL_H

#include <stdbool.h>

#define BFL_MIN_BLOCK_SIZE 32
#define BFL_MIN_LG 5
#define BFL_SIZE 32
#define WORD_ALIGN 8

#define ALIGNED(x, alignment) ((((uint64_t)x) & ((alignment)-1)) == 0)
#define ALIGN_FORWARD(x, alignment) \
    ((((uint64_t)x) + ((alignment)-1)) & (~((uint64_t)(alignment)-1)))

#define IS_WORD_ALIGNED(x) (ALIGNED(x, WORD_ALIGN))
#define ALIGN_WORD_FORWARD(x) ALIGN_FORWARD(x, WORD_ALIGN)

struct block_header_right;

typedef uint8_t lgsize_t;

typedef struct Node {
  struct block_header_right* right;
  struct Node* next; // for free list
  struct Node* prev; // for free list
  size_t size;
  bool free;
} Node;

typedef struct block_header_right {
  Node* left;
} block_header_right;

#define TOTAL_HEADER_SIZE (sizeof(Node)+sizeof(block_header_right))

typedef struct {
  Node* lists[BFL_SIZE];
} binned_free_list;

// create a binned free list
binned_free_list bfl_new();

// malloc using binned free list
void* bfl_malloc(binned_free_list* bfl, size_t size);

// free using binned free list
void bfl_free(binned_free_list* bfl, void* ptr);

// realloc using binned free list
void* bfl_realloc(binned_free_list* bfl, void* ptr, size_t size);

// log base 2, rounding up: lg2(8)==3; lg2(9)==4;
// but returned value is always at least BFL_MIN_LG.
static lgsize_t lg2_up(size_t n) {
  if (n) n--;
  lgsize_t lg = BFL_MIN_LG;
  n >>= BFL_MIN_LG - 1;
  while (n >>= 1) lg++;
  return lg;
}

// log base 2, rounding down: lg2(15)==3; lg2(16)==4;
static lgsize_t lg2_down(size_t n) {
  lgsize_t lg = 0;
  while (n >>= 1) lg++;
  return lg;
}

#endif
