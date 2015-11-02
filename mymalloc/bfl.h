#ifndef _BFL_H
#define _BFL_H

#include <stdbool.h>

#define BFL_SIZE 32
#define WORD_ALIGN 8

#define ALIGNED(x, alignment) ((((uint64_t)x) & ((alignment)-1)) == 0)
#define ALIGN_FORWARD(x, alignment) \
    ((((uint64_t)x) + ((alignment)-1)) & (~((uint64_t)(alignment)-1)))

#define IS_WORD_ALIGNED(x) (ALIGNED(x, WORD_ALIGN))
#define ALIGN_WORD_FORWARD(x) ALIGN_FORWARD(x, WORD_ALIGN)

size_t lg2(uint32_t n);

typedef struct freelist {
  struct freelist * next;
  struct freelist * prev;
  size_t size;
} freelist;

void freelist_free(freelist* fl);

typedef freelist** binned_free_list;

// split freelist into two freelists
void freelist_split(freelist* src_node, binned_free_list* bfl, size_t size);

// create a binned free list
binned_free_list* bfl_new();

// delete the binned free list
void bfl_delete(binned_free_list* bfl);

// bfl has non-nill freelist at depth k, will split free node into depth k-1
void bfl_single_split(binned_free_list* bfl, size_t k);

// malloc using binned free list
void* bfl_malloc(binned_free_list* bfl, size_t size);

// free using binned free list
void bfl_free(binned_free_list* bfl, void* node);

// realloc using binned free list
void* bfl_realloc(binned_free_list* bfl, void* node, size_t size);

// Insert a new freelist block at level k
// Checking whether I need to auto merge
void insert_block(freelist * node, binned_free_list * bfl, int k, bool auto_merge);

void perform_merge(binned_free_list * bfl, freelist * node, int k);
#endif
