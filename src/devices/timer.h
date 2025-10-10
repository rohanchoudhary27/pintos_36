#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <round.h>
#include <stdint.h>
#include "lib/kernel/list.h"
#include "threads/synch.h"

/* Number of timer interrupts per second. */
#define TIMER_FREQ 100

void timer_init (void);
void timer_calibrate (void);

int64_t timer_ticks (void);
int64_t timer_elapsed (int64_t);

/* Sleep and yield the CPU to other threads. */
void timer_sleep (int64_t ticks);
void timer_msleep (int64_t milliseconds);
void timer_usleep (int64_t microseconds);
void timer_nsleep (int64_t nanoseconds);

/* Busy waits. */
void timer_mdelay (int64_t milliseconds);
void timer_udelay (int64_t microseconds);
void timer_ndelay (int64_t nanoseconds);

void timer_print_stats (void);

/*Associates thread (through semaphore) with its wake 
  up time. Compatible with existing linked-list structure.*/
struct sleeping_thread
{
  struct list_elem elem;  /* List element (for linked list) */
  int64_t ticks;          /* System ticks when thread may wake up */
  struct semaphore sema;  /* Semaphore for thread */
};

#endif /* devices/timer.h */
