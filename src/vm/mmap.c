#include "mmap.h"

/* Hash calculation function for mmap table. */
unsigned hash_mmap_table(const struct hash_elem* e, void* aux UNUSED) {
  struct mmap_entry* mmap_entry = hash_entry(e, struct mmap_entry, elem);
  return hash_int(mmap_entry->mmap_id);
}

/* Comparison function for mmap table. */
bool hash_less_mmap_table(const struct hash_elem* a,
                                 const struct hash_elem* b, void* aux UNUSED) {
  struct mmap_entry* mmap_entry_a = hash_entry(a, struct mmap_entry, elem);
  struct mmap_entry* mmap_entry_b = hash_entry(b, struct mmap_entry, elem);
  return (mmap_entry_a->mmap_id < mmap_entry_b->mmap_id);
}

void destroy_mmap_table(struct hash_elem* e, void* aux UNUSED) {
  struct mmap_entry* mmap_entry = hash_entry(e, struct mmap_entry, elem);
  mmap_destroy_entry(mmap_entry);
}


mmapid_t mmap_add_file(int fd, void *vaddr) {
  struct process* cur_process = thread_current()->process;
  int mmap_id = cur_process->next_mmap_id++;

  /* ----------------------------------------------------------------------
   *  Syscall Error Checks
   * --------------------------------------------------------------------*/

  // Check if its in user vaddr
  if (!is_user_vaddr(vaddr)) {
    return SYS_ERROR;
  }

  // Fail if addr is not page aligned or addr equals to 0
  if ((uintptr_t)vaddr % PGSIZE || (uintptr_t)vaddr == 0) {
    return SYS_ERROR;
  }

  // STDIO streams cannot be mapped so fail if their fd's are given
  if (fd < FD_BEGIN) {
    return SYS_ERROR;
  }

  // For any other case set the position on files
  struct file* tmp_file = process_get_file(fd);
  if (tmp_file == NULL) { // Return if the file is NULL
    return SYS_ERROR;
  }

  /* We reopen the file in order to not lose it if the process
     closes the original file. ANY FAILURE AFTER THIS POINT
     MUST CLOSE THIS FILE. */
  struct file* file = file_reopen(tmp_file);
  if (file == NULL) {
    return SYS_ERROR; // If we get null return error.
  }

  size_t cur_file_length = file_length(file);
  size_t page_count = (cur_file_length + PGSIZE - 1) / PGSIZE;

  // If file we are mapping has length 0 fail immediately
  if (cur_file_length == 0) {
    file_close(file);
    return SYS_ERROR;
  }

  // Make sure we don't go in stack reserved territory in memory
  if (vaddr + page_count * PGSIZE >= PHYS_BASE - MAX_STACK_SIZE) {
    file_close(file);
    return SYS_ERROR;
  }

  struct process* current_process = thread_current()->process;
  struct supplemental_page_table* spt = current_process->spt;

  // Find if there is something else mapped on these pages
  for (uintptr_t i = 0; i < cur_file_length; i += PGSIZE) {
    if (spt_lookup(spt, vaddr + i) != NULL) { // The addr is occupied
      file_close(file);
      return SYS_ERROR; // Then we can't mmap here so return SYS_ERROR
    }
  }

  /* ----------------------------------------------------------------------
   *  Map File to Virtual Address and Add to
   * --------------------------------------------------------------------*/

  /* Loop through all full pages we need to add to virtual memory */
  uintptr_t i;
  for (i = 0; i < cur_file_length; i += PGSIZE) {
    size_t set_ready_bytes =
        (i + PGSIZE <= cur_file_length) ? PGSIZE : cur_file_length % PGSIZE;

    /* Here we check for cases where spt_add_mmap fails */
    if (spt_add_mmap(
            cur_process->spt, // The current process' spt
            vaddr + i,         // The address of the current page added
            file,             // The original file this data is taken from
            i,                // The offset is i
            set_ready_bytes,  // Adding the only non page sized data from file
            PGSIZE - set_ready_bytes, // Rest of bytes in page are zero_bytes
            true,                     // mmaps are always writable
            mmap_id                   // Id of the mmap
            ) == NULL) {

      /* Start from the first page start point until
         the one before the last one we added and remove
         every page we added to spt. */
      for (uintptr_t j = 0; j < i; j += PGSIZE) {
        struct supp_page_table_entry* spte =
            spt_lookup(cur_process->spt, vaddr + j);

        if (spte == NULL)
          continue;

        spt_remove(cur_process->spt, spte);
      }

      file_close(file);
      return SYS_ERROR;
    }
  }

  /* Pass a new entry to mmap_table */
  struct mmap_entry* new_entry = malloc(sizeof *new_entry);
  new_entry->mmap_id = mmap_id;
  new_entry->vaddr = vaddr;
  new_entry->page_count = page_count;
  new_entry->file = file;

  hash_insert(&cur_process->mmap_table, &new_entry->elem);

  return mmap_id;
}

void mmap_destroy_entry(struct mmap_entry* mmap_entry) {
  struct process* cur_process = thread_current()->process;

  intptr_t cur_vaddr = (intptr_t)mmap_entry->vaddr;
  for (uint32_t j = 0; j < mmap_entry->page_count; j++, cur_vaddr += PGSIZE) {
    struct supp_page_table_entry* spte =
        spt_lookup(cur_process->spt, (void*)cur_vaddr);

    if (spte == NULL)
      continue;

    /* If the page is in a frame (in memory) */
    if (spte->frame != NULL) {
      void* kpage = spte->frame->k_addr;

      // TODO: Maybe check this part again later.
      if (pagedir_is_dirty(cur_process->pagedir, (void*)cur_vaddr)) {
        file_write_at(spte->file, kpage, spte->read_bytes, spte->file_offset);
      }

      pagedir_clear_page(cur_process->pagedir, (void*)cur_vaddr);
      frame_free_mapping(kpage, cur_process, (void*)cur_vaddr);
    }

    spt_remove(cur_process->spt, spte);
  }

  hash_delete(&cur_process->mmap_table, &mmap_entry->elem);
  file_close(mmap_entry->file);
  free(mmap_entry);
}

void mmap_destroy_entry_by_id(mmapid_t mmap_id) {
  struct process* cur_process = thread_current()->process;

  struct mmap_entry mock_entry;
  mock_entry.mmap_id = mmap_id;
  struct hash_elem* e = hash_find(&cur_process->mmap_table, &mock_entry.elem);
  if (e == NULL)
    return;

  struct mmap_entry* mmap_entry = hash_entry(e, struct mmap_entry, elem);

  mmap_destroy_entry(mmap_entry);
}