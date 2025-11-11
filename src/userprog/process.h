#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include "threads/thread.h"

#define PROC_SUCC (0)  /* Exit code for normal process termination. */
#define PROC_ERR (-1)  /* Exit code for erroneous process termination. */

/* The value next_fd begins at. Needed to reserve some fd's
   for input and output file */
#define FD_BEGIN (2)

/* file descriptor values that corresponds to input and output files */
#define INPUT_FILE_FD (0)
#define OUTPUT_FILE_FD (1)

/* File descriptor to file pairs struct that can be added to lists */
struct file_desc {
  int fd;
  struct file* file;
  struct list_elem elem;
};

/* A user process, wrapped around a kernel thread. */
struct process {
  uint32_t* pagedir;          /* Process page directory. */
  struct thread* thread;      /* Process underlying kernel thread. */
  struct file* file;          /* Process executable file */
  struct list fd_table;       /* List that contains fd file pairs */
  uint32_t next_fd;           /* The next file descriptor value */
  struct process* parent;     /* Parent of the process. */
  struct list children;       /* List of child_process. */
  int exit_status;            /* Current exit status of the process. */
  struct lock process_lock;   /* Per process lock. */
  struct semaphore wait_sema; /* Semaphore for the children processes the
                                 process is waiting for. */
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

struct file *process_get_file(int fd);
int process_add_file(struct file *f);
void process_close_file(int fd);
void process_close_all_files(void);

#endif /* userprog/process.h */
