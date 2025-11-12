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

#include <debug.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>

#define SYSCALL_NUMBER_BITS 32

static void* sys_halt(int argc, void* argv[] UNUSED);
static void* sys_exit(int argc, void* argv[]);
static void* sys_exec(int argc, void* argv[]);
static void* sys_wait(int argc, void* argv[]);
static void* sys_create(int argc, void* argv[]);
static void* sys_remove(int argc, void* argv[]);
static void* sys_open(int argc, void* argv[]);
static void* filesize(int argc, void* argv[]);
static void* sys_read(int argc, void* argv[]);
static void* sys_write(int argc, void* argv[]);
void sys_seek(int fd, unsigned position);
unsigned sys_tell(int fd);
void sys_close(int fd);

static void syscall_handler(struct intr_frame*);
static uint32_t get_syscall_number_from_frame(struct intr_frame*);
static void is_valid_uaddr(const void*);
static void exit_and_set_process_status(int);
static void* get_syscall_arg_addr(void*, int);

struct syscall_descriptor {
  int argc;
  int is_return_void;
  syscall_f sys_func;
};

/* For now, let's store the table as an array with indices representing
    syscall numbers, and the value being the function pointer. Maybe we
    can in the future turn this into hashmap. */
static struct syscall_descriptor syscall_map[] = {
    [SYS_HALT] = {
        .argc = 0, .is_return_void = 1, .sys_func = (syscall_f)sys_halt}};

void syscall_init(void) {
  lock_init(&global_file_lock);
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame* f) {
  int syscall_no = get_syscall_number_from_frame(f);
  int num_syscall_funcs = sizeof(syscall_map) / sizeof(syscall_map[0]);
  if (syscall_no < num_syscall_funcs || syscall_no >= num_syscall_funcs) {
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
  is_valid_udata(f->esp, 32);
  uint32_t* esp = (uint32_t*)f->esp;
  uint32_t syscall_no = *esp;
  return syscall_no;
}

// TODO: implement buffer/string check as well. The lecture recording
// suggests that you first check the buffer start address, if it's good,
// check its end address as well.
static void is_valid_uaddr(const void* addr) {
  void* phy_addr = pagedir_get_page(thread_current()->process->pagedir, addr);
  if (addr == NULL || !is_user_vaddr(addr) || phy_addr == NULL) {
    exit_and_set_process_status(PROC_ERR);
  }
}

static void is_valid_udata(const void* data_addr, unsigned size) {
  uintptr_t addrptr = (uintptr_t)data_addr;
  uintptr_t endptr = addrptr + size;
  uintptr_t pg_boundry = (uintptr_t)pg_round_down(data_addr);
  while (addrptr < endptr) {
    is_valid_uaddr((void*)addrptr);
    pg_boundry += PGSIZE;
    if (pg_boundry < endptr)
      addrptr = pg_boundry;
    else
      addrptr = endptr;
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
   stack pointer. Calculates the memory address based on the esp and argument
   index. Kills the process if the argument address is invalid. */
static void* get_syscall_arg_addr(void* f_esp, int arg_ind) {
  void* arg_addr = ((int*)f_esp) + 1 + (arg_ind);
  is_valid_uaddr(arg_addr);
  return arg_addr;
}

/* ------------------------ System call Functions ------------------------ */

/* An internal syscall function implementation will conform to the syscall
   typedef in syscall.h, accepting a struct intr_frame * and not returning
   anything. The return value of the syscall will be pushed into the interrupt
   frame's eax field. Each of the syscalls will get their arguments through the
   macro intr_stack_arg, passing the esp and the argument index. */

/* halt: terminates the entire OS by calling shutdown_power_off. */
static void* sys_halt(int argc, void* argv[] UNUSED) { shutdown_power_off(); }

/* exit: terminates a user process by calling process_exit. */
static void* sys_exit(int argc, void* argv[]) {
  int status = (int)argv[0]; // TODO: validate this data as well
  // Set child process statxus to be the status as well
  process_exit(status);
}

static void* sys_exec(int argc, void* argv[]) {
  const char* cmd_line = (const char*)argv[0];
  is_valid_udata(cmd_line, PGSIZE);
  tid_t proc_res = process_execute(cmd_line);
  return (void*)proc_res;
}

static void* sys_wait(int argc, void* argv[]) {
  tid_t pid = (tid_t)argv[0];
  int res = process_wait(pid);
  return (void*)res;
}

static void* sys_create(int argc, void* argv[]) {
  const char* file = (const char*)argv[0];
  unsigned initial_size = (unsigned)argv[1];
  is_valid_udata(file, READDIR_MAX_LEN);
  lock_acquire(&global_file_lock);
  bool succ = filesys_create (file, initial_size);
  lock_release(&global_file_lock);
  return (void*)succ;
}

static void* sys_remove(int argc, void* argv[]) {
  const char* file_name = (const char*)argv[0];
  is_valid_udata(file_name, READDIR_MAX_LEN);
  lock_acquire(&global_file_lock);
  bool succ = filesys_remove(file_name);
  lock_release(&global_file_lock);

  return (void*)file_name;
}

static void* sys_open(int argc, void* argv[]) {
  const char* file_name = (const char*)argv[0];
  is_valid_udata(file_name, READDIR_MAX_LEN);

  /* Open file in file system. */
  lock_acquire(&global_file_lock);
  struct file* file = filesys_open(file_name);
  lock_release(&global_file_lock);

  /* Check if file could not be opened. */
  if (file == NULL)
    return SYS_ERROR;

  /* process_add_file returns -1 if operation didn't work and closes file */
  int pd = process_add_file(file);

  if (pd == -1) {
    return SYS_ERROR;
  }

  return pd;
}

static void* filesize(int argc, void* argv[]) {
  int fd = (int)argv[0];
  /* If the fd corresponds to input or output files we return 0
    as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return SYS_ERROR;

  /* For any other case return the size of the file */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return SYS_ERROR;
  }
  lock_acquire(&global_file_lock);
  int length = file_length(file);
  lock_release(&global_file_lock);
  return (void*)length;
}

/* read: */
static void* sys_read(int argc, void* argv[]) {
  int fd = (int)argv[0];
  void* buffer = argv[1];
  unsigned size = (unsigned)argv[2];

  is_valid_udata(buffer, size);

  if (buffer == NULL) {
    return SYS_ERROR; // Buffer is NULL so return -1
  }

  if (fd == STDOUT_FILENO) {
    return SYS_ERROR; // Can't read from output file so return -1
  }

  if (fd == STDIN_FILENO) {
    for (uint32_t i = 0; i < size; i++) {
      ((char*)buffer)[i] = input_getc();
    }
    return size;
  }

  /* Otherwise write to the file from the position specified
    in it's properties */
  struct file* file = process_get_file(fd);

  if (file == NULL)
    return SYS_ERROR; // If no file is found with given fd return -1

  lock_acquire(&global_file_lock);
  unsigned offset = file_tell(file);
  int bytes_read = file_read_at(file, buffer, size, offset);
  lock_release(&global_file_lock);

  return (void*)bytes_read;
}

/* write: size number of bytes from buffer to fd */
static void* sys_write(int argc, void* argv[]) {

  int fd = (int)argv[0];
  void* buffer = argv[1];
  unsigned size = (unsigned)argv[2];

  is_valid_udata(buffer, size);

  if (buffer == NULL) {
    return (void*)(intptr_t)NO_WRITE;
  }

  if (fd == STDIN_FILENO) {
    return (void*)(intptr_t)NO_WRITE; // Can't write to input file so return 0
  }

  if (fd == STDOUT_FILENO) {
    /* Write to console, BUFFER_WRITE_SIZE chars at a time. */
    unsigned i;
    for (i = 0; i + BUFFER_WRITE_SIZE <= size; i += BUFFER_WRITE_SIZE) {
      putbuf((char*)buffer + i, BUFFER_WRITE_SIZE);
    }
    putbuf(buffer + i, size - i);
    return (void*)(intptr_t)size;
  }

  /* Otherwise write to the file from the position specified
     in it's properties */
  struct file* file = process_get_file(fd);
  if (file == NULL)
    return (
        void*)(intptr_t)NO_WRITE; // If no file is found with given fd return 0
  lock_acquire(&global_file_lock);

  unsigned offset = file_tell(file);
  int bytes_written = file_write_at(file, buffer, size, offset);
  lock_release(&global_file_lock);

  return (void*)(intptr_t)bytes_written;
}

/* seek: */
void sys_seek(int fd, unsigned position) {
  /* If the fd corresponds to input or output files we return
    as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return;

  /* For any other case set the position on files */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return if the file is NULL */
    return;
  }

  lock_acquire(&global_file_lock);
  file_seek(file, position);
  lock_release(&global_file_lock);
}

unsigned sys_tell(int fd) {
  /* If the fd corresponds to input or output files we return 0
     as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return SYS_TELL_NOT_FOUND;

  /* For any other case return the position property of the
     file object */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return SYS_TELL_NOT_FOUND;
  }

  lock_acquire(&global_file_lock);
  unsigned offset = file_tell(file);
  lock_release(&global_file_lock);

  return offset;
}

void sys_close(int fd) {
  /* If the fd corresponds to input or output files we return
     as we can't close them */
  if (fd < FD_BEGIN)
    return;

  /* For any other case we close the file directly */
  process_close_file(fd);
}
