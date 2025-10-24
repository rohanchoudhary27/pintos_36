#ifndef THREADS_FIXED_POINT_NUMBER_H
#define THREADS_THREAD_H

#include <stdint.h>

// Fixed point number info
#define FP_FRAC_BITS 14
#define FP_SCALE (1 << FP_FRAC_BITS)

// fixed_point_number struct
typedef struct { int32_t raw; } fixed_point_number;

// int to fixed_point_number function
fixed_point_number int_to_fixed (int a);

// fixed_point_number to int function
int32_t fixed_to_int (fixed_point_number a);
int32_t fixed_to_int_truncate (fixed_point_number a);

// Multiplication functions
fixed_point_number multfi (fixed_point_number a, int32_t b);
fixed_point_number multff (fixed_point_number a, fixed_point_number b);

// Division functions
fixed_point_number divfi (fixed_point_number a, int32_t b);
fixed_point_number divff (fixed_point_number a, fixed_point_number b);

// Sum functions
fixed_point_number sumfi (fixed_point_number a, int32_t b);
fixed_point_number sumff (fixed_point_number a, fixed_point_number b);

// Subtraction functions
fixed_point_number subfi (fixed_point_number a, int32_t b);
fixed_point_number subff (fixed_point_number a, fixed_point_number b);

#endif /* threads/fixed-point-number.h */
