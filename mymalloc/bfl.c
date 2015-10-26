#include <stdlib.h>

#include "./bfl.h"

binned_free_list bfl_new() {
  binned_free_list bfl = calloc(BFL_SIZE, sizeof(freelist*));
  return bfl;
}

void freelist_free(freelist* fl) {
  if (fl == NULL) return;
  freelist_free(fl->next);
  free(fl);
}

void freelist_split(freelist* fl, size_t size) {
  freelist* nfl = (freelist*)((void*)(fl)+size/2);
  fl->next = nfl;
  nfl->next = NULL;
}

void bfl_delete(binned_free_list* bfl) {
  for (int i=0; i<BFL_SIZE; i++) {
    freelist_free(bfl[i]);
  }
  free(*bfl);
}

void bfl_single_split(binned_free_list* bfl, size_t k) {
  freelist* temp = bfl[k];
  if (temp == NULL) return;
  bfl[k] = temp->next;
  temp = freelist_split(temp, 1<<k);
  temp->next = bfl[k-1];
  bfl[k-1] = temp; 
}

void* bfl_malloc(binned_free_list* bfl, size_t size) {
  size_t k = lg2(uint32_t(size));
  size_t depth = k;
  while (depth < BFL_SIZE && bfl[depth] == NULL) depth++;
  freelist* temp;
  if (bfl[depth] != NULL) {
    while (depth > k) {
      bfl_single_split(bfl, depth);
      depth--;
    }
  } else {
    temp = (freelist*)(((size_t*)malloc((1 << (k+1))+size(size_t))) + 1);
    bfl[k+1] = fl;
    bfl_single_split(bfl, k+1);
  }
  temp = bfl[k];
  bfl[k] = temp->next;
  *((size_t*)((void*)(temp))-1) = k;
  return (void*)temp;
}

void bfl_free(binned_free_list* bfl, void* node) {
  size_t k = *(((size_t*)node)-1); // leak
  freelist* fl = (freelist*)node;
  fl->next = bfl[k];
  bfl[k] = fl;
}

void* bfl_realloc(binned_free_list* bfl, void* node, size_t size) {
  if (node == NULL)
    return bfl_malloc(bfl, size);
  if (size == 0) {
    bfl_free(bfl, node);
    return node;
  }
  size_t k = *(((size_t*)node)-1);
  size_t nodesize = 1 << k;
  if (size > nodesize) {
    void* new_node = bfl_malloc(bfl, size);
    memcpy(new_node, node, nodesize);
    // TODO: put node in bfl
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
  return (size_t)(MultiplyDeBruijnBitPosition[(uint32_t)(v * 0x07C4ACDDU) >> 27]);
}



