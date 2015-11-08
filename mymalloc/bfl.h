#ifndef _BFL_H
#define _BFL_H

#include <stdbool.h>

#define BFL_MIN_BLOCK_SIZE 32
#define BFL_SIZE 32
#define WORD_ALIGN 8

#define ALIGNED(x, alignment) ((((uint64_t)x) & ((alignment)-1)) == 0)
#define ALIGN_FORWARD(x, alignment) \
    ((((uint64_t)x) + ((alignment)-1)) & (~((uint64_t)(alignment)-1)))

#define IS_WORD_ALIGNED(x) (ALIGNED(x, WORD_ALIGN))
#define ALIGN_WORD_FORWARD(x) ALIGN_FORWARD(x, WORD_ALIGN)

struct block_header_right;

typedef struct block_header {
  struct block_header_right* right;
  struct block_header* next; // for free list
  struct block_header* prev; // for free list
  size_t size;
  bool free;
} block_header;

typedef struct block_header_right {
  block_header* left;
} block_header_right;

#define TOTAL_HEADER_SIZE (sizeof(block_header)+sizeof(block_header_right))

typedef block_header** binned_free_list;

// create a binned free list
binned_free_list bfl_new();

// delete the binned free list
void bfl_delete(binned_free_list bfl);

// malloc using binned free list
void* bfl_malloc(binned_free_list bfl, size_t size);

// free using binned free list
void bfl_free(binned_free_list bfl, void* node);

// realloc using binned free list
void* bfl_realloc(binned_free_list bfl, void* node, size_t size);

#endif
