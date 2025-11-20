#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include "threads/synch.h"
#include "lib/stdbool.h"
#include "userprog/process.h"


struct frame_table {
    struct hash table;
    struct lock frame_table_lock;
};

enum FRAME_KIND {
    FRAME_EXEC,
    FRAME_MMAP,
    FRAME_STACK
};

struct frame {
    void* p_addr;
    bool pinned;
    enum FRAME_KIND f_kind;
    struct list process_frame_mappings;
    struct hash_elem elem;
};

struct process_frame_mapping {
    struct process* process;
    void * user_vaddr;
    struct list_elem elem;
};


#endif /* vm/frame.h */
