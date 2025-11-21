#include "vm/frame.h"

/* Hash calculation function for frame in frame_table hashmap. */
static unsigned hash_frame_table(const struct hash_elem* e, void* aux UNUSED) {
  struct frame* frame = hash_entry(e, struct frame, elem);
  return hash_int(frame->p_addr);
}

/* Comparison function for frames in frame_table hashmap. */
static bool hash_less_frame_table(const struct hash_elem* a,
                               const struct hash_elem* b, void* aux UNUSED) {
  struct frame* frame_a = hash_entry(a, struct frame, elem);
  struct frame* frame_b = hash_entry(b, struct frame, elem);
  return (frame_a->p_addr < frame_b->p_addr);
}
