#ifndef VM_MMAP_H
#define VM_MMAP_H

#include <stdbool.h>
#include "lib/kernel/hash.h"
#include "lib/stdbool.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"

typedef int32_t mmapid_t;

/* Struct to store in mmap_table in processes */
struct mmap_entry {
  uint32_t mmap_id;
  void* vaddr;
  uint32_t page_count;
  struct file* file;
  struct hash_elem elem;
};


unsigned hash_mmap_table(const struct hash_elem* e, void* aux UNUSED);
bool hash_less_mmap_table(const struct hash_elem* a,
                                 const struct hash_elem* b, void* aux UNUSED);
void destroy_mmap_table(struct hash_elem* e, void* aux UNUSED);

mmapid_t mmap_add_file(int, void*);
void mmap_destroy_entry(struct mmap_entry*);
void mmap_destroy_entry_by_id(mmapid_t);
void mmap_destroy_all(void);

#endif /* vm/mmap.h */