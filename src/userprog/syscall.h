#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "filesys/file.h"
#include "lib/stdbool.h"
#include "threads/interrupt.h"
#include "process.h"

typedef void* (*syscall_f)(int argc, void* argv);

struct lock global_file_lock;

void syscall_init(void);

#endif /* userprog/syscall.h */
