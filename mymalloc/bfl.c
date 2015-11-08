#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "./bfl.h"
#include "./memlib.h"

// lg bithack
// https://graphics.stanford.edu/~seander/bithacks.html#IntegerLogDeBruijn
static size_t lg2(uint32_t m) {
  uint32_t n = m;
  static const int MultiplyDeBruijnBitPosition[32] = {
    0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
    8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
  };

  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  size_t r = (size_t)(MultiplyDeBruijnBitPosition[(uint32_t)(n * 0x07C4ACDDU) >> 27]);
  // round down
  if ((1 << r) > m) {
    r--;
  }
  return r;
}

// alloc a block of value size, ensuring the returned address is 8-byte aligned
// size must be a multiple of the word size (8 byte)
static void* alloc_aligned(size_t size) {
  void * hi = mem_heap_hi();
  size_t padding = (void *) ALIGN_WORD_FORWARD(hi) - hi;
  size_t pad_size = padding + size;

  void * q = mem_sbrk(pad_size);
  if (q == NULL) {    
#ifdef DEBUG
    // is this because we run out of memory?
    printf("Memory limit exceeded\n");
#endif
    return NULL;
  }  
  return mem_heap_hi() - size;
}

binned_free_list bfl_new() {
  binned_free_list bfl = alloc_aligned(BFL_SIZE*sizeof(block_header*));
  for (int i = 0; i < BFL_SIZE; i++) {
    bfl[i] = NULL;
  }
  return bfl;
}

static void block_delete(block_header* bl) {
  if (bl == NULL) return;
  block_delete(bl->next);
  free(bl->right);
  free(bl);
}

void bfl_delete(binned_free_list bfl) {
  for (int i = 0; i < BFL_SIZE; i++) {
    block_delete(bfl[i]);
  }
  free(bfl);
}

static void inline bfl_remove_block(binned_free_list bfl, block_header* bl) {
  if (bl->prev != NULL) {
    bl->prev->next = bl->next;
  } else {
    bfl[lg2(bl->size)] = bl->next;
  }
}

static void inline bfl_add_block(binned_free_list bfl, block_header* bl) {
  const size_t k = lg2(bl->size);
  bl->prev = NULL;
  bl->next = bfl[k];
  if (bfl[k] != NULL) {
    bfl[k]->prev = bl;
  }
  bfl[k] = bl;
}

static void inline bfl_add(binned_free_list bfl, void* node, size_t size) {
  block_header* bl = (block_header*)node;
  bl->right = (block_header_right*)((char*)bl + sizeof(block_header) + size);
  bl->right->left = bl;
  bl->size = size;
  bl->free = true;
  bfl_add_block(bfl, bl);
}

static block_header* try_merge(binned_free_list bfl, block_header* bl) {
  if (bl == NULL) return NULL;
  assert(bl->free);
  block_header* left = bl;
  block_header_right* right = bl->right;
  const void* lo = mem_heap_lo();
  const void* hi = mem_heap_hi();
  // merge left
  // address header at left-1, check valid
  if (!(left == lo)) {
    block_header* further_left = ((block_header_right*)left-1)->left;
    if ((void*)further_left >= lo && further_left->free) {
      bfl_remove_block(bfl, left);
      bfl_remove_block(bfl, further_left);

      further_left->size += left->size + TOTAL_HEADER_SIZE;
      left = further_left;
      left->right = right;
      right->left = left;
      bfl_add_block(bfl, left);
    }
  }
  // merge right
  // address header at right+1, check valid
  if (!((void*)(right+sizeof(block_header)) > hi)) {
    block_header* next_left = (block_header*)(left->right+1);
    if ((void*)(next_left->right+1) < hi && next_left->free) {
      bfl_remove_block(bfl, left);
      bfl_remove_block(bfl, next_left);

      left->size += next_left->size + TOTAL_HEADER_SIZE;
      right = next_left->right;
      left->right = right;
      right->left = left;
      bfl_add_block(bfl, left);
    }
  }
  return left;
}

static block_header* block_split(binned_free_list bfl, block_header* bl, const size_t size) {  
  bfl_remove_block(bfl, bl);
  block_header_right* right = bl->right;

  assert(size >= BFL_MIN_BLOCK_SIZE);
  // shrink left to size
  bl->size = size;
  bl->right = (block_header_right*)((char*)bl + sizeof(block_header) + size);
  bl->right->left = bl;

  // reinsert right to bfl
  // size is the difference from the old right header to the new right header
  size_t right_size = (void*)right - (void*)(bl->right);
  assert(right_size >= BFL_MIN_BLOCK_SIZE);
  bfl_add(bfl, (void*)(bl->right+1), right_size);
  return bl;
}

static int inline how_to_use_block(block_header* const bl, const size_t size) {
  if (bl == NULL) return 0; // can't use
  if (bl->size-size >= BFL_MIN_BLOCK_SIZE) return 1; // should split
  if (bl->size > size) return 2; // don't need to split
  return 0;
}

static bool inline can_use_block(block_header* const bl, const size_t size) {
  return how_to_use_block(bl, size);
}

void* bfl_malloc(binned_free_list bfl, size_t size) {
  assert(size >= 0);
  if (size < BFL_MIN_BLOCK_SIZE) {
    size = BFL_MIN_BLOCK_SIZE;
  }
  size_t total_size = size + TOTAL_HEADER_SIZE;
  const size_t k = lg2(total_size);

  size_t depth = k;
  block_header* bl = bfl[depth];
  while (bl != NULL && !can_use_block(bl, total_size)) {
    bl = bl->next;
  }
  if (bl == NULL) {
    depth++;
    bl = bfl[depth];
    while (!can_use_block(bl, total_size) && bl != NULL) {
      bl = bl->next;
    }
  }

  switch (how_to_use_block(bl, total_size)) {
    case 0: ;
      // No free block, allocate new
      bl = (block_header*)alloc_aligned(total_size);
      if (bl == NULL) {
        return NULL;
      }
      break;
    case 1:
      // A free block found, should split
      bl = block_split(bfl, bl, size);
      break;
    case 2:
      // A free block found, no split necessary
      bfl_remove_block(bfl, bl);
  }

  bl->next = NULL;
  bl->prev = NULL;
  bl->size = size;
  bl->free = false;
  bl->right = (block_header_right*)((char*)bl + sizeof(block_header) + size);
  bl->right->left = bl;
  return (void*)((char*)bl + sizeof(block_header));
}

void bfl_free(binned_free_list bfl, void* node) {
  if (node == NULL) return;
  block_header* bl = (block_header*)(node - sizeof(block_header));
  bl->free = true;
  bfl_add_block(bfl, bl);
  try_merge(bfl, bl);
}

void* bfl_realloc(binned_free_list bfl, void* node, size_t size) {
  // If the original node is NULL, we need to allocate a new node
  if (node == NULL)
    return bfl_malloc(bfl, size);

  // If the requested size is 0, we free the node
  if (size == 0) {
    bfl_free(bfl, node);
    return node;
  }

  block_header* bl = (block_header*)(node - sizeof(block_header));
  // If the new size equals to the old size, or only a little smaller,
  // return the old block
  if (size <= bl->size && size > bl->size + BFL_MIN_BLOCK_SIZE) {
    return node;
  }
  /* If the requested size is larger than the current size,
   * we malloc a new block of the new size, copy the content
   * of the current block to the new block, and free the old block
   */
  if (size > bl->size) {
    void* new_node = bfl_malloc(bfl, size);
    memcpy(new_node, node, bl->size);
    bfl_free(bfl, node);
    return new_node;
  } else {
    // Otherwise, split the block into a smaller one
    // Put the rest into the bfl
    block_split(bfl, bl, size);
  }
  return bl;
}
