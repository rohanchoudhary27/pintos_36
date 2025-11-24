#include "vm/frame.h"
#include "lib/kernel/list.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include <debug.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static unsigned hash_frame_table(const struct hash_elem*, void*);
static bool hash_less_frame_table(const struct hash_elem*,
                                  const struct hash_elem*, void*);
static void frame_destructor(struct hash_elem* e, void* aux);

/* ----------------------------------------------------------------------
 *  Hash helpers
 * --------------------------------------------------------------------*/

/* Hash calculation function for frame in frame_table hashmap. */
static unsigned hash_frame_table(const struct hash_elem* e, void* aux UNUSED) {
  struct frame* frame = hash_entry(e, struct frame, elem);
  return hash_bytes(&frame->k_addr, sizeof(frame->k_addr));
}

/* Comparison function for frames in frame_table hashmap. */
static bool hash_less_frame_table(const struct hash_elem* a,
                               const struct hash_elem* b, void* aux UNUSED) {
  struct frame* frame_a = hash_entry(a, struct frame, elem);
  struct frame* frame_b = hash_entry(b, struct frame, elem);
  return (frame_a->k_addr < frame_b->k_addr);
}

/* ----------------------------------------------------------------------
 *  Frame table lifecycle
 * --------------------------------------------------------------------*/

void frame_table_init(void) {
  lock_init(&ft.frame_table_lock);
  hash_init(&ft.table, hash_frame_table, hash_less_frame_table, NULL);
}

void frame_table_destroy(void) {
  lock_acquire(&ft.frame_table_lock);
  hash_destroy(&ft.table, frame_destructor);
  lock_release(&ft.frame_table_lock);
}

/* Destructor for hash_destroy: frees all frames, their mappings,
    and their underlying physical pages. */
static void frame_destructor(struct hash_elem* e, void* aux UNUSED) {
  struct frame* f = hash_entry(e, struct frame, elem);

  while (!list_empty(&f->process_frame_mappings)) {
    struct list_elem* le = list_pop_front(&f->process_frame_mappings);
    struct process_frame_mapping* pfm =
        list_entry(le, struct process_frame_mapping, elem);
    free(pfm);
  }

  palloc_free_page(f->k_addr);
  free(f);
}

/* Drop a specific (process, user_vaddr) mapping from the frame
    identified by k_addr.
    If this was the last mapping, the frame entry is removed from the
    global table and the underlying physical page is freed. */
void frame_free_mapping(void* k_addr, struct process* proc, void* uaddr) {
  ASSERT(proc != NULL);

  struct frame* f;
  struct process_frame_mapping* pfm = NULL;

  uaddr = pg_round_down(uaddr);

  lock_acquire(&ft.frame_table_lock);

  f = frame_lookup_locked(k_addr);
  ASSERT(f != NULL);

  struct list_elem* e;
  for (e = list_begin(&f->process_frame_mappings);
       e != list_end(&f->process_frame_mappings); e = list_next(e)) {
    struct process_frame_mapping* cur =
        list_entry(e, struct process_frame_mapping, elem);
    if (cur->process == proc && cur->user_vaddr == uaddr) {
      list_remove(&cur->elem);
      pfm = cur;
      break;
    }
  }

  ASSERT(pfm != NULL);

  bool last_mapping = list_empty(&f->process_frame_mappings);
  if (last_mapping) {
    hash_delete(&ft.table, &f->elem);
  }

  lock_release(&ft.frame_table_lock);

  free(pfm);

  if (last_mapping) {
    palloc_free_page(f->k_addr);
    free(f);
  }
}

/* Free the whole frame (all mappings). */
void frame_free(void* k_addr) {
  lock_acquire(&ft.frame_table_lock);

  struct frame* f = frame_lookup_locked(k_addr);
  if (f == NULL) {
    lock_release(&ft.frame_table_lock);
    return;
  }

  hash_delete(&ft.table, &f->elem);

  struct list pfm_list = f->process_frame_mappings;
  list_init(&f->process_frame_mappings); // clear the list

  lock_release(&ft.frame_table_lock);

  /* Free all process_frame_mapping structures. */
  while (!list_empty(&pfm_list)) {
    struct list_elem* e = list_pop_front(&pfm_list);
    struct process_frame_mapping* pfm =
        list_entry(e, struct process_frame_mapping, elem);
    free(pfm);
  }

  palloc_free_page(f->k_addr);
  free(f);
}

/* ----------------------------------------------------------------------
 *  Frame allocations
 * --------------------------------------------------------------------*/

/* Allocate a user frame and create a frame-table entry for it. The flags enum
    MUST include PAL_USER. Returns the kernel virtual address of the frame on
    success, or NULL on failure (e.g. out of user-pool pages). Eviction will be
    plugged in here later. */
void* frame_alloc(enum palloc_flags flags, enum FRAME_KIND kind,
                  struct process* proc, void* upage) {
  ASSERT(proc != NULL);
  ASSERT(flags & PAL_USER); // only manage user frames

  void* k_addr = palloc_get_page(flags | PAL_USER);

  if (k_addr == NULL) {
    // eviction later here
    return NULL;
  }

  struct frame* f = malloc(sizeof *f);
  if (f == NULL) {
    palloc_free_page(k_addr);
    return NULL;
  }

  f->k_addr = k_addr;
  f->pinned = false;
  f->f_kind = kind;
  list_init(&f->process_frame_mappings);
  struct process_frame_mapping* pfm = malloc(sizeof *pfm);
  if (pfm == NULL) {
    free(f);
    palloc_free_page(k_addr);
    return NULL;
  }

  pfm->process = proc;
  pfm->user_vaddr = pg_round_down(upage);

  lock_acquire(&ft.frame_table_lock);
  list_push_back(&f->process_frame_mappings, &pfm->elem);
  struct hash_elem* old UNUSED = hash_insert(&ft.table, &f->elem);
  ASSERT(old == NULL);
  lock_release(&ft.frame_table_lock);

  return k_addr;
}

/* Add another (process, user_vaddr) mapping to an existing frame.
    Used for read-only page sharing (same frame mapped into multiple
    processes). Returns true on success, false on allocation failure. */
bool frame_add_mapping(void* k_addr, struct process* proc, void* uaddr) {
  ASSERT(proc != NULL);

  lock_acquire(&ft.frame_table_lock);
  struct frame* f = frame_lookup(k_addr);
  if (f == NULL) {
    lock_release(&ft.frame_table_lock);
    return false;
  }

  struct process_frame_mapping* pfm = malloc(sizeof *pfm);
  if (pfm == NULL) {
    lock_release(&ft.frame_table_lock);
    return false;
  }

  pfm->process = proc;
  pfm->user_vaddr = pg_round_down(uaddr);
  list_push_back(&f->process_frame_mappings, &pfm->elem);
  lock_release(&ft.frame_table_lock);

  return true;
}

/* ----------------------------------------------------------------------
 *  Frame / Frame Table API functions
 * --------------------------------------------------------------------*/

/* Lookup the frame structure for a given kernel address.
    Returns NULL if not found. This is a read-only lookup, so we only briefly
    hold the lock. */
struct frame* frame_lookup(void* k_addr) {
  lock_acquire(&ft.frame_table_lock);
  struct frame* f = frame_lookup_locked(k_addr);
  lock_release(&ft.frame_table_lock);
  return f;
}

/* Look up a frame by kernel address. Callers MUST HOLD ft.frame_table_lock. */
struct frame* frame_lookup_locked(void* k_addr) {
  struct frame tmp;
  struct hash_elem* e;

  tmp.k_addr = k_addr;
  e = hash_find(&ft.table, &tmp.elem);
  if (e == NULL)
    return NULL;
  return hash_entry(e, struct frame, elem);
}

/* Pin / unpin frames to temporarily prevent eviction. */
void frame_pin(void* k_addr) {
  lock_acquire(&ft.frame_table_lock);
  struct frame* f = frame_lookup_locked(k_addr);
  if (f != NULL)
    f->pinned = true;
  lock_release(&ft.frame_table_lock);
}

void frame_unpin(void* k_addr) {
  lock_acquire(&ft.frame_table_lock);
  struct frame* f = frame_lookup_locked(k_addr);
  if (f != NULL)
    f->pinned = false;
  lock_release(&ft.frame_table_lock);
}

/* ----------------------------------------------------------------------
 *  Debug helper: print the entire frame table in a readable format
 * --------------------------------------------------------------------*/

static const char* frame_kind_to_str(enum FRAME_KIND k) {
  switch (k) {
    case FRAME_EXEC:
      return "EXEC";
    case FRAME_MMAP:
      return "MMAP";
    case FRAME_STACK:
      return "STACK";
    default:
      return "UNKNOWN";
  }
}

void frame_table_print(void) {
  lock_acquire(&ft.frame_table_lock);

  printf("\n==================== FRAME TABLE ====================\n");
  printf("%-12s %-8s %-6s  %s\n", "k_addr", "kind", "pinned",
         "mappings (process , user_vaddr)");

  if (hash_empty(&ft.table)) {
    printf("[no frames]\n");
    return;
  }

  struct hash_iterator it;
  hash_first(&it, &ft.table);

  while (hash_next(&it)) {
    struct frame* f = hash_entry(hash_cur(&it), struct frame, elem);

    printf("%p  %-8s %-6s  ", f->k_addr, frame_kind_to_str(f->f_kind),
           f->pinned ? "yes" : "no");

    /* Print all mappings for this frame */
    struct list_elem* e;
    if (list_empty(&f->process_frame_mappings)) {
      printf("[no mappings]\n");
    } else {
      bool first = true;
      for (e = list_begin(&f->process_frame_mappings);
           e != list_end(&f->process_frame_mappings); e = list_next(e)) {
        struct process_frame_mapping* pfm =
            list_entry(e, struct process_frame_mapping, elem);

        if (!first)
          printf("%46s", ""); /* indentation for subsequent mappings */

        printf("(proc=%s , upage=%p)\n", pfm->process->thread->name,
               pfm->user_vaddr);

        first = false;
      }
    }
  }

  printf("======================================================\n\n");

  lock_release(&ft.frame_table_lock);
}