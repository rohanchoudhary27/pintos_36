#include "vm/page.h"      /* for SPT structs + prototypes */
#include "devices/swap.h" /* later, when you integrate swap */
#include "filesys/file.h"
#include "lib/kernel/hash.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/frame.h" /* for struct frame */
#include <stdio.h>
#include <string.h>

#define DEBUG_PC 1

#ifdef DEBUG_PC

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

static unsigned hash_read_only_table(const struct hash_elem*, void*);
static bool hash_less_read_only_table(const struct hash_elem*,
                                      const struct hash_elem*, void*);

/* ----------------------------------------------------------------------
 *  Hash helpers
 * --------------------------------------------------------------------*/

/* Hash by user page address (upage). */
static unsigned spt_hash(const struct hash_elem* e, void* aux UNUSED) {
  const struct supp_page_table_entry* spte =
      hash_entry(e, struct supp_page_table_entry, elem);
  return hash_bytes(&spte->upage, sizeof spte->upage);
}

/* Order by upage address. */
static bool spt_less(const struct hash_elem* a, const struct hash_elem* b,
                     void* aux UNUSED) {
  const struct supp_page_table_entry* spte_a =
      hash_entry(a, struct supp_page_table_entry, elem);
  const struct supp_page_table_entry* spte_b =
      hash_entry(b, struct supp_page_table_entry, elem);
  return spte_a->upage < spte_b->upage;
}

/* Read_only_table hash functions */

static unsigned hash_read_only_table(const struct hash_elem* e, void*) {
  struct read_only_entry* read_only_entry =
      hash_entry(e, struct read_only_entry, elem);

  unsigned hash1 = hash_ptr(read_only_entry->file);
  unsigned hash2 = hash_int(read_only_entry->offset);

  // Return exclusive or (^) -arbitrary func- as demonstrated in example in
  // spec
  return hash1 ^ hash2;
}

static bool hash_less_read_only_table(const struct hash_elem* a,
                                      const struct hash_elem* b, void*) {
  struct read_only_entry* read_only_entry_a =
      hash_entry(a, struct read_only_entry, elem);
  struct read_only_entry* read_only_entry_b =
      hash_entry(b, struct read_only_entry, elem);

  struct file* file_a = read_only_entry_a->file;
  struct file* file_b = read_only_entry_b->file;

  // Compare both the keys in comparision
  if (file_a != file_b) {
    return (file_a < file_b);
  } else {
    return (read_only_entry_a->offset < read_only_entry_b->offset);
  }
}

/* ----------------------------------------------------------------------
 *  SPT lifecycle
 * --------------------------------------------------------------------*/

void spt_init(struct supplemental_page_table* spt) {
  ASSERT(spt != NULL);
  hash_init(&spt->page_map, spt_hash, spt_less, NULL);
  lock_init(&spt->lock);
}

/* Helper used by spt_destroy: free a single SPTE.
   NOTE: This only frees the *SPT entry itself*.
   The caller is responsible for making sure any associated frame/swap
   has already been cleaned up (or will be cleaned up separately). */
static void spt_free_spte(struct hash_elem* e, void* aux UNUSED) {
  struct supp_page_table_entry* spte =
      hash_entry(e, struct supp_page_table_entry, elem);

  /* Sanity: at this point we expect that no frame still points to this SPTE
     and any swap slot has been released. */
  ASSERT(spte->frame == NULL ||
         !"frame should have been detached before destroy");

  free(spte);
}

/* Destroy the whole SPT: frees all SPTE structs.
   Call this from process_exit *after* you have walked the SPT
   and cleaned up frames / swap / mmap as needed. */
void spt_destroy(struct supplemental_page_table* spt) {
  ASSERT(spt != NULL);
  lock_acquire(&spt->lock);
  hash_destroy(&spt->page_map, spt_free_spte);
  lock_release(&spt->lock);
}

/* ----------------------------------------------------------------------
 *  SPT API functions
 * --------------------------------------------------------------------*/

/* Internal lookup: expects upage to be page-aligned.
   Does NOT acquire the SPT lock. */
static struct supp_page_table_entry*
spt_lookup_internal(struct supplemental_page_table* spt, void* upage) {
  struct supp_page_table_entry tmp;
  struct hash_elem* e;

  tmp.upage = upage;
  e = hash_find(&spt->page_map, &tmp.elem);
  if (e == NULL)
    return NULL;
  return hash_entry(e, struct supp_page_table_entry, elem);
}

/* Look up SPTE for user address UADDR.
   Acquires and releases the SPT lock internally. */
struct supp_page_table_entry* spt_lookup(struct supplemental_page_table* spt,
                                         const void* uaddr) {
  ASSERT(spt != NULL);
  void* upage = pg_round_down(uaddr);

  lock_acquire(&spt->lock);
  struct supp_page_table_entry* spte = spt_lookup_internal(spt, upage);
  lock_release(&spt->lock);

  return spte;
}

/* Same as spt_lookup, but assumes the caller already holds spt->lock.
   Useful when you want to do several operations atomically. */
struct supp_page_table_entry*
spt_lookup_locked(struct supplemental_page_table* spt, const void* uaddr) {
  ASSERT(spt != NULL);
  void* upage = pg_round_down(uaddr);
  return spt_lookup_internal(spt, upage);
}

/* Insert a freshly allocated SPTE into the SPT.
   Returns true on success, false if an entry already exists for this upage. */
static bool spt_insert(struct supplemental_page_table* spt,
                       struct supp_page_table_entry* spte) {
  ASSERT(spt != NULL);
  ASSERT(spte != NULL);
  ASSERT(spte->upage == pg_round_down(spte->upage));

  lock_acquire(&spt->lock);
  struct hash_elem* old = hash_insert(&spt->page_map, &spte->elem);
  lock_release(&spt->lock);

  if (old != NULL) {
    return false;
  }
  return true;
}

/* Create an EXEC SPTE (lazy-loaded from an executable segment). */
struct supp_page_table_entry* spt_add_exec(struct supplemental_page_table* spt,
                                           void* upage, struct file* file,
                                           off_t file_offset, size_t read_bytes,
                                           size_t zero_bytes, bool writable) {
  struct supp_page_table_entry* spte = malloc(sizeof *spte);
  if (spte == NULL)
    return NULL;

  spte->upage = pg_round_down(upage);
  spte->kind = SPTE_EXEC;
  spte->writable = writable;
  spte->frame = NULL;
  spte->is_stack = false;

  spte->in_swap = false;
  spte->swap_slot = 0;

  spte->file = file;
  spte->file_offset = file_offset;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;
  spte->file_writable = writable;

  spte->mapid = -1; /* not an mmap page */

  if (!spt_insert(spt, spte)) {
    free(spte);
    return NULL;
  }
  return spte;
}

/* Create an anonymous SPTE (stack or heap page, initially zero). */
struct supp_page_table_entry* spt_add_anon(struct supplemental_page_table* spt,
                                           void* upage, bool writable,
                                           bool is_stack) {
  struct supp_page_table_entry* spte = malloc(sizeof *spte);
  if (spte == NULL)
    return NULL;

  spte->upage = pg_round_down(upage);
  spte->kind = SPTE_ANON;
  spte->writable = writable;
  spte->frame = NULL;
  spte->is_stack = is_stack;

  spte->in_swap = false;
  spte->swap_slot = 0;

  spte->file = NULL;
  spte->file_offset = 0;
  spte->read_bytes = 0;
  spte->zero_bytes = PGSIZE;
  spte->file_writable = false;

  spte->mapid = -1;

  if (!spt_insert(spt, spte)) {
    free(spte);
    return NULL;
  }
  return spte;
}

/* Create an MMAP SPTE (lazy-loaded from a memory-mapped file region). */
struct supp_page_table_entry* spt_add_mmap(struct supplemental_page_table* spt,
                                           void* upage, struct file* file,
                                           off_t file_offset, size_t read_bytes,
                                           size_t zero_bytes, bool writable,
                                           int mapid) {
  struct supp_page_table_entry* spte = malloc(sizeof *spte);
  if (spte == NULL)
    return NULL;

  spte->upage = pg_round_down(upage);
  spte->kind = SPTE_MMAP;
  spte->writable = writable;
  spte->frame = NULL;
  spte->is_stack = false; /* not stack */

  spte->in_swap = false;
  spte->swap_slot = 0;

  spte->file = file;
  spte->file_offset = file_offset;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;
  spte->file_writable = writable;

  spte->mapid = mapid;

  if (!spt_insert(spt, spte)) {
    free(spte);
    return NULL;
  }
  return spte;
}

/* Remove SPTE from the SPT and free it.
   Caller must ensure any frame / swap slot / mmap bookkeeping is already done.
 */
void spt_remove(struct supplemental_page_table* spt,
                struct supp_page_table_entry* spte) {
  ASSERT(spt != NULL);
  ASSERT(spte != NULL);

  lock_acquire(&spt->lock);
  hash_delete(&spt->page_map, &spte->elem);
  lock_release(&spt->lock);

  free(spte);
}

/* Record that this SPTE is now backed by FRAME. */
void spt_set_frame(struct supp_page_table_entry* spte, struct frame* frame) {
  ASSERT(spte != NULL);
  ASSERT(frame != NULL);
  spte->frame = frame;
  spte->in_swap = false;
}

/* Called when a frame is evicted from RAM. */
void spt_clear_frame(struct supp_page_table_entry* spte) {
  ASSERT(spte != NULL);
  spte->frame = NULL;
}

void spt_print(struct supplemental_page_table* spt) {
  ASSERT(spt != NULL);

  lock_acquire(&spt->lock);

  printf(
      "\n==================== SUPPLEMENTAL PAGE TABLE ====================\n");
  printf("%-12s %-8s %-9s %-10s %-10s  %-10s %-10s %-10s\n", "upage", "kind",
         "writable", "in_frame", "in_swap", "file_ofs", "read_bytes",
         "zero_bytes");

  if (hash_empty(&spt->page_map)) {
    printf("[no entries]\n");
    printf("================================================================="
           "\n\n");
    lock_release(&spt->lock);
    return;
  }

  struct hash_iterator it;
  hash_first(&it, &spt->page_map);

  while (hash_next(&it)) {
    struct supp_page_table_entry* spte =
        hash_entry(hash_cur(&it), struct supp_page_table_entry, elem);

    const char* kind_str = spte->kind == SPTE_EXEC   ? "EXEC"
                           : spte->kind == SPTE_ANON ? "ANON"
                           : spte->kind == SPTE_MMAP ? "MMAP"
                                                     : "UNK";

    const char* frame_str = spte->frame ? "yes" : "no";

    printf("%p  %-8s %-9s %-10s %-10s", spte->upage, kind_str,
           spte->writable ? "true" : "false", frame_str,
           spte->in_swap ? "true" : "false");

    /* For EXEC/MMAP pages, show file metadata. */
    if (spte->kind == SPTE_EXEC || spte->kind == SPTE_MMAP) {
      printf("  %-10d %-10zu %-10zu", (int)spte->file_offset, spte->read_bytes,
             spte->zero_bytes);
    } else {
      /* For ANON pages, we can print placeholders. */
      printf("  %-10s %-10s %-10s", "-", "-", "-");
    }

    printf("\n");

    /* If page is mapped in a frame, print the frame address too. */
    if (spte->frame) {
      printf("          -> frame kaddr: %p\n", spte->frame->k_addr);
    }
  }

  printf(
      "=================================================================\n\n");

  lock_release(&spt->lock);
}

/* ----------------------------------------------------------------------
 *  Read-Only Page Table Lifecycle (Global)
 * --------------------------------------------------------------------*/

void* shared_page_lookup(
    struct read_only_table*
        read_only_table, // Remember to change this once global
    struct file* file, off_t offset) {
  ASSERT(file != NULL)
  struct read_only_entry key;
  key.file = file;
  key.offset = offset;

  struct hash_elem* e = hash_find(&read_only_table->read_only_table, &key.elem);

  if (e != NULL) {
    struct read_only_entry* entry = hash_entry(e, struct read_only_entry, elem);
    return entry->page;
  }
  return NULL;
}

void shared_page_ref_inc(struct read_only_table* read_only_table,
                         struct file* file, off_t offset, void* frame) {
  struct read_only_entry key;
  key.file = file;
  key.offset = offset;

  struct hash_elem* e = hash_find(&read_only_table->read_only_table, &key.elem);
  if (e != NULL) {
    struct read_only_entry* entry = hash_entry(e, struct read_only_entry, elem);
    entry->reference_count++;
  }
}

void shared_page_ref_dec(struct read_only_table* read_only_table,
                         struct file* file, off_t offset) {
  struct read_only_entry key;
  key.file = file;
  key.offset = offset;

  struct hash_elem* e = hash_find(&read_only_table->read_only_table, &key.elem);
  if (e != NULL) {
    struct read_only_entry* entry = hash_entry(e, struct read_only_entry, elem);
    entry->reference_count--;

    if (entry->reference_count == 0) {
      hash_delete(&read_only_table, &entry->elem);
      free(entry);
    }
  }
}

/* ----------------------------------------------------------------------
 *  Task 3 functions -- Lazy Loading, Stack Growth
 * --------------------------------------------------------------------*/

bool lazy_load_page(struct supp_page_table_entry* spte) {
  struct process* p = thread_current()->process;
  void* upage = spte->upage;

  void* kpage;
  if (spte->kind != SPTE_EXEC)
    return false;

  bool success = true;

  /* Check for file-backed SPTE */
  if (spte->file == NULL ||
      !(spte->kind == SPTE_EXEC || spte->kind == SPTE_MMAP)) {
    return false;
  }

  ASSERT(spte->file != NULL);

  if (spte->zero_bytes == PGSIZE) { /* Do not read the file if it's all zeros */
    kpage = frame_alloc(PAL_USER | PAL_ZERO, FRAME_EXEC, p, upage);
    if (kpage == NULL)
      return false;
  } else {
    kpage = frame_alloc(PAL_USER, FRAME_EXEC, p, upage);
    if (kpage == NULL)
      return false;

    file_seek(spte->file, spte->file_offset);

    int bytes_read = file_read(spte->file, kpage, spte->read_bytes);

    if (bytes_read != (int)spte->read_bytes)
      success = false;
    memset(kpage + bytes_read, 0, spte->zero_bytes);
  }

  if (!success) {
    frame_free_mapping(kpage, p, upage);
    return false;
  }
  if (!install_page(upage, kpage, spte->writable)) {
    frame_free_mapping(kpage, p, upage);
    return false;
  }

  struct frame* f = frame_lookup(kpage);
  spt_set_frame(spte, f);
  spte->in_swap = false;
  return true;
}

/*
TODO: the slides say Beware: as with any other page, the stack can be swapped
out does this mean if it's in a swap slot it should be brought back? idk
*/
bool grow_stack(void* fault_addr, struct supplemental_page_table* spt) {
  struct process* p = thread_current()->process;
  void* upage = pg_round_down(fault_addr);

  struct supp_page_table_entry* new_spte = spt_add_anon(spt, upage, true, true);
  if (new_spte == NULL)
    return false;

  void* kpage = frame_alloc(PAL_USER | PAL_ZERO, FRAME_ANON, p, upage);
  if (kpage == NULL) {
    spt_remove(spt, new_spte);
    return false;
  }

  struct frame* f = frame_lookup(kpage);
  spt_set_frame(new_spte, f);

  if (!install_page(upage, kpage, new_spte->writable)) {
    spt_remove(spt, new_spte);
    frame_free_mapping(kpage, p, upage);
    return false;
  }
  return true;
}

bool load_and_pin_user_buffer_pages(void* buffer, unsigned size) {
  if (size == 0)
    return true;

  struct process* p = thread_current()->process;
  struct supplemental_page_table* spt = p->spt;

  uint8_t* start = pg_round_down(buffer);
  uint8_t* end = pg_round_down((uint8_t*)buffer + size - 1);

  for (uint8_t* upage = start; upage <= end; upage += PGSIZE) {
    struct supp_page_table_entry* spte = spt_lookup(spt, upage);
    if (spte == NULL)
      return false;

    if (spte->frame == NULL) { // not in frame yet, demand load
      if (spte->kind == SPTE_EXEC || spte->kind == SPTE_MMAP) {
        if (!lazy_load_page(spte))
          return false;
      } else if (spte->kind == SPTE_ANON && spte->in_swap) {
        // TODO: get from swap
        return false;
      }
    }
    ASSERT(spte->frame != NULL); // now we should load it in memory
    frame_pin(spte->frame->k_addr);
  }
  return true;
}

void unpin_user_buffer_pages(void* buffer, unsigned size) {
  if (size == 0)
    return;

  struct process* p = thread_current()->process;
  struct supplemental_page_table* spt = p->spt;

  uint8_t* start = pg_round_down(buffer);
  uint8_t* end = pg_round_down((uint8_t*)buffer + size - 1);

  for (uint8_t* upage = start; upage <= end; upage += PGSIZE) {
    struct supp_page_table_entry* spte = spt_lookup(spt, upage);
    if (spte != NULL && spte->frame != NULL) // already in frame
      frame_unpin(spte->frame->k_addr);
  }
}