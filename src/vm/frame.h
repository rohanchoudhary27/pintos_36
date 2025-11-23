#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "lib/kernel/hash.h"
#include "lib/stdbool.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/process.h"

struct frame_table {
  struct hash table;
  struct lock frame_table_lock;
};

/* Global frame table. */
static struct frame_table ft;

enum FRAME_KIND { FRAME_EXEC, FRAME_MMAP, FRAME_STACK };

struct frame {
  void* k_addr; /* Kernel virtual address of the frame (from pallog_get_page) */
  bool pinned;  /* Flag to set if the frame is pinned or not (if pinned, cannot
                   evict the frame) */
  enum FRAME_KIND f_kind;             /* Frame kind */
  struct list process_frame_mappings; /* List of processes that own the frame */
  struct hash_elem elem;
};

struct process_frame_mapping {
  struct process* process; /* Process that owns the frame */
  void* user_vaddr;        /* The owning user virtual page, rounded down */
  struct list_elem elem;   /* to put this in the process_frame_mappings list */
};

void frame_table_init(void);
void frame_table_destroy(void);

void* frame_alloc(enum palloc_flags, enum FRAME_KIND, struct process*, void*);
void frame_free(void*);

bool frame_add_mapping(void*, struct process*, void*);
void frame_free_mapping(void*, struct process*, void*);
void frame_free(void*);

struct frame* frame_lookup(void*);
struct frame* frame_lookup_locked(void*);
void frame_pin(void*);
void frame_unpin(void*);

#endif /* vm/frame.h */
