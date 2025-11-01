#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "userprog/process.h"

#include <inttypes.h>
#include <stdio.h>
#include <syscall-nr.h>

static void syscall_handler (struct intr_frame *);

typedef void(syscall)(struct intr_frame*);

#define NUMBER_OF_SYSCALLS 20

/* For now, let's store the table as an array with indices representing
    syscall numbers, and the value being the function pointer. Maybe we
    can in the future turn this into hashmap. */
static syscall* syscall_map[NUMBER_OF_SYSCALLS];

static void init_syscalls_map() {
  // syscall_map[SYS_WRITE] = ....
}

void syscall_init(void) {
  init_syscall_map();
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame* f) {
  int syscall_no = get_syscall_number_from_frame(f);
  syscall_map[syscall_no](f);
  process_exit (PROC_ERR);
}

static uint32_t get_syscall_number_from_frame(struct intr_frame* f) {
  // For now, just return the esp without any additional checks
  // Ideally, we want to have a function that can read from
  // user stack while doing validation, and return 4 byte
  // words from the stack.
  uint32_t* sys_no_addr = (uint32_t*)f->esp;
  return *sys_no_addr;

int write(int fd, const void* buffer, unsigned size) {

  if (buffer == NULL) {
    return 0;
  }
  if (fd == 1) {
    putbuf(buffer, size);
    return size;
  }
  return 0;

}