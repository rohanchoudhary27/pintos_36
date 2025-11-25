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


/*

Functions we need to implement:

void spt_init(struct supplemental_page_table *spt);
void spt_destroy(struct supplemental_page_table *spt);
struct supp_page_table_entry* spt_lookup(struct supplemental_page_table *spt, void *upage); // this one uses locks
struct supp_page_table_entry* spt_lookup_locked(struct supplemental_page_table *spt, void *upage); // this one does not use locks
// ^ for the above 2 functions check out frame.h to see the usage
struct supp_page_table_entry*
spt_install_exec(struct supplemental_page_table *spt,
                 void *upage,
                 struct file *file,
                 off_t offset,
                 size_t read_bytes,
                 size_t zero_bytes,
                 bool writable);

struct supp_page_table_entry *
spt_install_anon(struct supplemental_page_table *spt,
                 void *upage,
                 bool writable,
                 bool is_stack);

struct supp_page_table_entry *
spt_install_mmap(struct supplemental_page_table *spt,
                 void *upage,
                 struct file *file,
                 off_t offset,
                 size_t read_bytes,
                 size_t zero_bytes,
                 bool writable,
                 int mapid);

void spt_set_frame(struct supp_page_table_entry *spte,
                   struct frame *frame);

void spt_clear_frame(struct supp_page_table_entry *spte);


void spt_remove(struct supplemental_page_table *spt,
                struct supp_page_table_entry *spte);


idk there may be other api's we need to implement as well


*/

#endif VM_PAGE_H /* vm/page.h */