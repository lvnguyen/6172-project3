#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "./bfl.h"
#include "./memlib.h"

static void bfl_remove(binned_free_list* bfl, Node* node);

// alloc a block of value size, ensuring the returned address is 8-byte aligned
// size must be a multiple of the word size (8 byte)
static Node* bfl_alloc_aligned(binned_free_list* bfl, const size_t size) {
  assert(size < BFL_INSANITY_SIZE);
  const void * lo = mem_heap_lo();
  const void * hi = mem_heap_hi();

  Node* node;
  size_t delta;
  // see if we can expand from a free node at the end of the heap
  if ( ((void*)((block_header_right*)hi - 1) >= lo) && ((void*)(((block_header_right*)hi - 1)->left) >= lo) &&
      ((void*)(((block_header_right*)hi - 1)->left) < hi) && (IS_FREE(((block_header_right*)hi - 1)->left)) ) {
    node = ((block_header_right*)hi - 1)->left;
    bfl_remove(bfl, node);
    if (GET_SIZE(node) >= size) return node;
    delta = size - GET_SIZE(node);
    goto bfl_alloc_aligned_end;
  }
  const size_t padding = (void*) ALIGN_WORD_FORWARD(hi) - hi;
  delta = padding + size;
bfl_alloc_aligned_end:
  if (mem_sbrk(delta) == NULL) {
    return NULL;
  }
  node = (Node*)(mem_heap_hi() - size);
  SET_SIZE(node, size);
  SET_UNFREE(node);
  NODE_TO_RIGHT(node)->left = node;
  return node;
}

binned_free_list bfl_new() {
  binned_free_list bfl;
  for (int i = 0; i < BFL_SIZE; i++) {
    bfl.lists[i] = NULL;
  }
  return bfl;
}

static void bfl_remove(binned_free_list* bfl, Node* node) {
  if (!IS_FREE(node)) return;
  if (node->prev != NULL) {
    node->prev->next = node->next;
  } else {
    bfl->lists[lg2_down(GET_SIZE(node))] = node->next;
  }
  if (node->next) node->next->prev = node->prev;
  SET_UNFREE(node);
}

static void bfl_add_block(binned_free_list* bfl, Node* node) {
  const lgsize_t k = lg2_down(GET_SIZE(node));
  SET_FREE(node);
  node->prev = NULL;
  node->next = bfl->lists[k];
  if (bfl->lists[k] != NULL) {
    bfl->lists[k]->prev = node;
  }
  bfl->lists[k] = node;
}

static void bfl_add(binned_free_list* bfl, void* ptr, size_t size) {
  assert(size < BFL_INSANITY_SIZE);
  Node* node = (Node*)ptr;
  SET_SIZE(node, size);
  NODE_TO_RIGHT(node)->left = node;
  bfl_add_block(bfl, node);
}

static void bfl_coalesce(binned_free_list* bfl, Node* node) {
  if (node == NULL) return;
  assert(IS_FREE(node));
  Node* left = node;
  block_header_right* right = NODE_TO_RIGHT(node);
  const void* lo = mem_heap_lo();
  const void* hi = mem_heap_hi();

  Node* further_left;
  Node* next_left;
  if (left != lo) {
    further_left = ((block_header_right*)left-1)->left;
    if ((void*)further_left >= lo && (void*)further_left < hi && IS_FREE(further_left)) {
      bfl_remove(bfl, further_left);
      UP_SIZE(further_left, left);
      left = further_left;
    }
  }

  if (!((void*)(right+1) > hi)) {
    next_left = (Node*)(NODE_TO_RIGHT(node)+1);
    if ((void*)next_left < hi && (void*)(NODE_TO_RIGHT(next_left)+1) < hi && IS_FREE(next_left)) {
      UP_SIZE(left, next_left);
      bfl_remove(bfl, next_left);
    }
  }

  NODE_TO_RIGHT(left)->left = left;
  bfl_add_block(bfl, left);
}

static void bfl_block_split(binned_free_list* bfl, Node* node, const size_t size) {
  assert(size >= BFL_MIN_BLOCK_SIZE);
  assert(size < BFL_INSANITY_SIZE);
  assert(size < GET_SIZE(node));
  assert(GET_SIZE(node) < BFL_INSANITY_SIZE);
  assert(GET_SIZE(node) >= size + BFL_MIN_BLOCK_SIZE);
  bfl_remove(bfl, node);
  block_header_right* right = NODE_TO_RIGHT(node);

  // shrink left to size
  SET_SIZE(node, size);
  SET_UNFREE(node);
  block_header_right* mid_right = NODE_TO_RIGHT(node);
  mid_right->left = node;

  // reinsert right to bfl
  // size is the difference from the old right header to the new right header
  size_t right_size = (void*)right - (void*)mid_right;
  assert(right_size >= BFL_MIN_BLOCK_SIZE);
  bfl_add(bfl, (void*)(mid_right+1), right_size);
}

static int how_to_use_block(Node* const node, const size_t size) {
  if (node == NULL || GET_SIZE(node) < size) return 0; // can't use
  if (GET_SIZE(node)-size >= BFL_MIN_BLOCK_SIZE) return 1; // should split
  if (GET_SIZE(node) > size) return 2; // don't need to split
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
  const lgsize_t k = lg2_up(size);

  lgsize_t depth = k;
  Node* node = bfl->lists[depth];

  // iterate current depth for usable block
  while (node != NULL && !can_use_block(node, size)) {
    node = node->next;
  }

  // climb up the bfl for usable block
  if (!can_use_block(node, size)) {
    while (depth < BFL_SIZE && !can_use_block(node, size)) {
      node = bfl->lists[++depth];
    }
  }

  // Not only we use this block, we use the smallest sized one
  if (node != NULL) {
    Node * tmp_node = node->next;
    for (tmp_node = node->next; tmp_node != NULL; tmp_node = tmp_node->next) {
      if (GET_SIZE(tmp_node) < GET_SIZE(node) && can_use_block(tmp_node, size)) {
        node = tmp_node;
      }
    }
  }

  switch (how_to_use_block(node, size)) {
    case 0:
      // No free block, allocate new
      node = bfl_alloc_aligned(bfl, size);
      if (node == NULL) {
        return NULL;
      }
      break;
    case 1:
      // A free block found, should split
      bfl_block_split(bfl, node, size);
      break;
    case 2:
      // A free block found, no split necessary
      bfl_remove(bfl, node);
  }

  assert(GET_SIZE(node) >= size);
  SET_UNFREE(node);
  assert(NODE_TO_RIGHT(node)->left == node);
  assert(IS_WORD_ALIGNED((void*)((external_node*)node + 1)));
  return (void*)((external_node*)node + 1);
}

void bfl_free(binned_free_list* bfl, void* ptr) {
  if (ptr == NULL) return;
  Node* node = (Node*)((external_node*)ptr - 1);
  SET_FREE(node);
  bfl_coalesce(bfl, node);
}

void* bfl_realloc(binned_free_list* bfl, void* ptr, const size_t orig_size) {
  // If the original node is NULL, we need to allocate a new node
  if (ptr == NULL)
    return bfl_malloc(bfl, orig_size);

  // If the requested size is 0, we free the node
  if (orig_size == 0) {
    bfl_free(bfl, ptr);
    return NULL;
  }

  size_t size = orig_size + TOTAL_HEADER_SIZE;
  size = ALIGN_WORD_FORWARD(size);
  if (size < BFL_MIN_BLOCK_SIZE) {
    size = BFL_MIN_BLOCK_SIZE;
  }

  // Coalesce before processing
  Node* node = (Node*)((external_node*)ptr - 1);
  Node* next_left = (Node*)(NODE_TO_RIGHT(node)+1);
  void* hi = mem_heap_hi();
  if ((void*)next_left < hi && IS_FREE(next_left)) {
    bfl_remove(bfl, next_left);
    UP_SIZE(node, next_left);
    NODE_TO_RIGHT(node)->left = node;
  }

  switch(how_to_use_block(node, size)) {
    case 0:
      // If the requested size is larger than the current size,
      // first we check if the block is at the end of the heap
      // If so, we can perform a small grow;
      // otherwise we need to grow a big block in the end

      // Check for end of block.
      if (hi - GET_SIZE(node) == (void*)node) {
        NODE_TO_RIGHT(node)->left = NULL;
        mem_sbrk(size - node->size);
        SET_SIZE(node, size);
        NODE_TO_RIGHT(node)->left = node;
        return ptr;
      }

      // Normal malloc
      void* new_ptr = bfl_malloc(bfl, orig_size);
      memcpy(new_ptr, ptr, GET_SIZE(node) - TOTAL_HEADER_SIZE);
      bfl_free(bfl, ptr);
      assert(IS_WORD_ALIGNED(new_ptr));
      return new_ptr;
    case 1:
      // Split bigger node to size
      bfl_block_split(bfl, node, size);
      break;
    case 2:
      // If the new size equals to the old size, or only a little smaller,
      // return the old block
      break;
  }
  SET_UNFREE(node);
  assert(IS_WORD_ALIGNED(ptr));
  return ptr;
}
