#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "userprog/process.h"

#include "lib/syscall-nr.h"
#include <inttypes.h>
#include <stdio.h>

typedef void(syscall)(struct intr_frame*);

#define NUMBER_OF_SYSCALLS 20

/* For now, let's store the table as an array with indices representing
    syscall numbers, and the value being the function pointer. Maybe we
    can in the future turn this into hashmap. */
static syscall* syscall_map[NUMBER_OF_SYSCALLS] = {
    [SYS_HALT] = halt,   [SYS_EXIT] = exit,         [SYS_EXEC] = exec,
    [SYS_WAIT] = wait,   [SYS_CREATE] = create,     [SYS_REMOVE] = remove,
    [SYS_OPEN] = open,   [SYS_FILESIZE] = filesize, [SYS_READ] = read,
    [SYS_WRITE] = write, [SYS_SEEK] = seek,         [SYS_TELL] = tell,
    [SYS_CLOSE] = close};

// static void init_syscalls_map() {
//  syscall_map[SYS_WRITE] = ....
//}

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
  if (!is_user_vaddr(f->esp)) {
    exit(PROC_ERR);
  }
  return *(uint32_t*)f->esp;
}

/* --- System call Functions --- */

/* write: size number of bytes from buffer to fd */
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

/* halt: terminates the entire OS by calling shutdown_power_off. */
void halt(void) { shutdown_power_off(); }

/* exit: terminates a user process by calling process_exit. */
void exit(int status) { process_exit(status); }

int wait(pid_t pid) { process_wait(); }
