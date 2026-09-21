/*
 * A Microsoft-style BASIC interpreter: line numbers or labels, the GW-BASIC
 * statement set, and QBasic's structured loops, graphics and sound.
 */
#ifndef BASIC_H
#define BASIC_H

enum { BASIC_OK, BASIC_ERROR, BASIC_BREAK };

typedef struct {
    int status;
    int source_line;        /* one based line of the program text */
    int line_number;        /* BASIC line number, or -1 */
    char message[96];
} basic_result_t;

/* Runs `source` to completion on the shared screen. */
void basic_run(const char *source, basic_result_t *result);

#endif
