#include "vm/page.h"      /* for SPT structs + prototypes */
#include "devices/swap.h" /* later, when you integrate swap */
#include "lib/kernel/hash.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/frame.h" /* for struct frame */

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
