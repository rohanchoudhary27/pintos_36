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

static void syscall_handler(struct intr_frame* f) {
  hex_dump(0, f->esp, 32, true);
  printf ("system call!\n");
  printf("printing f->esp: %p\n", (void*)f->esp);
  printf("printing *f->esp: %d\n", *((int*)f->esp));
  process_exit (PROC_ERR);
}
