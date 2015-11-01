#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "./bfl.h"
#include "./memlib.h"

// lg bithack
// https://graphics.stanford.edu/~seander/bithacks.html#IntegerLogDeBruijn
size_t lg2(uint32_t n) {
  static const int MultiplyDeBruijnBitPosition[32] = {
    0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
    8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
  };

  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return (size_t)(MultiplyDeBruijnBitPosition[(uint32_t)(n * 0x07C4ACDDU) >> 27]);
}

binned_free_list bfl_new() {
  binned_free_list bfl = calloc(BFL_SIZE, sizeof(freelist*));
  return bfl;
}

void freelist_free(freelist* fl) {
  if (fl == NULL) return;
  freelist_free(fl->next);
  free(fl);
}

void freelist_split(freelist* src_node, freelist* target_list, size_t size) {  
  freelist* intermediate = src_node + (size >> 1);
  intermediate->next = target_list;
  src_node->next = intermediate;
  target_list = src_node;  
}

void bfl_delete(binned_free_list* bfl) {
  for (int i=0; i<BFL_SIZE; i++) {
    freelist_free(bfl[i]);
  }
  free(*bfl);
}

// Split a node at level k into two smaller nodes
// Require k > 0 and bfl[k] is not empty
void bfl_single_split(binned_free_list* bfl, size_t k) {
  freelist* temp = bfl[k];
  if (temp == NULL) return;
  bfl[k] = temp->next;
  freelist_split(temp, bfl[k - 1], 1<<k);
}

void* bfl_malloc(binned_free_list* bfl, size_t size) {
  /* To alloc a block whose size is size, we find the first possible level (named depth) such that
   * bfl[depth] != NULL and depth >= lg2(size) 
   * (lg2(size) is the smallest nonnegative integer satisfying 2^(lg2(size)) >= size)  
   * If there's no such level, we malloc a new block of level k and give it to the request
   * 
   * A malloc block contains the size and the header (will be put at the beginning of the block)
   */

  size_t k = lg2((uint32_t) size);
  size_t depth = k;
  while (depth < BFL_SIZE && bfl[depth] == NULL) depth++;
  freelist* temp;

  // A free block found, split until bfl[k] contains a block
  if (bfl[depth] != NULL) {
    while (depth > k) {
      bfl_single_split(bfl, depth);
      depth--;
    }
  } else {
    // call mem_sbrk to malloc a new block
    size_t requested_size = (1 << k) + size;
    void * new_alloc = mem_sbrk(requested_size);
    temp = (freelist *) new_alloc - requested_size;
    bfl[k] = temp;
  }

  temp->next = bfl[k];
  bfl[k] = temp;
  *(size_t *) ((void*) temp) = k;
  return temp;
}

void bfl_free(binned_free_list* bfl, void* node) {
  size_t k = *(size_t *) node; // leak. TODO: I forgot what this comment means
  freelist* fl = (freelist *)node;
  fl->next = bfl[k];
  bfl[k] = fl;
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

  /* If the requested size is larger than the current size,
   * we malloc a new block of the new size, copy the content
   * of the current block to the new block, and free the old block
   */
  if (size > nodesize) {
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
  }
}