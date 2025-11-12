#include "userprog/syscall.h"
#include "devices/input.h"
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

int filesize(int fd) {
  /* If the fd corresponds to input or output files we return 0
    as we don't have file objects for them */
  if (fd < FD_BEGIN)
    return 0;

  /* For any other case return the size of the file */
  struct file* file = process_get_file(fd);
  if (file == NULL) { /* Return 0 if the file is NULL */
    return 0;
  }
  int length = file_length(file);

  return length;
}

/* read: */
int read(int fd, void* buffer, unsigned size) {
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

  unsigned offset = file_tell(file);
  int bytes_read = file_read_at(file, buffer, size, offset);

  return bytes_read;
}

/* write: size number of bytes from buffer to fd */
int write(int fd, const void* buffer, unsigned size) {
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
void seek(int fd, unsigned position) {
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

unsigned tell(int fd) {
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

void close(int fd) {
  /* If the fd corresponds to input or output files we return
     as we can't close them */
  if (fd < FD_BEGIN)
    return;

  /* For any other case we close the file directly */
  process_close_file(fd);
}

/* halt: terminates the entire OS by calling shutdown_power_off. */
void halt(void) { shutdown_power_off(); }

/* exit: terminates a user process by calling process_exit. */
void exit(int status) { process_exit(status); }

int wait(pid_t pid) { process_wait(pid); }
