#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "filesys/file.h"
#include "lib/stdbool.h"
#include "threads/interrupt.h"
#include "process.h"

#define BUFFER_WRITE_SIZE (150)

#define SYS_ERROR (-1)
#define NO_WRITE (0)
#define SYS_TELL_NOT_FOUND (0)

typedef void(syscall)(struct intr_frame*);

void syscall_init(void);

void sys_halt(struct intr_frame* f);
// pid_t sys_exec(const char* cmd_line);
// int sys_wait(pid_t pid);
// bool sys_create(const char* file, unsigned initial_size);
// bool sys_remove(const char* file);
// int sys_open(const char* file);
// int sys_filesize(int fd);
// int sys_read(int fd, void* buffer, unsigned size);
// int sys_write(int fd, const void* buffer, unsigned size);
// void sys_seek(int fd, unsigned position);
// unsigned sys_tell(int fd);
// void sys_close(int fd);

#endif /* userprog/syscall.h */
