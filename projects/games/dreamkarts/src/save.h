/* DreamKarts save file: the online name, kept on a VMU (see save.c). */
#ifndef DK_SAVE_H
#define DK_SAVE_H

#include "dk.h"

/* Starts reading the saved name in the background. */
void save_load_name(void);
/* The last name a read found or a store was given, if any: what to show
   before a fresh read lands. */
bool save_peek_name(char *out, size_t size);
/* True once, after a background read found a valid name; copies it out. */
bool save_take_name(char *out, size_t size);
/* Writes the name in the background, unless the card already holds it. */
void save_store_name(const char *name);

#endif
