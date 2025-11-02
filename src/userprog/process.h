#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

#define PROC_SUCC (0)  /* Exit code for normal process termination. */
#define PROC_ERR (-1)  /* Exit code for erroneous process termination. */

/* A user process, wrapped around a kernel thread. */
struct process
  {
  uint32_t* pagedir;          /* Process page directory. */
  struct thread* thread;      /* Process underlying kernel thread. */
  struct process* parent;     /* Parent of the process. */
  struct list children;       /* List of child_process. */
  int exit_status;            /* Current exit status of the process. */
};

struct child_process {
  tid_t tid;       /*The tid of the child. */
  int exit_status; /* Exit status of child. */
  bool has_exited; /* Flag for whether child process has exited. */
  struct semaphore wait_sema;
  struct list elem;
};

void root_process_init(void);
tid_t process_execute(const char* cmd_line);
int process_wait(tid_t);
void process_exit(int) NO_RETURN;
void process_activate(void);

#endif /* userprog/process.h */
