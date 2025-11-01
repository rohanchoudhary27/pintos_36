#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
int write(int fd, const void* buffer, unsigned size);
void halt(void);

#endif /* userprog/syscall.h */
