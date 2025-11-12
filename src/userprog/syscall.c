#include "userprog/syscall.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>

#define SYSCALL_NUMBER_BITS 32
#define NUMBER_OF_SYSCALLS 20

static void syscall_handler(struct intr_frame*);
static uint32_t get_syscall_number_from_frame(struct intr_frame*);
static void kill_process_if_user_addr_invalid(const void*);
static void exit_and_set_process_status(int);
static void* get_syscall_arg_addr(void*, int);

struct lock global_file_lock;

// char *addr = *((char *) get_syscall_arg_addr(esp, 1));
#define intr_stack_arg(type, esp, ind) *((type*)get_syscall_arg_addr(esp, ind));

/* For now, let's store the table as an array with indices representing
    syscall numbers, and the value being the function pointer. Maybe we
    can in the future turn this into hashmap. */
static syscall* syscall_map[] = {sys_halt};

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
  syscall_map[syscall_no](f);
}

/* Gets syscall number from the interrupt frame, and kills the process if the
   address is invalid, i.e., address is in the user space, not null, and is
   mapped. */
static uint32_t get_syscall_number_from_frame(struct intr_frame* f) {
  kill_process_if_user_data_invalid(f->esp, 32);
  uint32_t* esp = (uint32_t*)f->esp;
  uint32_t syscall_no = *esp;
  return syscall_no;
}

// TODO: implement buffer/string check as well. The lecture recording
// suggests that you first check the buffer start address, if it's good,
// check its end address as well.
static void kill_process_if_user_addr_invalid(const void* addr) {
  void* phy_addr = pagedir_get_page(thread_current()->process->pagedir, addr);
  if (addr == NULL || !is_user_vaddr(addr) || phy_addr == NULL) {
    exit_and_set_process_status(PROC_ERR);
  }
}

static void kill_process_if_user_data_invalid(const void* data_addr,
                                              unsigned size) {
  uintptr_t addrptr = (uintptr_t)data_addr;
  uintptr_t endptr = addrptr + size;
  uintptr_t pg_boundry = (uintptr_t)pg_round_down(data_addr);
  while (addrptr < endptr) {
    kill_process_if_user_addr_invalid((void*)addrptr);
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
  // FIXME: check status and check thread current if it is null or if the
  // process is null
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
  void* arg_addr = f_esp + 1 + (arg_ind - 1);
  kill_process_if_user_addr_invalid(arg_addr);
  return arg_addr;
}

/* ------------------------ System call Functions ------------------------ */

/* An internal syscall function implementation will conform to the syscall
   typedef in syscall.h, accepting a struct intr_frame * and not returning
   anything. The return value of the syscall will be pushed into the interrupt
   frame's eax field. Each of the syscalls will get their arguments through the
   macro intr_stack_arg, passing the esp and the argument index. */

/* halt: terminates the entire OS by calling shutdown_power_off. */
static void sys_halt(struct intr_frame* f UNUSED) { shutdown_power_off(); }

/* exit: terminates a user process by calling process_exit. */
static void sys_exit(struct intr_frame* f) { 
  int status = intr_stack_arg(int, f->esp, 1); // TODO: validate this data as well
  // Set child process status to be the status as well
  process_exit(status); 
}

static void sys_exec(struct intr_frame *f) {
  const char *cmd_line = intr_stack_arg(const char *, f->esp, 1);
  tid_t proc_res = process_execute(cmd_line);
  // TODO: set up the f->eax to be pid_t
}

static void sys_wait(struct intr_frame *f) {
  // TODO: validate the thing
  tid_t pid = intr_stack_arg(tid_t, f->esp, 1);
  int res = process_wait(pid);
  // TODO: set up the f->eax to be res
}

static void sys_create(struct intr_frame *f) {
  const char *file = intr_stack_arg(const char *, f->esp, 1);
  unsigned initial_size = intr_stack_arg(unsigned, f->esp, 2);
  lock_acquire(&global_file_lock);
  bool succ = filesys_create (file, initial_size);
  lock_release(&global_file_lock);

  // TODO: set the f->eax to be succ

}

static void sys_remove(struct intr_frame* f) {
  const char* file_n = intr_stack_arg(const char*, f->esp, 1);
  // TODO: validate the data
  lock_acquire(&global_file_lock);
  bool succ = filesys_remove(file_n);
  lock_release(&global_file_lock);
  // TODO: set f->eax to succ
}

static void sys_open(struct intr_frame* f) {}

static void sys_filesize(struct intr_frame* f) {
  int fd = intr_stack_arg(int, f->esp, 1);
  /* If the fd corresponds to input or output files we return 0
    as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return 0;

  /* For any other case return the size of the file */
  struct file* file = sys_process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return 0;
  }
  lock_acquire(&global_file_lock);
  int length = file_length(file);
  lock_release(&global_file_lock);

  // TODO: set the f->eax to lenght
}

/* read: */
static void sys_read(struct intr_frame* f) {
  int fd = intr_stack_arg(int, f->esp, 1);
  void* buffer = intr_stack_arg(void*, f->esp, 2);
  unsigned size = intr_stack_arg(unsigned, f->esp, 3);

  // TODO: validate user data of buffer with size

  if (buffer == NULL) {
    return -1; // Buffer is NULL so return -1
  }

  if (fd == OUTPUT_FILE_FD) {
    return -1; // Can't read from output file so return -1
  }

  if (fd == INPUT_FILE_FD) {
    for (uint32_t i = 0; i < size; i++) {
      ((uint8_t*)buffer)[i] = input_getc();
    }
    return size;
  }

  /* Otherwise write to the file from the position specified
    in it's properties */
  struct file* file = process_get_file(fd);

  if (file == NULL)
    return -1; // If no file is found with given fd return -1

  lock_acquire(&global_file_lock);
  unsigned offset = file_tell(file);
  int bytes_read = file_read_at(file, buffer, size, offset);
  lock_release(&global_file_lock);

  // TODO: set f->eax to be bytes_read
}

/* write: size number of bytes from buffer to fd */
int sys_write(int fd, const void* buffer, unsigned size) {
  if (buffer == NULL) {
    return 0;
  }

  if (fd == INPUT_FILE_FD) {
    return 0; // Can't write to input file so return 0
  }

  if (fd == OUTPUT_FILE_FD) {
    putbuf(buffer, size);
    return size;
  }

  /* Otherwise write to the file from the position specified
     in it's properties */
  struct file* file = process_get_file(fd);

  if (file == NULL)
    return 0; // If no file is found with given fd return -1

  unsigned offset = file_tell(file);
  int bytes_written = file_write_at(file, buffer, size, offset);

  return bytes_written;
}

/* seek: */
void sys_seek(int fd, unsigned position) {
  /* If the fd corresponds to input or output files we return
    as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return;

  /* For any other case set the position on files */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return 0;
  }
  file_seek(file, position);
}

unsigned sys_tell(int fd) {
  /* If the fd corresponds to input or output files we return 0
     as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return 0;

  /* For any other case return the position property of the
     file object */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return 0;
  }
  unsigned offset = file_tell(file);

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
