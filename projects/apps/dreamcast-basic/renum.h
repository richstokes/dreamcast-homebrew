/* RENUM for the editor: see renum.c. */
#ifndef RENUM_H
#define RENUM_H

/*
 * Renumbers a program 10, 20, 30... in text order and rewrites the line
 * numbers after GOTO, GOSUB, THEN, ELSE and RESTORE. A program without any
 * line numbers gains them, and its targets - which meant editor lines - are
 * converted. `lines` holds `count` NUL-terminated buffers of `capacity`
 * bytes, edited in place.
 *
 * Returns the number of targets that matched no line (left unchanged), or -1
 * with nothing modified if a renumbered line would not fit.
 */
int renum_program(char **lines, int count, int capacity);

/* The BASIC line number that starts `line`, or -1. */
long renum_line_number(const char *line);

#endif
