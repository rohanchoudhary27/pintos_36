#include "vm/frame.h"
#include "devices/swap.h"
#include "lib/kernel/list.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/page.h"
#include <bitmap.h>
#include <debug.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEBUG_FR 1

#ifdef DEBUG_FR

#define DBG(fmt, ...)                                                          \
  do {                                                                         \
    printf("[%s:%d | %s] " fmt "\n", __FILE__, __LINE__, __func__,             \
           ##__VA_ARGS__);                                                     \
  } while (0)
#else
#define DBG(fmt, ...)                                                          \
  do {                                                                         \
  } while (0)
#endif

static struct frame* frame_to_evict(void);
static unsigned hash_frame_table(const struct hash_elem*, void*);
static bool hash_less_frame_table(const struct hash_elem*,
                                  const struct hash_elem*, void*);
static void frame_destructor(struct hash_elem* e, void* aux);
static bool frame_evict(void);
static struct frame* frame_to_evict(void);

static struct list frame_list;
static struct list_elem* clock_hand;

static struct frame* frame_to_evict(void) {

  // Aquire lock before the loop as we are looking at a shared list
  // lock_acquire(&ft.frame_table_lock);
  struct frame* to_evict = NULL;
  // Decide what to pick
  while (to_evict == NULL) {

    if (clock_hand == NULL || clock_hand == list_end(&frame_list)) {
      clock_hand = list_begin(&frame_list);
    }
    // Initally this will be the start, but as we go around the clock it will
    // increment
    struct frame* f = list_entry(clock_hand, struct frame, clock_elem);

    // Skip pinned frames
    if (f->pinned) {
      clock_hand = list_next(clock_hand);
      continue;
    }

    // Check if this is being shared by other processes
    bool recently_used = false;

    struct list_elem* e;

    for (e = list_begin(&f->process_frame_mappings);
         e != list_end(&f->process_frame_mappings); e = list_next(e)) {
      struct process_frame_mapping* pfm =
          list_entry(e, struct process_frame_mapping, elem);

      if (pagedir_is_accessed(pfm->process->pagedir, pfm->user_vaddr)) {
        recently_used = true;

        pagedir_set_accessed(pfm->process->pagedir, pfm->user_vaddr, false);
      }

      if (recently_used) {
        clock_hand = list_next(clock_hand);
      } else {
        to_evict = f;

        clock_hand = list_next(clock_hand);
      }
    }
  }

  // Release shared lock
  // lock_release(&ft.frame_table_lock);

  // Return the process to evict
  return to_evict;
}

static bool frame_evict(void) {

  lock_acquire(&ft.frame_table_lock);

  struct frame* evict = frame_to_evict(); // uses the clock algorithm

  if (evict == NULL) {
    lock_release(&ft.frame_table_lock);
    return false;
  }

  hash_delete(&ft.table, &evict->elem);

  /* detach the frame from the global frame table, and move all
      (process, upage) mappings into a local list so we can work on
      them without holding the frame_table_lock */
  struct list mappings;
  list_init(&mappings);
  while (!list_empty(&evict->process_frame_mappings)) {
    struct list_elem* e = list_pop_front(&evict->process_frame_mappings);
    list_push_back(&mappings, e);
  }

  lock_release(&ft.frame_table_lock);

  if (list_empty(&mappings)) {
    /* a frame wihtout owners should not happen, so free it */
    palloc_free_page(evict->k_addr);
    free(evict);
    return true;
  }

  struct process_frame_mapping* first_pfm =
      list_entry(list_front(&mappings), struct process_frame_mapping, elem);
  struct process* first_proc = first_pfm->process;
  void* first_upage = first_pfm->user_vaddr;

  /* locked, race-condition free lookup. */
  struct supp_page_table_entry* first_spte =
      spt_lookup(first_proc->spt, first_upage);
  ASSERT(first_spte != NULL);

  /* Cases to consider while evicting
    (1) EXEC type, clean pages (includes read only, shared pages): Drop
    (2) EXEC type, dirty pages: Swap out
    (3) MMAP type, writable, dirty pages: Write to filesys
    (4) MMAP type, non writeable, dirty pages: Swap out
    (5) MMAP type, writable or nonwriteable, clean pages: Drop
    (6) ANON type pages: Swap out */
  bool is_mmap = (first_spte->kind == SPTE_MMAP);
  bool is_exec = (first_spte->kind == SPTE_EXEC);
  bool is_file_backed =
      (first_spte->file != NULL && first_spte->read_bytes > 0 && is_exec) ||
      is_mmap;
  bool can_write_back = (is_mmap && first_spte->file_writable);

  /* check if the page is dirty in any of the owning pairs */
  bool dirty = false;
  struct list_elem* e;
  for (e = list_begin(&mappings); e != list_end(&mappings); e = list_next(e)) {
    struct process_frame_mapping* pfm =
        list_entry(e, struct process_frame_mapping, elem);
    struct process* proc = pfm->process;
    void* upage = pfm->user_vaddr;

    if (pagedir_is_dirty(proc->pagedir, upage) ||
        pagedir_is_dirty(proc->pagedir, evict->k_addr)) {
      dirty = true;
      break;
    }
  }

  bool drop_without_swap = false;
  bool use_swap = false;
  bool is_ro_shared =
      (is_file_backed && !dirty && !first_spte->writable && is_exec);
  size_t slot = BITMAP_ERROR;

  if (is_file_backed && !dirty) {
    /* Clean, file-backed (exec or data/mmap): contents still match the file, so
        we can safely drop the frame. Corresponds to cases (1) and (5) above */
    drop_without_swap = true;
  } else if (!is_file_backed) {
    /* Anonymous page (stack/heap): always needs swap. Corresponds to case (6)
     * above */
    use_swap = true;
  } else { /* is_file_backed && dirty */
    if (can_write_back) {
      /* Writable MMAP: spec wants modifications to be visible in the file.
         Simplest: write back now and then drop the frame as clean. Corresponds
         to case (3) above */
      lock_acquire(&global_file_lock); // TODO global file lock???????
      file_seek(first_spte->file, first_spte->file_offset);
      file_write(first_spte->file, evict->k_addr, first_spte->read_bytes);
      lock_release(&global_file_lock);

      drop_without_swap = true;
      dirty = false;
    } else {
      /* Dirty exec or non-writable file-backed: treat like anon and
         store in swap. Corresponds to cases (2), (4) above. */
      use_swap = true;
    }
  }

  if (use_swap) {
    /* we store it in swap once, and all SPTEs will point to the same slot */
    slot = swap_out(evict->k_addr);
    if (slot == BITMAP_ERROR) {
      /* Reattach the mapping to keep frame table consistent */
      lock_acquire(&ft.frame_table_lock);
      while (!list_empty(&mappings)) {
        struct list_elem* el = list_pop_front(&mappings);
        list_push_back(&evict->process_frame_mappings, el);
      }
      hash_insert(&ft.table, &evict->elem);
      lock_release(&ft.frame_table_lock);
      return false;
    }
  } else {
    /* we simply drop the frame. Each process will lazy-load the page again from
        the executable when it faults next time. */
    if (drop_without_swap && is_ro_shared) {
      shared_page_ref_dec(first_spte->file, first_spte->file_offset);
    }
  }

  for (e = list_begin(&mappings); e != list_end(&mappings); e = list_next(e)) {
    struct process_frame_mapping* pfm =
        list_entry(e, struct process_frame_mapping, elem);
    struct process* proc = pfm->process;
    void* upage = pfm->user_vaddr;

    struct supp_page_table_entry* spte = spt_lookup(proc->spt, upage);
    ASSERT(spte != NULL);

    // TODO: race conditions in pagedir_clear_page?
    pagedir_clear_page(proc->pagedir, upage);

    spte->in_swap = use_swap;
    spte->swap_slot = (spte->in_swap) ? slot : 0;
    spt_clear_frame(spte);
  }

  /* free the per-mapping structs and the frame itself. */
  while (!list_empty(&mappings)) {
    struct list_elem* el = list_pop_front(&mappings);
    struct process_frame_mapping* pfm =
        list_entry(el, struct process_frame_mapping, elem);
    free(pfm);
  }

  palloc_free_page(evict->k_addr);
  free(evict);

  return true;
}

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
  list_init(&frame_list);
  clock_hand = NULL;
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

  if (clock_hand == &f->clock_elem) {
    clock_hand = list_next(clock_hand);
  }
  list_remove(&f->clock_elem);

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
    if (!frame_evict())
      return NULL;
    /* try again, if it fails again we couldn't evict something
        and something's wrong */
    k_addr = palloc_get_page(flags | PAL_USER);
    if (k_addr == NULL)
      return NULL; /* something is seriously wrong */
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
  struct hash_elem* old = hash_insert(&ft.table, &f->elem);
  ASSERT(old == NULL);
  list_push_back(&frame_list, &f->clock_elem);
  lock_release(&ft.frame_table_lock);

  return k_addr;
}

/* Add another (process, user_vaddr) mapping to an existing frame.
    Used for read-only page sharing (same frame mapped into multiple
    processes). Returns true on success, false on allocation failure. */
bool frame_add_mapping(void* k_addr, struct process* proc, void* uaddr) {
  ASSERT(proc != NULL);

  lock_acquire(&ft.frame_table_lock);
  struct frame* f = frame_lookup_locked(k_addr);
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
    case FRAME_ANON:
      return "ANON";
    default:
      return "UNKNOWN";
  }
}

void frame_table_print(void) {
#ifndef DEBUG_FR
  return;
#endif
  lock_acquire(&ft.frame_table_lock);

  printf("\n==================== FRAME TABLE ====================\n");
  printf("%-12s %-8s %-6s  %s\n", "k_addr", "kind", "pinned",
         "mappings (process , user_vaddr)");

  if (hash_empty(&ft.table)) {
    printf("[no frames]\n");
    printf("======================================================\n\n");
    lock_release(&ft.frame_table_lock);
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