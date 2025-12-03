#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "filesys/file.h"
#include "lib/stdbool.h"
#include "process.h"
#include "threads/interrupt.h"
#include "vm/mmap.h"

#define BUFFER_WRITE_SIZE (150)

#define SYS_ERROR (-1)
#define NO_WRITE (0)
#define SYS_TELL_NOT_FOUND (0)

typedef void* (*syscall_f)(int argc, void* argv);

/* The value next_fd begins at. Needed to reserve some fd's
   for input and output file */
#define FD_BEGIN (2)

/* File descriptor to file pairs struct that can be added to lists */
struct file_desc {
  int fd;
  struct file* file;
  struct hash_elem elem;
};

struct lock global_file_lock;

void syscall_init(void);

#endif /* userprog/syscall.h */
