#include "userprog/syscall.h"
#include "userprog/process.h"
#include "threads/interrupt.h"
#include <stdio.h>
#include <syscall-nr.h>

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  process_exit (PROC_ERR);
}

static int write(int fd, const void* buffer, unsigned size) {

  if (buffer == NULL) {
    return 0;
  }
  if (fd == 1) {
    putbuf(buffer, size);
    return size;
  }
  return 0;
}