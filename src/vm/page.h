#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdbool.h>
#include "lib/kernel/hash.h"
#include "lib/stdbool.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/process.h"

enum SPTE_KIND {
    SPTE_ANON,   /* Anon page (stack/heap/zero page) */
    SPTE_EXEC,   /* File-backed page from an executable (code/data segments) */
    SPTE_MMAP    /* File-backed page from a memory-mapped file */
};

/* Per process supplemental page table. */
struct supplemental_page_table {
    struct hash page_map;    /* Hash: upage -> struct supp_page_table_entry. */
    struct lock lock;        /* Protects page_map and its entries. */
};


struct supp_page_table_entry {
    void *upage;    /* key: user virtual page */
    enum SPTE_KIND kind; /* page kind */
    bool writable;  /* may this page be written to by user program */
    
    /* if frame != NULL, page is in frame */
    struct frame *frame;  /* Is this page currently in a frame?  (NULL if not -- swapped or not loaded yet). */
    bool is_stack;         /* true if this anon page is part of the stack. */

    /* if in_swap == true, page is in swap disk */
    bool in_swap;          /* true if contents live in swap slot (and frame is NULL) */
    size_t swap_slot;      /* slot in swap space, valid only if in_swap is true */

    /* if file != NULL, page comes from a file (executable or mmaped file) */
    struct file *file;     /* Underlying file */
    off_t file_offset;     /* Offset within file */
    size_t read_bytes;     /* How many bytes to read from file */
    size_t zero_bytes;     /* How many trailing bytes to zero */
    bool file_writable;    /* file writable */
   
    /* For MMAP, optionally remember which mapping this belongs to. */
    int mapid;             /* Or struct mmap_region*; depends on your mmap design. */

    struct hash_elem elem;
};

void spt_init(struct supplemental_page_table*);
void spt_destroy(struct supplemental_page_table*);
struct supp_page_table_entry* spt_lookup(struct supplemental_page_table*,
                                         const void*);
struct supp_page_table_entry* spt_lookup_locked(struct supplemental_page_table*,
                                                const void*);

struct supp_page_table_entry* spt_add_exec(struct supplemental_page_table*,
                                           void*, struct file*, off_t, size_t,
                                           size_t, bool);

struct supp_page_table_entry* spt_add_anon(struct supplemental_page_table*,
                                           void*, bool, bool);

struct supp_page_table_entry* spt_add_mmap(struct supplemental_page_table*,
                                           void*, struct file*, off_t, size_t,
                                           size_t, bool, int);
void spt_remove(struct supplemental_page_table*, struct supp_page_table_entry*);
void spt_set_frame(struct supp_page_table_entry*, struct frame*);
void spt_clear_frame(struct supp_page_table_entry*);
#endif VM_PAGE_H /* vm/page.h */