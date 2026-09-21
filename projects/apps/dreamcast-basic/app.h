/* The editor application: see app.c. */
#ifndef APP_H
#define APP_H

#include <stddef.h>

void app_init(void);
/* Handles one key press; call forever. */
void app_step(void);
/* Test hooks: replace the document, and run it as F5 would. */
void app_load_text(const char *name, const char *text, size_t size);
void app_run_now(void);

#endif
