#include "userprog/syscall.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "user/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/frame.h"
#include "vm/page.h"

#include <debug.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>

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

// Define the number of bits used to represent a system call number.
// In Pintos, system call numbers are stored as 32-bit integers.
#define SYSCALL_NUMBER_BITS 32

static void* sys_halt(int argc, void* argv[] UNUSED);
static void* sys_exit(int argc, void* argv[]);
static void* sys_exec(int argc, void* argv[]);
static void* sys_wait(int argc, void* argv[]);
static void* sys_create(int argc, void* argv[]);
static void* sys_remove(int argc, void* argv[]);
static void* sys_open(int argc, void* argv[]);
static void* sys_filesize(int argc, void* argv[]);
static void* sys_read(int argc, void* argv[]);
static void* sys_write(int argc, void* argv[]);
static void* sys_seek(int argc, void* argv[]);
static void* sys_tell(int argc, void* argv[]);
static void* sys_close(int argc, void* argv[]);

static void syscall_handler(struct intr_frame*);
static uint32_t get_syscall_number_from_frame(struct intr_frame*);
static void is_valid_udata(const void*, unsigned);
static void is_valid_uaddr(const void*);
static void exit_and_set_process_status(int);
static void* get_syscall_arg_addr(void*, int);

// Descriptor for a system call in Pintos.
// Each system call is described by the number of arguments it takes,
// whether it returns a value, and the function that implements it.
struct syscall_descriptor {
  int argc;           // Number of arguments the system call expects.
  int is_return_void; // 1 if the syscall does not return a value else 0
  syscall_f
      sys_func; // Pointer to the function that implements the system call.
};

static struct syscall_descriptor syscall_map[] = {
    [SYS_HALT] = {.argc = 0,
                  .is_return_void = 1,
                  .sys_func = (syscall_f)sys_halt},
    [SYS_EXIT] = {.argc = 1,
                  .is_return_void = 1,
                  .sys_func = (syscall_f)sys_exit},
    [SYS_EXEC] = {.argc = 1,
                  .is_return_void = 0,
                  .sys_func = (syscall_f)sys_exec},
    [SYS_WAIT] = {.argc = 1,
                  .is_return_void = 0,
                  .sys_func = (syscall_f)sys_wait},
    [SYS_CREATE] = {.argc = 2,
                    .is_return_void = 0,
                    .sys_func = (syscall_f)sys_create},
    [SYS_REMOVE] = {.argc = 1,
                    .is_return_void = 0,
                    .sys_func = (syscall_f)sys_remove},
    [SYS_OPEN] = {.argc = 1,
                  .is_return_void = 0,
                  .sys_func = (syscall_f)sys_open},
    [SYS_FILESIZE] = {.argc = 1,
                      .is_return_void = 0,
                      .sys_func = (syscall_f)sys_filesize},
    [SYS_READ] = {.argc = 3,
                  .is_return_void = 0,
                  .sys_func = (syscall_f)sys_read},

    [SYS_WRITE] = {.argc = 3,
                   .is_return_void = 0,
                   .sys_func = (syscall_f)sys_write},

    [SYS_SEEK] = {.argc = 2,
                  .is_return_void = 1,
                  .sys_func = (syscall_f)sys_seek},
    [SYS_TELL] = {.argc = 1,
                  .is_return_void = 0,
                  .sys_func = (syscall_f)sys_tell},
    [SYS_CLOSE] = {
        .argc = 1, .is_return_void = 1, .sys_func = (syscall_f)sys_close}};

// Initializes the system call interface in Pintos.
void syscall_init(void) {
  lock_init(&global_file_lock);
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// Handles a system call invoked by a user program.
// 'f' contains the user program's CPU state at the time of the syscall.
static void syscall_handler(struct intr_frame* f) {
  thread_current()->user_esp = f->esp;
  int syscall_no = get_syscall_number_from_frame(f);
  int num_syscall_funcs = sizeof(syscall_map) / sizeof(syscall_map[0]);
  if (syscall_no < 0 || syscall_no >= num_syscall_funcs) {
    exit_and_set_process_status(PROC_ERR);
  }

  struct syscall_descriptor sys_info = syscall_map[syscall_no];
  void* argv[sys_info.argc];
  for (int i = 0; i < sys_info.argc; i++) {
    argv[i] = get_syscall_arg_addr(f->esp, i);
  }

  if (sys_info.is_return_void)
    sys_info.sys_func(sys_info.argc, argv);
  else {
    uint32_t feax = (uint32_t)sys_info.sys_func(sys_info.argc, argv);
    f->eax = feax;
  }
}

/* Gets syscall number from the interrupt frame, and kills the process if the
   address is invalid, i.e., address is in the user space, not null, and is
   mapped. */
static uint32_t get_syscall_number_from_frame(struct intr_frame* f) {
  is_valid_udata(f->esp, SYSCALL_NUMBER_BITS);
  uint32_t* esp = (uint32_t*)f->esp;
  uint32_t syscall_no = *esp;
  return syscall_no;
}

/*
 * Checks that a user string is valid.
 * Loops through each character until '\0'.
 */
static void validate_string(const char* s) {
  for (;;) {
    is_valid_uaddr(s); // Make sure this *char* is accessible
    if (*s == '\0') {
      break;
    }
    s++;
  }
}

/*
 * Checks if a user-supplied address is valid and mapped.
 * Terminates the process if the address is NULL, not in user space,
 * or not mapped in the page directory.
 */
static void is_valid_uaddr(const void* addr) {

  struct thread* current_thread = thread_current();

  if (addr == NULL || !is_user_vaddr(addr) || current_thread == NULL ||
      current_thread->process == NULL ||
      current_thread->process->pagedir == NULL) {
    exit_and_set_process_status(PROC_ERR);
  }
}

/*
 * Validates that a block of user memory is accessible.
 * Checks each page in the range [data_addr, data_addr + size).
 */
static void is_valid_udata(const void* data_addr, unsigned size) {
  uintptr_t addrptr = (uintptr_t)data_addr;
  unsigned i = 0;
  while (i < size) {
    is_valid_uaddr((void*)addrptr + i);
    unsigned move_ptr_by = PGSIZE - ((uintptr_t)(addrptr + i) % PGSIZE);
    i += move_ptr_by < (size - i) ? move_ptr_by : (size - i);
  }
}

/* Exits the current process with the provided status code. Before exiting, it
   sets the process exit_status to status. */
static void exit_and_set_process_status(int status) {
  struct thread* curr_thread = thread_current();
  ASSERT(curr_thread->process != NULL);

  lock_acquire(&curr_thread->process->process_lock);
  curr_thread->process->exit_status = status;
  lock_release(&curr_thread->process->process_lock);
  process_exit(status);
}

/* Retrieves the address of a system call argument from the interrupt frame
   stack pointer. Calculates the memory address based on the esp and
   argument index. Kills the process if the argument address is invalid. */
static void* get_syscall_arg_addr(void* f_esp, int arg_ind) {
  void* arg_addr = ((int*)f_esp) + 1 + (arg_ind);
  is_valid_uaddr(arg_addr);
  void* arg = (void*)*(((int*)f_esp) + 1 + (arg_ind));
  return arg;
}

/* ------------------------ System call Functions ------------------------ */

/* An internal syscall function implementation will conform to the syscall
   typedef in syscall.h, accepting a struct intr_frame * and not returning
   anything. The return value of the syscall will be pushed into the interrupt
   frame's eax field. Each of the syscalls will get their arguments through the
   macro intr_stack_arg, passing the esp and the argument index. */

/* halt: terminates the entire OS by calling shutdown_power_off. */
static void* sys_halt(int argc UNUSED, void* argv[] UNUSED) {
  if (argc != 0)
    return NULL;
  shutdown_power_off();
  return NULL;
}

/* exit: terminates a user process by calling process_exit. */
static void* sys_exit(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  int status = (int)argv[0];
  // Set child process statxus to be the status as well
  thread_current()->child_info->exit_status = status;
  exit_and_set_process_status(status);
  return NULL;
}

/*
 * Executes a new process specified by a command line.
 * Validates the command string and waits for the child to load.
 * Returns the child process ID on success, or SYS_ERROR on failure.
 */
static void* sys_exec(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  const char* cmd_line = (const char*)argv[0];
  validate_string(cmd_line);
  tid_t proc_res = process_execute(cmd_line);
  // wait for child process
  struct thread* current_thread = thread_current();
  if (current_thread == NULL) {
    return (void*)SYS_ERROR;
  }
  struct child_process cp;
  cp.pid = proc_res;

  struct hash_elem* child_hash =
      hash_find(&current_thread->process->children, &cp.elem);
  if (child_hash == NULL) {
    return (void*)SYS_ERROR;
  }
  struct child_process* child_p =
      hash_entry(child_hash, struct child_process, elem);

  sema_down(&child_p->load_semaphore);

  if (!child_p->load_success) {
    return (void*)SYS_ERROR;
  }
  return (void*)proc_res;
}

/*
 * Waits for a child process to finish.
 * Takes the child process ID as argument and returns its exit status.
 */
static void* sys_wait(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  tid_t pid = (tid_t)argv[0];
  int res = process_wait(pid);
  return (void*)res;
}

/*
 * Creates a new file with the given name and initial size.
 * Returns true on success, false on failure.
 */
static void* sys_create(int argc, void* argv[]) {
  if (argc != 2)
    return NULL;
  const char* file_name = (const char*)argv[0];
  unsigned initial_size = (unsigned)argv[1];
  // Validate that the file name string is safe
  validate_string(file_name);
  // Acquire global file system lock to prevent race conditions
  lock_acquire(&global_file_lock);
  bool succ = filesys_create(file_name, initial_size);
  lock_release(&global_file_lock);
  return (void*)succ;
}

/*
 * Removes a file with the given name from the file system.
 * Returns true on success, false on failure.
 */
static void* sys_remove(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  const char* file_name = (const char*)argv[0];
  // Validate that the file name string is safe
  validate_string(file_name);
  // Acquire global file system lock to prevent race conditions
  lock_acquire(&global_file_lock);
  bool succ = filesys_remove(file_name);
  lock_release(&global_file_lock);

  return (void*)succ;
}

/*
 * Opens a file with the given name.
 * Returns the file descriptor on success, or SYS_ERROR on failure.
 */
static void* sys_open(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  const char* file_name = (const char*)argv[0];
  validate_string(file_name);

  /* Open file in file system. */
  lock_acquire(&global_file_lock);
  struct file* file = filesys_open(file_name);
  lock_release(&global_file_lock);

  /* Check if file could not be opened. */
  if (file == NULL)
    return (void*)SYS_ERROR;

  /* process_add_file returns -1 if operation didn't work and closes file */
  int pd = process_add_file(file);

  if (pd == -1) {
    return (void*)SYS_ERROR;
  }

  return (void*)pd;
}

/*
 * Returns the size of a file given its file descriptor.
 * Returns SYS_ERROR if the descriptor is invalid or not associated with a file.
 */
static void* sys_filesize(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  int fd = (int)argv[0];
  /* If the fd corresponds to input or output files we return 0
    as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return (void*)SYS_ERROR;

  /* For any other case return the size of the file */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return (void*)SYS_ERROR;
  }
  // Acquire global file system lock to prevent race conditions
  lock_acquire(&global_file_lock);
  int length = file_length(file);
  lock_release(&global_file_lock);
  return (void*)length;
}

/*
 * Reads data from a file or standard input into a buffer.
 * Returns the number of bytes read, or SYS_ERROR on failure.
 */
static void* sys_read(int argc, void* argv[]) {
  if (argc != 3)
    return NULL;
  int fd = (int)argv[0];
  void* buffer = argv[1];
  unsigned size = (unsigned)argv[2];

  is_valid_udata(buffer, size);

  if (buffer == NULL) {
    return (void*)SYS_ERROR; // Buffer is NULL so return -1
  }

  if (fd == STDOUT_FILENO) {
    return (void*)SYS_ERROR; // Can't read from output file so return -1
  }

  /* Make sure [buffer, buffer+size) is loaded into frames and pinned.*/
  if (!load_and_pin_user_buffer_pages(buffer, size)) {
    return (void*)SYS_ERROR;
  }

  if (fd == STDIN_FILENO) {
    for (uint32_t i = 0; i < size; i++) {
      ((char*)buffer)[i] = input_getc();
    }
    return (void*)size;
  }

  /* Otherwise write to the file from the position specified
    in it's properties */
  struct file* file = process_get_file(fd);

  if (file == NULL) {
    unpin_user_buffer_pages(buffer, size);
    return (void*)SYS_ERROR; // If no file is found with given fd return -1
  }

  lock_acquire(&global_file_lock);
  int bytes_read = file_read(file, buffer, size);
  lock_release(&global_file_lock);
  unpin_user_buffer_pages(buffer, size);
  return (void*)bytes_read;
}

/* write: size number of bytes from buffer to fd */
static void* sys_write(int argc, void* argv[]) {

  if (argc != 3)
    return NULL;

  int fd = (int)argv[0];
  void* buffer = argv[1];
  unsigned size = (unsigned)argv[2];

  is_valid_udata(buffer, size);

  if (buffer == NULL) {
    return (void*)NO_WRITE;
  }

  if (fd == STDIN_FILENO) {
    return (void*)NO_WRITE; // Can't write to input file so return 0
  }

  if (fd == STDOUT_FILENO) {
    /* Write to console, BUFFER_WRITE_SIZE chars at a time. */
    unsigned i;
    for (i = 0; i + BUFFER_WRITE_SIZE <= size; i += BUFFER_WRITE_SIZE) {
      putbuf((char*)buffer + i, BUFFER_WRITE_SIZE);
    }
    putbuf(buffer + i, size - i);
    return (void*)size;
  }

  /* Otherwise write to the file from the position specified
     in it's properties */
  struct file* file = process_get_file(fd);
  if (file == NULL)
    return (void*)NO_WRITE; // If no file is found with given fd return 0

  lock_acquire(&global_file_lock);
  int bytes_written = file_write(file, buffer, size);
  lock_release(&global_file_lock);

  return (void*)bytes_written;
}

/* seek: */
static void* sys_seek(int argc, void* argv[]) {
  if (argc != 2)
    return NULL;
  /* If the fd corresponds to input or output files we return
    as we don't have file objects for them */

  int fd = (int)argv[0];
  unsigned pos = (unsigned)argv[1];

  if (fd < FD_BEGIN)
    return NULL;

  /* For any other case set the position on files */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return if the file is NULL */
    return NULL;
  }

  lock_acquire(&global_file_lock);
  file_seek(file, pos);
  lock_release(&global_file_lock);
  return NULL;
}

/*
 * Returns the current position (offset) in a file given its descriptor.
 * Returns SYS_TELL_NOT_FOUND if the descriptor is invalid or not associated
 * with a file.
 */
static void* sys_tell(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  /* If the fd corresponds to input or output files we return 0
     as we don't have file objects for them */

  int fd = (int)argv[0];

  if (fd < FD_BEGIN)
    return (void*)(unsigned)SYS_TELL_NOT_FOUND;

  /* For any other case return the position property of the
     file object */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return (void*)(unsigned)SYS_TELL_NOT_FOUND;
  }

  lock_acquire(&global_file_lock);
  unsigned offset = file_tell(file);
  lock_release(&global_file_lock);

  return (void*)(unsigned)offset;
}

/*
 * Closes a file given its file descriptor.
 * Does nothing for standard input/output descriptors.
 */
static void* sys_close(int argc, void* argv[]) {
  if (argc != 1)
    return NULL;
  /* If the fd corresponds to input or output files we return
     as we can't close them */

  int fd = (int)argv[0];
  if (fd < FD_BEGIN)
    return NULL;

  /* For any other case we close the file directly */
  process_close_file(fd);
  return NULL;
}