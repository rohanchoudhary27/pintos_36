#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define BYTE_SIZE 4

static bool process_init (struct thread *t);
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static char** parse_cmd_line(const char*, int*);
static void destroy_fd_table(struct hash_elem*, void*);

/* child proc hash func */
static unsigned hash_child_process(const struct hash_elem* e,
                                   void* aux UNUSED) {
  struct child_process* child = hash_entry(e, struct child_process, elem);
  return hash_int(child->pid);
}

/* child proc hashmap comparision func */
static bool hash_less_child_process(const struct hash_elem* a,
                                    const struct hash_elem* b,
                                    void* aux UNUSED) {
  struct child_process* child_a = hash_entry(a, struct child_process, elem);
  struct child_process* child_b = hash_entry(b, struct child_process, elem);
  return (child_a->pid < child_b->pid);
}

/* Hash calculation function for file descriptions' hashmap. */
static unsigned hash_fd_table(const struct hash_elem* e, void* aux UNUSED) {
  struct file_desc* file_desc = hash_entry(e, struct file_desc, elem);
  return hash_int(file_desc->fd);
}

/* Comparison function for file descriptions' hashmap. */
static bool hash_less_fd_table(const struct hash_elem* a,
                               const struct hash_elem* b, void* aux UNUSED) {
  struct file_desc* file_desc_a = hash_entry(a, struct file_desc, elem);
  struct file_desc* file_desc_b = hash_entry(b, struct file_desc, elem);
  return (file_desc_a->fd < file_desc_b->fd);
}

/* Initialises the main thread as a minimal root OS process.
   Note that when this thread/process terminates, the OS will shutdown. */
void
root_process_init (void)
{
  struct thread *cur = thread_current ();
  bool success = process_init (cur);
  
  if (!success)
    PANIC("Unable to initialise root OS process.");
}

static char** parse_cmd_line(const char* cmd_line, int* argc) {
  char* copy = palloc_get_page(0);
  ASSERT(copy != NULL);

  strlcpy(copy, cmd_line, PGSIZE);

  char* save_ptr;
  char* token;

  int count = 0;

  for (token = strtok_r(copy, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr))
    count++;

  *argc = count;

  char** argv = malloc(sizeof(char*) * *argc);
  char *save_ptr_ii;
  strlcpy(copy, cmd_line, PGSIZE);

  int i = 0;
  for (token = strtok_r(copy, " ", &save_ptr_ii); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr_ii))
    argv[i++] = token;
  return argv;
}

/* Starts a new thread running a user program loaded from CMD_LINE.
   The new thread may be scheduled (and may even exit) before process_execute() returns.
   Returns the new process's thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *cmd_line) 
{
  struct thread* cur = thread_current();
  char* cmd_line_copy;
  tid_t tid;

  /* Make a copy of CMD_LINE.
     Otherwise there's a race between the caller and load(). */
  cmd_line_copy = palloc_get_page (0);
  if (cmd_line_copy == NULL)
    return TID_ERROR;
  strlcpy (cmd_line_copy, cmd_line, PGSIZE);

  /* Create a new thread to execute CMD_LINE. */
  enum intr_level old_level = intr_disable();
  tid = thread_create (cmd_line, PRI_DEFAULT, start_process, cmd_line_copy);
  if (tid == TID_ERROR) {
    palloc_free_page (cmd_line_copy);
    intr_set_level(old_level);
    return TID_ERROR;
  }

  struct child_process* child_p = malloc(sizeof(struct child_process));
  child_p->pid = (pid_t)tid;
  child_p->exit_status = PROC_ERR;
  child_p->has_exited = false;

  child_p->exists = true;     /* Child process exists when created */
  child_p->has_parent = true; /* Parent exists initially */

  lock_init(&child_p->exit_lock);
  cond_init(&child_p->load_condition);
  sema_init(&child_p->wait_sema, 0);
  hash_insert(&cur->process->children, &child_p->elem);

  struct thread* child_thread = thread_get_by_tid(tid);
  if (child_thread != NULL && child_thread->process != NULL) {
    child_thread->process->parent = cur->process;
    child_thread->process->child = child_p;
  }
  intr_set_level(old_level);
  return tid;
}

/* Basic initialisation of a process running on thread T. */
static bool 
process_init (struct thread *t)
{
  /* Create process struct to store process meta-data */
  struct process *process = malloc (sizeof(struct process));
  if (process == NULL)
    return false;

  /* Initialise process attributes. */
  process->pagedir = NULL;
  process->thread = t;
  process->parent = NULL;
  process->child = NULL;
  hash_init(&process->children, hash_child_process, hash_less_child_process,
            NULL);
  process->exit_status = PROC_ERR;
  hash_init(&process->fd_table, hash_fd_table, hash_less_fd_table, NULL);
  process->next_fd = FD_BEGIN;

  /* Initializing lock of process struct*/
  lock_init(&process->process_lock);

  /* Link thread to its process. */
  t->process = process;
  return true;
}

static void push_pointer_data(void** esp, const void* data, size_t bytes) {
  *esp -= bytes;
  memcpy(*esp, &data, bytes);
}

static void push_string_data(void** esp, const char* sdata) {
  size_t bytes = strlen(sdata) + 1;
  *esp -= bytes;
  strlcpy(*esp, sdata, bytes);
}

static void push_null(void** esp) {
  *esp -= sizeof(void*);
  memset(*esp, 0, sizeof(void*));
}

static void align_stack(void** esp, size_t align) {
  uintptr_t misalign = (uintptr_t)(*esp) % align;
  if (misalign != 0) {
    *esp -= misalign;
    memset(*esp, 0, misalign);
  }
}

static bool push_arguments(void** esp, char** argv, int argc) {
  char* arg_addrs[argc];

  for (int i = argc - 1; i >= 0; i--) {
    push_string_data(esp, argv[i]);
    arg_addrs[i] = *esp;
  }

  align_stack(esp, 4);
  // *esp = (void*)((uintptr_t)(*esp) & ~(4 - 1));

  push_null(esp);

  for (int i = argc - 1; i >= 0; i--) {
    push_pointer_data(esp, arg_addrs[i], sizeof(char*));
  }

  char** argv_addr = *esp;
  push_pointer_data(esp, argv_addr, sizeof(char**));

  push_pointer_data(esp, (void*)(uintptr_t)argc, sizeof(int));
  push_null(esp);

  return true;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* args_) {

  // size of stack to check for overflow
  size_t size_stack = BYTE_SIZE * sizeof(void*);

  // size_t max_args = (strlen(args_) + 1) / 2;

  // size_t count_max_args =
  //     (PGSIZE - BYTE_SIZE) / (sizeof(char) + sizeof(char) + sizeof(char*));

  // size_t max_of_args = count_max_args > max_args ? count_max_args : max_args;
  // char** argv = malloc(max_of_args * sizeof(char*));

  bool success = true;

  // if (argv == NULL) {
  //   success = false; // This needs to be some other error
  // }

  int argc = 0;
  char** argv = parse_cmd_line(args_, &argc);
  printf("argc: %d\n", argc);
  for (int i = 0; i < argc; i++) {
    printf("argv[%d]: %s\n", i, argv[i]);
  }
  struct thread *cur = thread_current ();
  char *file_name = args_;
  struct intr_frame if_;

  if (success) {
    success = process_init(cur);
  }

  if (success)
    {
      /* Initialize interrupt frame and load executable. */
      memset (&if_, 0, sizeof if_);
      if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
      if_.cs = SEL_UCSEG;
      if_.eflags = FLAG_IF | FLAG_MBS;
      success = load(argv[0], &if_.eip, &if_.esp);
      if (success) {
        success = push_arguments(&if_.esp, argv, argc);
      }
  }

  size_stack = (size_stack + (BYTE_SIZE - 1)) &
               (~(BYTE_SIZE - 1) + (argc * (int)sizeof(char*)));

  if (size_stack > PGSIZE) {
    success = false;
  }

  /* After loading, we are done with the command-line copy passed to us by process_execute. */
  palloc_free_page (file_name);
  free(argv);
  /* If load failed, terminate the process. */
  if (!success)
    process_exit(PROC_ERR);

  /* Start the user process by simulating a return from an
      interrupt, implemented by intr_exit (in
      threads/intr-stubs.S).  Because intr_exit takes all of its
      arguments on the stack in the form of a `struct intr_frame',
      we just point the stack pointer (%esp) to our stack frame
      and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status. 
 * If it was terminated by the kernel (i.e. killed due to an exception), 
 * returns -1.  
 * If TID is invalid or if it was not a child of the calling process, or if 
 * process_wait() has already been successfully called for the given TID, 
 * returns -1 immediately, without waiting.
 * 
 * This function will be implemented in task 2.
 * For now, it does nothing. */
int process_wait(tid_t child_tid UNUSED) {
  // Look up child in current process's children hash
  struct child_process mock_child;
  mock_child.pid = (pid_t)child_tid;
  struct hash_elem* e =
      hash_find(&thread_current()->process->children, &mock_child.elem);
  if (e == NULL)
    return -1; // Invalid or non-child PID

  struct child_process* child_process =
      hash_entry(e, struct child_process, elem);

  // Wait until child finishes execution (set by sema_up in process_exit)
  sema_down(&child_process->wait_sema);

  // Synchronize with child's exit (ensures exit_status is ready)
  lock_acquire(&child_process->exit_lock);
  lock_release(&child_process->exit_lock);

  // Retrieve and clean up child info
  int exit_status = child_process->exit_status;
  hash_delete(&thread_current()->process->children, &child_process->elem);
  free(child_process);

  return exit_status; // Return child’s exit code to parent
}

/* Helper func to destroy children*/
static void destroy_children(struct hash_elem* e, void* aux UNUSED) {
  struct child_process* child = hash_entry(e, struct child_process, elem);

  bool flag_free = false;
  lock_acquire(&child->exit_lock);
  /* If child does not exist, since parent is dying, free child_info. */
  if (child->exists) {
    child->has_parent = false;
  } else {
    flag_free = true;
  }
  lock_release(&child->exit_lock);

  if (flag_free) {
    free(child);
  }
}

/* Helper func to destroy fd_table */
static void destroy_fd_table(struct hash_elem* element, void* aux UNUSED) {
  struct file_desc* fd = hash_entry(element, struct file_desc, elem);
  file_close(fd->file);
  free(fd);
}

/* Free the current process's resources and then exit the underlying thread. */
void
process_exit (int exit_code UNUSED)
{
  struct process *cur = thread_current ()->process;

  bool flag_free = false;

  /* telling parent child has exited and frees child_process if parent also
   dead*/
  if (cur->child != NULL) {
    lock_acquire(&cur->child->exit_lock);
    if (cur->child->has_parent) {
      cur->child->exists = false;
      sema_up(&cur->child->wait_sema);
    } else {
      flag_free = true;
    }
    lock_release(&cur->child->exit_lock);
    if (flag_free) {
      free(cur->child);
    }
  }

  /* kill all children */
  hash_destroy(&cur->children, destroy_children);

  /* destroy fd_table */
  lock_acquire(&cur->process_lock);
  hash_destroy(&cur->fd_table, destroy_fd_table);
  lock_release(&cur->process_lock);

  uint32_t *pd;
  struct file *file;
  
  /* Clean up all process memory footprint, if it exists. */
  if (cur != NULL)
    {  

      /* Destroy the current process's page directory and switch back
         to the kernel-only page directory. */
      pd = cur->pagedir;
      if (pd != NULL) 
        {
          /* Correct ordering here is crucial.  We must set
             cur->pagedir to NULL before switching page directories,
             so that a timer interrupt can't switch back to the
             process page directory.  We must activate the base page
             directory before destroying the process's page
             directory, or our active page directory will be one
             that's been freed (and cleared). */
          cur->pagedir = NULL;
          pagedir_activate (NULL);
          pagedir_destroy (pd);
        }

      /* Allow access to the current process's executable file if
         the process is exit */
        file = cur->executable;
        if (file != NULL) {
          lock_acquire(&global_file_lock);
          /* Don't have to file allow write as file close does that
             but left it to be explicit about intent */
          file_allow_write(file);
          file_close(file);
          lock_release(&global_file_lock);
          cur->executable = NULL;
        }
  
      /* Finally, destroy the current process's struct */
      printf("<%s>: exit(%d)\n", thread_current()->name, exit_code);
      thread_current()->process = NULL;
      free(cur);

      // TODO: do hash_destroy() also implement a way to destroy the
      // child_processes
    }
    
  /* Now terminate the thread underlying this process. */
  thread_exit();
  NOT_REACHED ();
}

/* Sets up the CPU for running user code in the current thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct process *cur = thread_current ()->process;
  
  /* Activate thread's page tables if running an active process. */
  if (cur != NULL)  
    pagedir_activate (cur->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct process *cur = thread_current ()->process;
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  cur->pagedir = pagedir_create ();
  if (cur->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

    cur->executable = file;
    file_deny_write(file);

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 3 || ehdr.e_version != 1 ||
        ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */

  /* If load failed, clean up the executable file if it was opened. */
  if (!success) {
    if (file != NULL) {
      file_close (file);
    }
    cur->executable = NULL;
  }
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
        
      /* Get a new page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL){
        return false;
      }
        
      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
      {
        palloc_free_page (kpage);
        return false; 
      }     

      /* Load data into the page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes){
        return false; 
      }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct process *cur = thread_current ()->process;

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (cur->pagedir, upage) == NULL
          && pagedir_set_page (cur->pagedir, upage, kpage, writable));
}

/* Returns the file that corresponds to the given file descriptor
   from the current process. */
struct file *
process_get_file(int fd)
{
  struct process* cur = thread_current()->process;
  struct file_desc key;
  key.fd = fd;

  struct hash_elem* e = hash_find(&cur->fd_table, &key.elem);
  if (e == NULL)
    return NULL;

  struct file_desc* f = hash_entry(e, struct file_desc, elem);
  return f->file;
}

/* Add a file to the current process and return the file
   descriptor (fd) as a return value. Returns -1 if
   operation fails. */
int
process_add_file(struct file *f)
{
  if (f == NULL)
    return -1;

  struct process* cur = thread_current()->process;
  struct file_desc* fd = malloc(sizeof *fd);

  if (fd == NULL)
    return -1;

  fd->fd = cur->next_fd++;
  fd->file = f;
  hash_insert(&cur->fd_table, &fd->elem);
  return fd->fd;
}

/* Removes a file from the current process. */
void
process_close_file(int fd)
{
  if (fd < FD_BEGIN)
    return;

  struct process *cur = thread_current ()->process;
  struct file_desc key;
  key.fd = fd;

  struct hash_elem* e = hash_find(&cur->fd_table, &key.elem);
  if (e == NULL)
    return;

  struct file_desc* f = hash_entry(e, struct file_desc, elem);
  hash_delete(&cur->fd_table, &f->elem);
  lock_acquire(&global_file_lock);
  file_close(f->file);
  lock_release(&global_file_lock);
  free(f);
}

/* Close all of the open files the current process has. */
void
process_close_all_files(void)
{
  struct process* cur = thread_current()->process;

  /* Either iterate and delete or just destroy with a destructor. */
  struct hash_iterator it;
  hash_first(&it, &cur->fd_table);
  while (hash_cur(&it) != NULL) {
    struct hash_elem* e = hash_cur(&it);
    struct file_desc* f = hash_entry(e, struct file_desc, elem);

    /* Move iterator before we delete. */
    hash_next(&it);
    hash_delete(&cur->fd_table, &f->elem);

    lock_acquire(&global_file_lock);
    file_close(f->file);
    lock_release(&global_file_lock);

    free(f);
  }
}
