#include "userprog/syscall.h"
#include "devices/shutdown.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
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

// char *addr = *((char *) get_syscall_arg_addr(esp, 1));
#define intr_stack_arg(type, esp, ind) *((type*)get_syscall_arg_addr(esp, ind));

/* For now, let's store the table as an array with indices representing
    syscall numbers, and the value being the function pointer. Maybe we
    can in the future turn this into hashmap. */
static syscall* syscall_map[] = {sys_halt};

void syscall_init(void) {
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

/* write: size number of bytes from buffer to fd */
// void sys_write(struct intr_frame* f) {
//   if (buffer == NULL) {
//     return 0;
//   }
//   if (fd == 1) {
//     putbuf(buffer, size);
//     return size;
//   }
//   return 0;
// }

/* halt: terminates the entire OS by calling shutdown_power_off. */
void sys_halt(struct intr_frame* f UNUSED) { shutdown_power_off(); }

/* exit: terminates a user process by calling process_exit. */
// void sys_exit(struct intr_frame* f) { process_exit(status); }

// void sys_wait(struct intr_frame* f) { process_wait() }
