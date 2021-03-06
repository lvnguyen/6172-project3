#ifndef _BFL_H
#define _BFL_H

#include <stdbool.h>

#define BFL_INSANITY_SIZE (1 << 25)
#define BFL_MIN_BLOCK_SIZE 64
#define BFL_MIN_SPLIT_SIZE 2*BFL_MIN_BLOCK_SIZE
#define BFL_MIN_LG 6
#define BFL_SIZE 26
#define WORD_ALIGN 8

#define ALIGNED(x, alignment) ((((uint64_t)x) & ((alignment)-1)) == 0)
#define ALIGN_FORWARD(x, alignment) \
    ((((uint64_t)x) + ((alignment)-1)) & (~((uint64_t)(alignment)-1)))

#define IS_WORD_ALIGNED(x) (ALIGNED(x, WORD_ALIGN))
#define ALIGN_WORD_FORWARD(x) ALIGN_FORWARD(x, WORD_ALIGN)

struct block_header_right;

typedef uint8_t lgsize_t;

#define NODE_TO_RIGHT(node) ((block_header_right*)((void*)node + GET_SIZE(node)) - 1)

// encode free bit in size
#define SET_FREE(node) (node->size |= 1)
#define SET_UNFREE(node) (node->size &= ~1)
#define IS_FREE(node) ((node->size & 1) != 0)
#define GET_SIZE(node) (node->size & ~1)
#define SET_SIZE(node, size) (node->size = (size & ~1) | IS_FREE(node))
#define UP_SIZE(node, other) (node->size += GET_SIZE(other))

/*
 * The structure of a node is as following
 * --------------------------------------
 * | Header | Data | block_header_right |
 * --------------------------------------
 * in which the header contains the metadata of the block
 * such as which node before it and after it.
 *
 * block_header_right will point back to the beginning of the block.
 * This will be used for coalescing purpose 
 */

typedef struct Node {
  size_t size;
  struct Node* next;  // next node in the free list of that level
  struct Node* prev;  // previous node in the free list of that level
} Node;

typedef struct {
  size_t size;
} external_node;

typedef struct block_header_right {
  Node* left;  // pointing back to the beginning of the block
} block_header_right;

#define TOTAL_HEADER_SIZE (sizeof(external_node)+sizeof(block_header_right))

/*
 * The binned_free_list is an array of free nodes
 * The k-th level contains nodes of size up to 2^k, but more than 2^(k - 1) (including headers)
 */
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

// log base 2, rounding up: lg2(8)==3; lg2(9)==4.
static lgsize_t lg2_up(size_t n) {
  if (n == 0) return 0;
  lgsize_t ups = (31 - __builtin_clz((int) n));
  return ((1 << ups) != n) ? (ups + 1) : ups;
}

// log base 2, rounding down: lg2(15)==3; lg2(16)==4;
static lgsize_t lg2_down(size_t n) {
  return (n == 0) ? 0 : (31 - __builtin_clz((int) n));
}

#endif
