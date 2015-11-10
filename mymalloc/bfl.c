#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "./bfl.h"
#include "./memlib.h"

// alloc a block of value size, ensuring the returned address is 8-byte aligned
// size must be a multiple of the word size (8 byte)
static void* alloc_aligned(const size_t size) {
  void * hi = mem_heap_hi();
  size_t padding = (void *) ALIGN_WORD_FORWARD(hi) - hi;
  size_t pad_size = padding + size;

  void * q = mem_sbrk(pad_size);
  if (q == NULL) {
    return NULL;
  }
  return mem_heap_hi() - size;
}

binned_free_list bfl_new() {
  binned_free_list bfl;
  for (int i = 0; i < BFL_SIZE; i++) {
    bfl.lists[i] = NULL;
  }
  return bfl;
}

static void inline bfl_remove(binned_free_list* bfl, Node* node) {
  if (node->prev != NULL) {
    node->prev->next = node->next;
  } else {
    bfl->lists[lg2_down(node->size)] = node->next;
  }
  if (node->next) node->next->prev = node->prev;
  node->free = false;
}

static void inline bfl_add_block(binned_free_list* bfl, Node* node) {
  const size_t k = lg2_down(node->size);
  node->free = true;
  node->prev = NULL;
  node->next = bfl->lists[k];
  if (bfl->lists[k] != NULL) {
    bfl->lists[k]->prev = node;
  }
  bfl->lists[k] = node;
}

static void inline bfl_add(binned_free_list* bfl, void* ptr, size_t size) {
  Node* node = (Node*)ptr;
  node->size = size;
  assert(size < BFL_INSANITY_SIZE);
  NODE_TO_RIGHT(node)->left = node;
  bfl_add_block(bfl, node);
}

static void bfl_coalesce(binned_free_list* bfl, Node* node) {
  if (node == NULL) return;
  assert(node->free);
  Node* left = node;
  block_header_right* right = NODE_TO_RIGHT(node);
  const void* lo = mem_heap_lo();
  const void* hi = mem_heap_hi();
  // merge left
  // address header at left-1, check valid
  if (!(left == lo)) {
    Node* further_left = ((block_header_right*)left-1)->left;
    if ((void*)further_left >= lo && (void*)further_left < hi && further_left->free) {
      bfl_remove(bfl, left);
      bfl_remove(bfl, further_left);

      further_left->size += left->size;
      left = further_left;
      right->left = left;
      bfl_add_block(bfl, left);
    }
  }
  // merge right
  // address header at right+1, check valid
  if (!((void*)(right+1) > hi)) {
    Node* next_left = (Node*)(NODE_TO_RIGHT(node)+1);
    if ((void*)next_left < hi && (void*)(NODE_TO_RIGHT(next_left)+1) < hi && next_left->free) {
      bfl_remove(bfl, left);
      bfl_remove(bfl, next_left);

      left->size += next_left->size;
      NODE_TO_RIGHT(left)->left = left;
      bfl_add_block(bfl, left);
    }
  }
}

static void bfl_block_split(binned_free_list* bfl, Node* node, const size_t size) {
  bfl_remove(bfl, node);
  assert(node->size < BFL_INSANITY_SIZE);
  block_header_right* right = NODE_TO_RIGHT(node);

  assert(node->size >= size + BFL_MIN_BLOCK_SIZE);
  assert(size >= BFL_MIN_BLOCK_SIZE);
  // shrink left to size
  node->size = size;
  node->free = false;
  assert(size < BFL_INSANITY_SIZE);
  block_header_right* mid_right = NODE_TO_RIGHT(node);
  mid_right->left = node;

  // reinsert right to bfl
  // size is the difference from the old right header to the new right header
  size_t right_size = (void*)right - (void*)mid_right;
  assert(right_size >= BFL_MIN_BLOCK_SIZE);
  bfl_add(bfl, (void*)(mid_right+1), right_size);
  bfl_coalesce(bfl, (Node*)(mid_right+1));
}

static int inline how_to_use_block(Node* const node, const size_t size) {
  if (node == NULL || node->size < size) return 0; // can't use
  if (node->size-size >= BFL_MIN_BLOCK_SIZE) return 1; // should split
  if (node->size > size) return 2; // don't need to split
  return 0;
}

static bool inline can_use_block(Node* const node, const size_t size) {
  return how_to_use_block(node, size);
}

void* bfl_malloc(binned_free_list* bfl, size_t size) {
  size += TOTAL_HEADER_SIZE;
  if (size < BFL_MIN_BLOCK_SIZE) {
    size = BFL_MIN_BLOCK_SIZE;
  }
  size = ALIGN_WORD_FORWARD(size);
  const size_t k = lg2_up(size);

  size_t depth = k;
  Node* node = bfl->lists[depth];
  // iterate current depth for usable block
  while (node != NULL && !can_use_block(node, size)) {
    node = node->next;
  }
  // climb up the bfl for usable block
  if (!can_use_block(node, size)) {
    depth++;
    while (depth < BFL_SIZE && !can_use_block(node, size)) {
      node = bfl->lists[++depth];
    }
  }

  switch (how_to_use_block(node, size)) {
    case 0:
      // No free block, allocate new
      node = (Node*)alloc_aligned(size);
      if (node == NULL) {
        return NULL;
      }
      node->size = size;
      assert(size < BFL_INSANITY_SIZE);
      NODE_TO_RIGHT(node)->left = node;
      break;
    case 1:
      // A free block found, should split
      bfl_block_split(bfl, node, size);
      break;
    case 2:
      // A free block found, no split necessary
      bfl_remove(bfl, node);
  }

  assert(node->size >= size);
  node->next = NULL;
  node->prev = NULL;
  node->free = false;
  assert(NODE_TO_RIGHT(node)->left == node);
  assert(IS_WORD_ALIGNED((void*)(node + 1)));
  return (void*)(node + 1);
}

void bfl_free(binned_free_list* bfl, void* ptr) {
  if (ptr == NULL) return;
  Node* node = (Node*)ptr - 1;
  node->free = true;
  bfl_add_block(bfl, node);
  bfl_coalesce(bfl, node);
}

void* bfl_realloc(binned_free_list* bfl, void* ptr, size_t size) {
  const size_t orig_size = size;
  size += TOTAL_HEADER_SIZE;
  if (size < BFL_MIN_BLOCK_SIZE) {
    size = BFL_MIN_BLOCK_SIZE;
  }
  size = ALIGN_WORD_FORWARD(size);
  // If the original node is NULL, we need to allocate a new node
  if (ptr == NULL)
    return bfl_malloc(bfl, orig_size);

  // If the requested size is 0, we free the node
  if (size == 0) {
    bfl_free(bfl, ptr);
    return NULL;
  }

  Node* node = (Node*)ptr - 1;
  void* new_ptr;
  switch(how_to_use_block(node, size)) {
    case 0:
      // If the requested size is larger than the current size,
      // we malloc a new block of the new size, copy the content
      // of the current block to the new block, and free the old block
      new_ptr = bfl_malloc(bfl, orig_size);
      memcpy(new_ptr, ptr, node->size - TOTAL_HEADER_SIZE);
      bfl_free(bfl, ptr);
      assert(IS_WORD_ALIGNED(new_ptr));
      return new_ptr;
    case 1:
      // Split bigger node to size
      bfl_block_split(bfl, node, size);
    case 2:
      // If the new size equals to the old size, or only a little smaller,
      // return the old block
      break;
  }
  node->free = false;
  assert(IS_WORD_ALIGNED(ptr));
  return ptr;
}
