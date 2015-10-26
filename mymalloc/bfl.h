#ifndef _BFL_H
#define _BFL_H

#define BFL_SIZE 32

size_t lg2(uint32_t n);

typedef struct freelist {
  struct freelist* next;
} freelist;

void freelist_free(freelist* fl);

// split freelist into two freelists
void freelist_split(freelist* fl, size_t size);

typedef freelist** binned_free_list;

binned_free_list bfl_new();

void bfl_delete(binned_free_list* bfl);

// bfl has non-nill freelist at depth k, will split free node into depth k-1
void bfl_single_split(binned_free_list* bfl, size_t k);

void* bfl_malloc(binned_free_list* bfl, size_t size);

void bfl_free(binned_free_list* bfl, void* node);

void* bfl_realloc(binned_free_list* bfl, void* node, size_t size);

#endif
