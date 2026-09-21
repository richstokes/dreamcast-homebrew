#include "renum.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FIRST_NUMBER 10
#define NUMBER_STEP 10

typedef struct {
    char **lines;
    int count;
    long *old_numbers;      /* -1 where a line has none */
    long *new_numbers;      /* -1 where a line gets none */
    bool numbered;          /* the program had line numbers to begin with */
    int unresolved;
} renum_t;

long renum_line_number(const char *line) {
    long number = 0;

    while(*line == ' ')
        ++line;
    if(!isdigit((unsigned char)*line))
        return -1;
    while(isdigit((unsigned char)*line) && number < 100000000)
        number = number * 10 + (*line++ - '0');
    return number;
}

static bool is_blank(const char *line) {
    while(*line == ' ')
        ++line;
    return *line == '\0';
}

static long map_target(renum_t *r, long target) {
    int i;

    if(r->numbered) {
        for(i = 0; i < r->count; ++i) {
            if(r->old_numbers[i] == target)
                return r->new_numbers[i];
        }
    }
    else if(target >= 1 && target <= r->count) {
        /* An editor line; a blank one falls through to the next statement. */
        for(i = (int)target - 1; i < r->count; ++i) {
            if(r->new_numbers[i] >= 0)
                return r->new_numbers[i];
        }
    }
    ++r->unresolved;
    return target;
}

typedef struct {
    char *text;
    size_t length, capacity;
    bool overflow;
} out_t;

static void put(out_t *out, const char *s, size_t n) {
    if(out->length + n >= out->capacity) {
        out->overflow = true;
        return;
    }
    memcpy(out->text + out->length, s, n);
    out->length += n;
    out->text[out->length] = '\0';
}

/* Copies a run of spaces, then maps a line number if one follows. */
static const char *copy_target(renum_t *r, const char *p, out_t *out) {
    const char *digits;
    char mapped[24];
    long target = 0;

    digits = p;
    while(*digits == ' ')
        ++digits;
    if(!isdigit((unsigned char)*digits))
        return p;
    put(out, p, (size_t)(digits - p));
    while(isdigit((unsigned char)*digits) && target < 100000000)
        target = target * 10 + (*digits++ - '0');
    snprintf(mapped, sizeof(mapped), "%ld", map_target(r, target));
    put(out, mapped, strlen(mapped));
    return digits;
}

static void rewrite_body(renum_t *r, const char *p, out_t *out) {
    static const char *const jumps[] = {
        "GOTO", "GOSUB", "THEN", "ELSE", "RESTORE", NULL
    };

    while(*p) {
        if(*p == '"') {
            const char *end = strchr(p + 1, '"');
            end = end ? end + 1 : p + strlen(p);
            put(out, p, (size_t)(end - p));
            p = end;
        }
        else if(*p == '\'') {
            break;
        }
        else if(isalpha((unsigned char)*p)) {
            char word[12];
            const char *start = p;
            size_t n = 0;
            bool jump = false, list = false;
            int k;

            while(isalpha((unsigned char)*p)) {
                if(n < sizeof(word) - 1)
                    word[n++] = (char)toupper((unsigned char)*p);
                ++p;
            }
            word[n] = '\0';
            if(strncmp(word, "REM", 3) == 0) {
                /* REM, and REMARKABLE... unless it is being assigned to. */
                const char *look = p;
                while(*look == ' ')
                    ++look;
                if(n == 3 || (*look != '=' && *look != '(')) {
                    p = start;
                    break;
                }
            }
            if(strcmp(word, "DATA") == 0) {
                /* DATA runs to the next colon outside quotes. */
                bool quoted = false;
                while(*p && (quoted || *p != ':')) {
                    if(*p == '"')
                        quoted = !quoted;
                    ++p;
                }
                put(out, start, (size_t)(p - start));
                continue;
            }
            if(strcmp(word, "GO") == 0) {
                /* GO TO and GO SUB */
                const char *look = p;
                while(*look == ' ')
                    ++look;
                if(strncasecmp(look, "TO", 2) == 0 &&
                   !isalpha((unsigned char)look[2])) {
                    p = look + 2;
                    jump = list = true;
                }
                else if(strncasecmp(look, "SUB", 3) == 0 &&
                        !isalpha((unsigned char)look[3])) {
                    p = look + 3;
                    jump = list = true;
                }
            }
            for(k = 0; jumps[k] && !jump; ++k) {
                if(strcmp(word, jumps[k]) == 0) {
                    jump = true;
                    list = k < 2;
                }
            }
            put(out, start, (size_t)(p - start));
            if(!jump) {
                /* The rest of an identifier such as A1 is not a target. */
                while(isalnum((unsigned char)*p) || *p == '.' || *p == '_' ||
                      *p == '$' || *p == '%')
                    put(out, p++, 1);
                continue;
            }
            p = copy_target(r, p, out);
            while(list) {
                /* ON x GOTO 100, 200, 300 */
                const char *look = p;
                while(*look == ' ')
                    ++look;
                if(*look != ',')
                    break;
                put(out, p, (size_t)(look + 1 - p));
                p = copy_target(r, look + 1, out);
            }
        }
        else {
            put(out, p++, 1);
        }
    }
    put(out, p, strlen(p));
}

int renum_program(char **lines, int count, int capacity) {
    renum_t r = {lines, count, NULL, NULL, false, 0};
    char **rewritten = calloc((size_t)count, sizeof(*rewritten));
    long next = FIRST_NUMBER;
    int i, result = -1;
    bool failed = false;

    r.old_numbers = malloc(sizeof(long) * (size_t)count);
    r.new_numbers = malloc(sizeof(long) * (size_t)count);
    if(!rewritten || !r.old_numbers || !r.new_numbers)
        goto done;
    for(i = 0; i < count; ++i) {
        r.old_numbers[i] = renum_line_number(lines[i]);
        r.numbered = r.numbered || r.old_numbers[i] >= 0;
    }
    for(i = 0; i < count; ++i) {
        const bool gets_number = r.numbered ? r.old_numbers[i] >= 0 :
                                              !is_blank(lines[i]);
        r.new_numbers[i] = gets_number ? next : -1;
        if(gets_number)
            next += NUMBER_STEP;
    }
    for(i = 0; i < count && !failed; ++i) {
        const char *body = lines[i];
        out_t out = {NULL, 0, (size_t)capacity, false};

        out.text = rewritten[i] = malloc((size_t)capacity);
        if(!out.text) {
            failed = true;
            break;
        }
        out.text[0] = '\0';
        if(r.old_numbers[i] >= 0) {
            while(*body == ' ')
                ++body;
            while(isdigit((unsigned char)*body))
                ++body;
            if(*body == ' ')
                ++body;
        }
        if(r.new_numbers[i] >= 0) {
            char number[24];
            snprintf(number, sizeof(number), "%ld ", r.new_numbers[i]);
            put(&out, number, strlen(number));
        }
        rewrite_body(&r, body, &out);
        failed = out.overflow;
    }
    if(!failed) {
        for(i = 0; i < count; ++i)
            strcpy(lines[i], rewritten[i]);
        result = r.unresolved;
    }
done:
    for(i = 0; rewritten && i < count; ++i)
        free(rewritten[i]);
    free(rewritten);
    free(r.old_numbers);
    free(r.new_numbers);
    return result;
}
