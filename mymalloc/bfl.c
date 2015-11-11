#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "./bfl.h"
#include "./memlib.h"

static void bfl_remove(binned_free_list* bfl, Node* node);
typedef enum {NOT_AVAILABLE, SPLIT_ABLE, SPLIT_UNABLE} block_type;

// alloc a block of value size, ensuring the returned address is 8-byte aligned
// size must be a multiple of the word size (8 byte)
static Node* bfl_alloc_aligned(binned_free_list* bfl, const size_t size) {
  assert(size < BFL_INSANITY_SIZE);
  const void * lo = mem_heap_lo();
  const void * hi = mem_heap_hi();

  Node* node;
  size_t delta;
  // If there is some free space at the end of the heap, we simply extend it
  if ( ((void*)((block_header_right*)hi - 1) >= lo) && ((void*)(((block_header_right*)hi - 1)->left) >= lo) &&
      ((void*)(((block_header_right*)hi - 1)->left) < hi) && (IS_FREE(((block_header_right*)hi - 1)->left)) ) {
    node = ((block_header_right*)hi - 1)->left;
    bfl_remove(bfl, node);
    if (GET_SIZE(node) >= size) return node;
    delta = size - GET_SIZE(node);
  } else {
	// The padding is to ensure the node address is 8-byte aligned
    const size_t padding = (void*) ALIGN_WORD_FORWARD(hi) - hi;
    delta = padding + size;
  }
  
  if (mem_sbrk(delta) == NULL) {
    return NULL;
  }
  
  // Set up metadata for node
  node = (Node*)(mem_heap_hi() - size);
  SET_SIZE(node, size);
  SET_UNFREE(node);
  NODE_TO_RIGHT(node)->left = node;
  return node;
}

// Create a new binned free list
binned_free_list bfl_new() {
  binned_free_list bfl;
  for (int i = 0; i < BFL_SIZE; i++) {
    bfl.lists[i] = NULL;
  }
  return bfl;
}

// Remove a node from the binned free list
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

// Add a block to the binned free list
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

// Wrap a Node object to ptr and add to binned free list
static void bfl_add(binned_free_list* bfl, void* ptr, size_t size) {
  assert(size < BFL_INSANITY_SIZE);
  Node* node = (Node*)ptr;
  SET_SIZE(node, size);
  NODE_TO_RIGHT(node)->left = node;
  bfl_add_block(bfl, node);
}

/* Perform coalescing (when you free node). By design, there are no two adjacent free blocks
 * Therefore bfl_coalesce will not be recursive.
 * There will be at most three blocks merging together, and we do separate checks for that.
 */
static void bfl_coalesce(binned_free_list* bfl, Node* node) {
  if (node == NULL) return;
  assert(IS_FREE(node));
  Node* left = node;
  block_header_right* right = NODE_TO_RIGHT(node);
  const void* lo = mem_heap_lo();
  const void* hi = mem_heap_hi();

  // Check for the block adjacent to the left of node
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

  // Check for the block adjacent to the right of node
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

// Perform a block split
static void bfl_block_split(binned_free_list* bfl, Node* node, const size_t size) {
  assert(size >= BFL_MIN_BLOCK_SIZE);
  assert(size < BFL_INSANITY_SIZE);
  assert(size < GET_SIZE(node));
  assert(GET_SIZE(node) < BFL_INSANITY_SIZE);
  assert(GET_SIZE(node) >= size + BFL_MIN_SPLIT_SIZE);
  
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

// This helper function checks for what purpose we want to do with the block
static block_type how_to_use_block(Node* const node, const size_t size) {
  if (node == NULL || GET_SIZE(node) < size) return NOT_AVAILABLE;  // can't use
  if (GET_SIZE(node)-size >= BFL_MIN_SPLIT_SIZE) return SPLIT_ABLE;  // should split
  if (GET_SIZE(node) > size) return SPLIT_UNABLE;  // don't need to split
  return NOT_AVAILABLE;
}

// Can we use this block for allocating purpose?
static bool inline can_use_block(Node* const node, const size_t size) {
  block_type answer = how_to_use_block(node, size);
  return (answer != NOT_AVAILABLE);
}

// Malloc on bfl
void* bfl_malloc(binned_free_list* bfl, size_t size) {
  size += TOTAL_HEADER_SIZE;
  if (size < BFL_MIN_BLOCK_SIZE) {
    size = BFL_MIN_BLOCK_SIZE;
  }
  size = ALIGN_WORD_FORWARD(size);
  
  // We find the smallest level that one can use a free block
  const lgsize_t k = lg2_up(size);
  lgsize_t depth = k;
  Node* node = bfl->lists[depth];

  // At level k, one needs to check for all blocks
  // Since blocks can have smaller size than the requested size
  // That won't happen in higher levels
  while (node != NULL && !can_use_block(node, size)) {
    node = node->next;
  }

  // For level depth > k, simply checking if there's any free block suffices
  if (!can_use_block(node, size)) {
    while (depth < BFL_SIZE && !can_use_block(node, size)) {
      node = bfl->lists[++depth];
    }
  }

  // A block has been found. We restrict to the smallest possible size block
  if (node != NULL) {
    Node * tmp_node = node->next;
    for (tmp_node = node->next; tmp_node != NULL; tmp_node = tmp_node->next) {
      if (GET_SIZE(tmp_node) < GET_SIZE(node) && can_use_block(tmp_node, size)) {
        node = tmp_node;
      }
    }
  }

  switch (how_to_use_block(node, size)) {
    case NOT_AVAILABLE:
      // No free block, allocate new
      node = bfl_alloc_aligned(bfl, size);
      if (node == NULL) {
        return NULL;
      }
      break;
    case SPLIT_ABLE:
      // A free block found, should split
      bfl_block_split(bfl, node, size);
      break;
    case SPLIT_UNABLE:
      // A free block found, no split necessary
      bfl_remove(bfl, node);
  }

  assert(GET_SIZE(node) >= size);
  SET_UNFREE(node);
  assert(NODE_TO_RIGHT(node)->left == node);
  assert(IS_WORD_ALIGNED((void*)((external_node*)node + 1)));
  return (void*)((external_node*)node + 1);
}

// Free a block
void bfl_free(binned_free_list* bfl, void* ptr) {
  if (ptr == NULL) return;
  Node* node = (Node*)((external_node*)ptr - 1);
  SET_FREE(node);
  bfl_coalesce(bfl, node);
}

// Realloc a block
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

  // We coalesce before checking
  Node* node = (Node*)((external_node*)ptr - 1);
  Node* next_left = (Node*)(NODE_TO_RIGHT(node)+1);
  void* hi = mem_heap_hi();
  if ((void*)next_left < hi && IS_FREE(next_left)) {
    bfl_remove(bfl, next_left);
    UP_SIZE(node, next_left);
    NODE_TO_RIGHT(node)->left = node;
  }

  switch(how_to_use_block(node, size)) {
    case NOT_AVAILABLE:
      // If the requested size is larger than the current size,
      // first we check if the block is at the end of the heap
      // If so, we can perform a small grow;
      // otherwise we need to grow a big block in the end
	  
      // Check for end of block.
      // Splitting like this is not really optimal, but it's too late to change
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
    case SPLIT_ABLE:
      // Split bigger node to size
      bfl_block_split(bfl, node, size);
      break;
    case SPLIT_UNABLE:
      // If the new size equals to the old size, or only a little smaller,
      // return the old block
      break;
  }
  SET_UNFREE(node);
  assert(IS_WORD_ALIGNED(ptr));
  return ptr;
}
