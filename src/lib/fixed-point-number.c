#include "fixed-point-number.h"
#include <stdint.h>


/* ---------- HELPER FUNCTIONS ---------- */
// Project from int64_t to int32_t
static inline int32_t sat_i64_to_i32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX; // Give INT32_MAX if positive overflow
    if (v < INT32_MIN) return INT32_MIN; // Give INT32_MIN if negative overflow
    return (int32_t)v;
}


/* ---------- REQUIRED FUNCTIONS ---------- */
// Create fixed_point_number set at integer
inline fixed_point_number int_to_fixed(int a) {
    fixed_point_number fixed_num;
    fixed_num.raw = a << FP_FRAC_BITS;
    return fixed_num;
}

// Convert fixed_point_number to integer
inline int32_t fixed_to_int(fixed_point_number a) {
    if (a.raw >= 0) return (a.raw + FP_SCALE / 2) / FP_SCALE;
    return (a.raw - FP_SCALE / 2) / FP_SCALE;
}

inline int32_t fixed_to_int_truncate(fixed_point_number a) {
    return (a.raw) / FP_SCALE;
    //return -((-a.raw + FP_SCALE - 1) / FP_SCALE);
}

// Multiplication with int32_t
inline fixed_point_number multfi(fixed_point_number a, int32_t b) {
    int64_t prod = (int64_t)a.raw * (int64_t)b;
    fixed_point_number r = { .raw = sat_i64_to_i32(prod) };
    return r;
}

// Multiplication with another fixed_point_number
inline fixed_point_number multff(fixed_point_number a, fixed_point_number b) {
    int64_t prod = (int64_t)a.raw * (int64_t)b.raw;
    int64_t scaled = prod / FP_SCALE; 
    fixed_point_number r = { .raw = sat_i64_to_i32(scaled) };
    return r;
}

// Division with int32_t
inline fixed_point_number divfi(fixed_point_number a, int32_t b) {
    // If we are dividing by zero the result is either INT32_MAX or INT32_MIN
    if (b == 0) {
        fixed_point_number inf = { .raw = (a.raw >= 0) ? INT32_MAX : INT32_MIN };
        return inf;
    }

    int64_t q = (int64_t)a.raw / (int64_t)b;
    fixed_point_number r = { .raw = sat_i64_to_i32(q) };
    return r;
}

// Division with another fixed_point_number
inline fixed_point_number divff(fixed_point_number a, fixed_point_number b) {
    // If we are dividing by zero the result is either INT32_MAX or INT32_MIN
    if (b.raw == 0) {
    fixed_point_number inf = { .raw = (a.raw >= 0) ? INT32_MAX : INT32_MIN };
    return inf;
    }

    int64_t num = ((int64_t)a.raw) << FP_FRAC_BITS;
    int64_t q = num / (int64_t)b.raw;
    fixed_point_number r = { .raw = sat_i64_to_i32(q) };
    return r;
}

// Summation with int
inline fixed_point_number sumfi(fixed_point_number a, int32_t b) {
    int64_t addend = ((int64_t)b) << FP_FRAC_BITS;
    int64_t sum = (int64_t)a.raw + addend;
    fixed_point_number r = { .raw = sat_i64_to_i32(sum) };
    return r;
}

// Summation with another fixed_point_number
inline fixed_point_number sumff(fixed_point_number a, fixed_point_number b) {
    int64_t sum = (int64_t)a.raw + (int64_t)b.raw;
    fixed_point_number r = { .raw = sat_i64_to_i32(sum) };
    return r;
}

// Subtraction with int
inline fixed_point_number subfi(fixed_point_number a, int32_t b) {
    int64_t subtractend = ((int64_t)b) << FP_FRAC_BITS;
    int64_t diff = (int64_t)a.raw - subtractend;
    fixed_point_number r = { .raw = sat_i64_to_i32(diff) };
    return r;
}

// Subtraction with another fixed_point_number
inline fixed_point_number subff(fixed_point_number a, fixed_point_number b) {
    int64_t diff = (int64_t)a.raw - (int64_t)b.raw;
    fixed_point_number r = { .raw = sat_i64_to_i32(diff) };
    return r;
}