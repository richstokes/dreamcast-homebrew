/* Engine: a small deterministic generator for effects and AI variety. A
   game that replays (demos, scripted tests) seeds it and keeps everything
   cosmetic that must not shift the sequence on generators of its own.

   One .c file of the game defines ENG_RAND_IMPL and includes this header
   (again, if it already has) to hold the generator: in the file that calls
   it most, GCC can inline it there. rand.c does just that, for games that
   don't mind where it lives. */
#ifndef ENG_RAND_H
#define ENG_RAND_H

#include <stdint.h>

uint32_t eng_rand(void);
void eng_seed(uint32_t seed);
uint32_t eng_rand_peek(void);   /* the generator's state, for QA fingerprints */
static inline float frand(void) { return (float)(eng_rand() & 0xffff) / 65535.0f; }
static inline float frand_s(void) { return frand() * 2.0f - 1.0f; }

#endif

#if defined(ENG_RAND_IMPL) && !defined(ENG_RAND_IMPL_DONE)
#define ENG_RAND_IMPL_DONE
/* A 32-bit LCG. */
static uint32_t rand_state = 0x2026dcu;

uint32_t eng_rand(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return rand_state >> 8;
}

uint32_t eng_rand_peek(void) {
    return rand_state;
}

void eng_seed(uint32_t seed) {
    rand_state = seed;
}
#endif
