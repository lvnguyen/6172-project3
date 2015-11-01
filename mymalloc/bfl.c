#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "./bfl.h"
#include "./memlib.h"

// lg bithack
// https://graphics.stanford.edu/~seander/bithacks.html#IntegerLogDeBruijn
size_t lg2(uint32_t n) {
  // TODO: put a faster log here
  size_t logn = 0;
  while ((1 << logn) < n) {
    logn++;
  }
  return logn;

  // This bithack is somehow incorrect
  /*static const int MultiplyDeBruijnBitPosition[32] = {
    0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
    8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
  };

  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return (size_t)(MultiplyDeBruijnBitPosition[(uint32_t)(n * 0x07C4ACDDU) >> 27]);*/
}

binned_free_list* bfl_new() {
  binned_free_list* bfl = calloc(BFL_SIZE, sizeof(freelist*));
  for (int i = 0; i < BFL_SIZE; i++) {
    bfl[i] = calloc(1, sizeof(freelist *));
  }
  return bfl;
}

// alloc a block of value size, ensuring the returned address is 8-byte aligned
// size must be a multiple of the word size (8 byte)
void * alloc_aligned(size_t size) {
  void * hi = mem_heap_hi();
  size_t padding = (void *) ALIGN_FORWARD(hi, WORD_ALIGN) - hi;
  size_t requested_size = padding + size;

  void * q = mem_sbrk(requested_size);
  if (q == NULL) {    
#ifdef DEBUG
    // is this because we run out of memory?
    printf("Memory limit exceeded\n");
#endif
    return NULL;
  }  
  assert( IS_WORD_ALIGNED(mem_heap_hi()) );
  return mem_heap_hi() - requested_size;
}

void freelist_free(freelist* fl) {
  if (fl == NULL) return;
  freelist_free(fl->next);
  free(fl);
}

void freelist_split(freelist* src_node, freelist* target_list, size_t size) {  
  freelist* intermediate = src_node + (size >> 1);
  if (target_list != NULL) {
    intermediate->next = target_list;
    src_node->next = intermediate;
    target_list = src_node;
  } else {
    src_node->next = intermediate;
    intermediate->next = NULL;
    target_list = src_node;
  }
}

void bfl_delete(binned_free_list* bfl) {
  for (int i = 0; i < BFL_SIZE; i++) {
    freelist_free(*bfl[i]);
  }
  free(*bfl);
}

// Split a node at level k into two smaller nodes
// Require k > 0 and bfl[k] is not empty
void bfl_single_split(binned_free_list* bfl, size_t k) {
  freelist* temp = *bfl[k];
  if (temp == NULL) return;
  *bfl[k] = temp->next;
  freelist_split(temp, *bfl[k - 1], 1<<k);
}

void* bfl_malloc(binned_free_list* bfl, size_t size) {
  /* To alloc a block whose size is size, we find the first possible level (named depth) such that
   * bfl[depth] != NULL and depth >= lg2(size) 
   * (lg2(size) is the smallest nonnegative integer satisfying 2^(lg2(size)) >= size)  
   * If there's no such level, we malloc a new block of level k and give it to the request
   * 
   * A malloc block contains the size and the header (will be put at the beginning of the block)
   */
  assert(size >= 0);

  size_t k = lg2((uint32_t) size);
  assert((1 << k) >= size);
  if (k > 0) {
    assert((1 << (k - 1)) < size);
  }

  size_t depth = k;
  while (depth < BFL_SIZE && *bfl[depth] == NULL) depth++;

  assert(depth >= 0 && depth <= BFL_SIZE);

  freelist* temp;

  // A free block found, split until bfl[k] contains a block  

  /*if (*bfl[depth] != NULL) {
    while (depth > k) {
      bfl_single_split(bfl, depth);
      depth--;
    }
    temp = *bfl[k];*/
  if (*bfl[k] != NULL) {
    temp = *bfl[k];
    *bfl[k] = (temp->next == NULL) ? NULL : temp->next;
  } else {
    // mem_sbrk a new block
    size_t requested_size = (1 << (k + 1));
    void * new_alloc = alloc_aligned(requested_size);
    if (new_alloc == NULL) {
      return NULL;
    }
    
    // Align new_alloc to be 8-byte
    temp = (freelist *) new_alloc;
    temp->next = NULL;
  }

  // TODO: put even more data into header (for coalescing)
  assert(temp != NULL);  
  * (size_t *) ((void *) temp) = k;
  return temp;
}

void bfl_free(binned_free_list* bfl, void* node) {
  if (node == NULL) {
    return;
  }

  // BUG: sometimes this created a really funny value for k
  size_t k = *(size_t *) node; // leak. TODO: I forgot what this comment means
  assert (k >= 0 && k < BFL_SIZE);

  freelist* fl = (freelist *) node;
  fl->next = *bfl[k];
  *bfl[k] = fl;
}

void* bfl_realloc(binned_free_list* bfl, void* node, size_t size) {
  // If the original node is NULL, we need to allocate a new node
  if (node == NULL)
    return bfl_malloc(bfl, size);

  // If the requested size is 0, we free the node
  if (size == 0) {
    bfl_free(bfl, node);
    return node;
  }

  size_t k = *(size_t *) node;
  size_t nodesize = 1 << k;

  // If the new size equals to the old size, we simply return the node
  if (size == nodesize) {
    return node;
  }
  /* TODO: for now, just alloc a new node, copy the old content to the new
   * and free the old node
   */
  void * new_node = bfl_malloc(bfl, size);
  memcpy(new_node, node, (size > nodesize) ? nodesize : size);
  bfl_free(bfl, node);
  return new_node;

  /* If the requested size is larger than the current size,
   * we malloc a new block of the new size, copy the content
   * of the current block to the new block, and free the old block
   */
  /*if (size > nodesize) {
    void* new_node = bfl_malloc(bfl, size);
    memcpy(new_node, node, nodesize);
    bfl_free(bfl, node);
    return new_node;
  } else {
    k--;
    while ((1 << k) + size < nodesize) {
      freelist* temp = (freelist*)(node + nodesize/2);
      temp->next = bfl[k];
      bfl[k] = temp;
      k--;
      nodesize /= 2;
    }
    *((size_t*)((void*)(node))-1) = k+1;
    return node;
  }*/
}