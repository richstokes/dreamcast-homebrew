#include "basic.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "av.h"
#include "platform.h"
#include "screen.h"

/* ------------------------------------------------------------------------ */
/* Keywords                                                                  */

#define KEYWORDS(X) \
    X(PRINT, "PRINT") X(INPUT, "INPUT") X(LINE, "LINE") X(LET, "LET") \
    X(IF, "IF") X(THEN, "THEN") X(ELSE, "ELSE") X(ELSEIF, "ELSEIF") \
    X(END, "END") X(ENDIF, "ENDIF") X(FOR, "FOR") X(TO, "TO") \
    X(STEP, "STEP") X(NEXT, "NEXT") X(WHILE, "WHILE") X(WEND, "WEND") \
    X(DO, "DO") X(LOOP, "LOOP") X(UNTIL, "UNTIL") X(EXIT, "EXIT") \
    X(GOTO, "GOTO") X(GOSUB, "GOSUB") X(GO, "GO") X(RETURN, "RETURN") \
    X(ON, "ON") X(OFF, "OFF") X(DATA, "DATA") X(READ, "READ") \
    X(RESTORE, "RESTORE") X(DIM, "DIM") X(DEF, "DEF") X(REM, "REM") \
    X(STOP, "STOP") X(SYSTEM, "SYSTEM") X(RANDOMIZE, "RANDOMIZE") \
    X(CLS, "CLS") X(LOCATE, "LOCATE") X(COLOR, "COLOR") \
    X(SCREEN, "SCREEN") X(WIDTH, "WIDTH") X(PSET, "PSET") \
    X(PRESET, "PRESET") X(CIRCLE, "CIRCLE") X(PAINT, "PAINT") \
    X(DRAW, "DRAW") X(PALETTE, "PALETTE") X(WINDOW, "WINDOW") \
    X(VIEW, "VIEW") X(SOUND, "SOUND") X(BEEP, "BEEP") X(PLAY, "PLAY") \
    X(SLEEP, "SLEEP") X(SWAP, "SWAP") X(ERASE, "ERASE") \
    X(CLEAR, "CLEAR") X(POKE, "POKE") X(OUT, "OUT") X(WAIT, "WAIT") \
    X(KEY, "KEY") X(OPTION, "OPTION") X(DECLARE, "DECLARE") \
    X(CONST, "CONST") X(USING, "USING") X(DEFINT, "DEFINT") \
    X(DEFSNG, "DEFSNG") X(DEFDBL, "DEFDBL") X(DEFLNG, "DEFLNG") \
    X(DEFSTR, "DEFSTR") X(SHARED, "SHARED") X(AS, "AS") \
    X(AND, "AND") X(OR, "OR") X(XOR, "XOR") X(NOT, "NOT") X(MOD, "MOD") \
    X(EQV, "EQV") X(IMP, "IMP") \
    X(ABS, "ABS") X(ATN, "ATN") X(COS, "COS") X(EXP, "EXP") X(FIX, "FIX") \
    X(INT, "INT") X(LOG, "LOG") X(RND, "RND") X(SGN, "SGN") X(SIN, "SIN") \
    X(SQR, "SQR") X(TAN, "TAN") X(LEN, "LEN") X(ASC, "ASC") X(VAL, "VAL") \
    X(INSTR, "INSTR") X(CHRS, "CHR$") X(STRS, "STR$") X(LEFTS, "LEFT$") \
    X(RIGHTS, "RIGHT$") X(MIDS, "MID$") X(STRINGS, "STRING$") \
    X(SPACES, "SPACE$") X(UCASES, "UCASE$") X(LCASES, "LCASE$") \
    X(LTRIMS, "LTRIM$") X(RTRIMS, "RTRIM$") X(HEXS, "HEX$") \
    X(OCTS, "OCT$") X(INKEYS, "INKEY$") X(INPUTS, "INPUT$") \
    X(DATES, "DATE$") X(TIMES, "TIME$") X(TIMER, "TIMER") \
    X(POINT, "POINT") X(POS, "POS") X(CSRLIN, "CSRLIN") X(TAB, "TAB") \
    X(SPC, "SPC") X(PEEK, "PEEK") X(FRE, "FRE") X(CINT, "CINT") \
    X(CLNG, "CLNG") X(CSNG, "CSNG") X(CDBL, "CDBL") X(STICK, "STICK") \
    X(STRIG, "STRIG")

#define KW_ENUM(id, text) KW_##id,
enum { KW_NONE, KEYWORDS(KW_ENUM) KW_COUNT };
#define KW_NAME(id, text) text,
static const char *const keyword_names[] = { "", KEYWORDS(KW_NAME) };

/* ------------------------------------------------------------------------ */
/* Program representation                                                    */

enum { T_END, T_EOL, T_NUM, T_STR, T_VAR, T_KW, T_FN, T_PUNCT, T_DATA };
enum { P_LE = 'L', P_GE = 'G', P_NE = 'N' };

typedef struct {
    uint8_t type;
    int16_t id;             /* keyword, or punctuation character */
    int32_t i;              /* string constant or symbol index */
    double n;
} tok_t;

typedef struct {
    int number;             /* -1 when the line has none */
    int start;              /* first token */
    int source_line;
} line_t;

typedef struct {
    char *s;
    int len;
} str_t;

#define MAX_DIMS 8
#define MAX_PARAMS 8
#define NAME_MAX_LEN 40

typedef struct {
    char name[NAME_MAX_LEN + 2];
    bool is_str, is_int;
    double num;
    str_t str;
    int ndims;
    int dims[MAX_DIMS];     /* upper bounds */
    int total;
    double *anum;
    str_t *astr;
    int fn_pc;              /* body of a DEF FN, or -1 */
    int fn_nparams;
    int fn_params[MAX_PARAMS];
    int label_line;         /* line carrying this name as a label, or -1 */
} sym_t;

typedef struct {
    bool is_str;
    double n;
    const char *s;
    int len;
} val_t;

typedef struct {
    sym_t *sym;
    int index;              /* array element, or -1 for the scalar */
} lval_t;

enum { F_FOR, F_GOSUB, F_WHILE, F_DO };

typedef struct {
    uint8_t type;
    sym_t *var;
    double limit, step;
    int pc, line;           /* loop body, loop head or return address */
} frame_t;

typedef struct {
    str_t text;
    bool quoted;
    int line;
} data_item_t;

#define MAX_FRAMES 512
#define MAX_STRING 32767
#define MAX_EXPR_DEPTH 48
#define ARENA_BLOCK 65536

typedef struct arena_block {
    struct arena_block *next;
    size_t size, used;
    char data[];
} arena_block_t;

static tok_t *toks;
static int ntoks, cap_toks;
static line_t *lines;
static int nlines, cap_lines;
static int *sorted_lines;
static str_t *consts;
static int nconsts, cap_consts;
static sym_t *syms;
static int nsyms, cap_syms;
static data_item_t *data_items;
static int ndata, cap_data, data_at;
static frame_t frames[MAX_FRAMES];
static int nframes;
static arena_block_t *arena;
static char deftype[26];

static int pc, cur_line;
static int expr_depth;
static jmp_buf bail;
static basic_result_t *result;
static uint32_t rnd_state;
static double rnd_last;
static unsigned poll_counter;
static double next_status_at;

static struct {
    bool on, flip_y;
    double x1, y1, x2, y2;
} window;

static val_t expr(void);
static val_t expr_prec(int min_prec);

/* ------------------------------------------------------------------------ */
/* Errors and memory                                                         */

static void fail(const char *format, ...) {
    va_list args;

    result->status = BASIC_ERROR;
    va_start(args, format);
    vsnprintf(result->message, sizeof(result->message), format, args);
    va_end(args);
    longjmp(bail, 1);
}

static void syntax_error(void) { fail("Syntax error"); }
static void type_mismatch(void) { fail("Type mismatch"); }
static void illegal_call(void) { fail("Illegal function call"); }
static void out_of_memory(void) { fail("Out of memory"); }

static void *xrealloc(void *old, size_t size) {
    void *grown = realloc(old, size ? size : 1);
    if(!grown)
        out_of_memory();
    return grown;
}

/* Strings built while evaluating a statement live here and are released
   together once the statement finishes. */
static char *tmp_alloc(int size) {
    arena_block_t *block = arena;
    size_t want = (size_t)size + 1;

    if(!block || block->size - block->used < want) {
        size_t block_size = want > ARENA_BLOCK ? want : ARENA_BLOCK;
        block = malloc(sizeof(*block) + block_size);
        if(!block)
            fail("Out of string space");
        block->size = block_size;
        block->used = 0;
        block->next = arena;
        arena = block;
    }
    block->used += want;
    return block->data + block->used - want;
}

static void tmp_release(bool everything) {
    while(arena && (everything || arena->next)) {
        arena_block_t *next = arena->next;
        free(arena);
        arena = next;
    }
    if(arena)
        arena->used = 0;
}

static val_t num_val(double n) {
    val_t v = {false, n, NULL, 0};
    return v;
}

static val_t str_val(const char *s, int len) {
    val_t v = {true, 0, s, len};
    return v;
}

static val_t tmp_str(const char *s, int len) {
    char *copy;
    if(len > MAX_STRING)
        fail("String too long");
    copy = tmp_alloc(len);
    if(len)
        memcpy(copy, s, (size_t)len);
    copy[len] = '\0';
    return str_val(copy, len);
}

static void str_set(str_t *dest, const char *s, int len) {
    char *copy = malloc((size_t)len + 1);
    if(!copy)
        fail("Out of string space");
    if(len)
        memcpy(copy, s, (size_t)len);
    copy[len] = '\0';
    free(dest->s);
    dest->s = copy;
    dest->len = len;
}

/* ------------------------------------------------------------------------ */
/* Tokenizer                                                                 */

static void emit(int type, int id, int i, double n) {
    if(ntoks == cap_toks) {
        cap_toks = cap_toks ? cap_toks * 2 : 4096;
        toks = xrealloc(toks, sizeof(*toks) * (size_t)cap_toks);
    }
    toks[ntoks].type = (uint8_t)type;
    toks[ntoks].id = (int16_t)id;
    toks[ntoks].i = i;
    toks[ntoks].n = n;
    ++ntoks;
}

static int add_const(const char *s, int len) {
    if(nconsts == cap_consts) {
        cap_consts = cap_consts ? cap_consts * 2 : 256;
        consts = xrealloc(consts, sizeof(*consts) * (size_t)cap_consts);
    }
    consts[nconsts].s = NULL;
    consts[nconsts].len = 0;
    str_set(&consts[nconsts], s, len);
    return nconsts++;
}

static int find_keyword(const char *name) {
    int i;
    for(i = 1; i < KW_COUNT; ++i) {
        if(strcmp(keyword_names[i], name) == 0)
            return i;
    }
    return KW_NONE;
}

static int find_sym(const char *name) {
    size_t length = strlen(name);
    sym_t *sym;
    int i;

    for(i = 0; i < nsyms; ++i) {
        if(strcmp(syms[i].name, name) == 0)
            return i;
    }
    if(nsyms == cap_syms) {
        cap_syms = cap_syms ? cap_syms * 2 : 128;
        syms = xrealloc(syms, sizeof(*syms) * (size_t)cap_syms);
    }
    sym = &syms[nsyms];
    memset(sym, 0, sizeof(*sym));
    strcpy(sym->name, name);
    sym->is_str = name[length - 1] == '$';
    sym->is_int = name[length - 1] == '%';
    if(!sym->is_str && !sym->is_int && isalpha((unsigned char)name[0])) {
        char kind = deftype[name[0] - 'A'];
        sym->is_str = kind == '$';
        sym->is_int = kind == '%';
    }
    sym->fn_pc = -1;
    sym->label_line = -1;
    return nsyms++;
}

static bool is_ident_char(int c) {
    return isalnum(c) || c == '.' || c == '_';
}

/* DEFINT A-Z and friends take effect while the program is being read. */
static const char *apply_deftype(const char *p, char kind) {
    for(;;) {
        int from, to;
        while(*p == ' ')
            ++p;
        if(!isalpha((unsigned char)*p))
            break;
        from = to = toupper((unsigned char)*p++);
        while(*p == ' ')
            ++p;
        if(*p == '-') {
            ++p;
            while(*p == ' ')
                ++p;
            if(!isalpha((unsigned char)*p))
                break;
            to = toupper((unsigned char)*p++);
        }
        for(; from <= to; ++from)
            deftype[from - 'A'] = kind;
        while(*p == ' ')
            ++p;
        if(*p != ',')
            break;
        ++p;
    }
    return p;
}

static void tokenize_line(const char *p, int source_line) {
    /* Old listings pack keywords against their operands: 1TO3, X>1ANDY<2. */
    static const char *const infix[] = {
        "THEN", "GOTO", "GOSUB", "ELSE", "STEP", "AND", "XOR", "MOD", "EQV",
        "IMP", "OR", "TO", NULL
    };
    static const char *const jumps[] = {"THEN", "ELSE", "GOTO", "GOSUB", NULL};
    const int first_token = ntoks;
    bool after_operand = false;
    line_t *line;

    if(nlines == cap_lines) {
        cap_lines = cap_lines ? cap_lines * 2 : 512;
        lines = xrealloc(lines, sizeof(*lines) * (size_t)cap_lines);
    }
    line = &lines[nlines];
    line->number = -1;
    line->start = ntoks;
    line->source_line = source_line;

    while(*p == ' ' || *p == '\t')
        ++p;
    if(isdigit((unsigned char)*p)) {
        long number = 0;
        while(isdigit((unsigned char)*p)) {
            if(number < 100000000)
                number = number * 10 + (*p - '0');
            ++p;
        }
        line->number = (int)number;
    }

    for(;;) {
        const int c = (unsigned char)*p;
        bool operand = false;

        if(c == '\0' || c == '\r' || c == '\n' || c == '\'')
            break;
        if(c == ' ' || c == '\t') {
            ++p;
            continue;
        }
        if(isdigit(c) || (c == '.' && isdigit((unsigned char)p[1]))) {
            char buffer[64];
            int length = 0;
            while((isdigit((unsigned char)*p) || *p == '.') && length < 60)
                buffer[length++] = *p++;
            if((*p == 'E' || *p == 'e' || *p == 'D' || *p == 'd') &&
               (isdigit((unsigned char)p[1]) ||
                ((p[1] == '+' || p[1] == '-') &&
                 isdigit((unsigned char)p[2])))) {
                buffer[length++] = 'E';
                ++p;
                if(*p == '+' || *p == '-')
                    buffer[length++] = *p++;
                while(isdigit((unsigned char)*p) && length < 62)
                    buffer[length++] = *p++;
            }
            buffer[length] = '\0';
            if(*p == '#' || *p == '!' || *p == '%')
                ++p;
            emit(T_NUM, 0, 0, strtod(buffer, NULL));
            operand = true;
        }
        else if(c == '&' && (toupper((unsigned char)p[1]) == 'H' ||
                             toupper((unsigned char)p[1]) == 'O')) {
            const int base = toupper((unsigned char)p[1]) == 'H' ? 16 : 8;
            char *end;
            long value = strtol(p + 2, &end, base);
            if(end == p + 2)
                value = 0;
            p = end;
            if(*p == '&' || *p == '%')
                ++p;
            if(value > 32767 && value <= 65535)
                value -= 65536;
            emit(T_NUM, 0, 0, (double)value);
            operand = true;
        }
        else if(c == '"') {
            const char *start = ++p;
            while(*p && *p != '"' && *p != '\r' && *p != '\n')
                ++p;
            emit(T_STR, 0, add_const(start, (int)(p - start)), 0);
            if(*p == '"')
                ++p;
            operand = true;
        }
        else if(isalpha(c)) {
            char name[NAME_MAX_LEN + 2];
            const char *start = p;
            int length = 0, keyword, k;
            bool split = false;

            while(is_ident_char((unsigned char)*p)) {
                if(length < NAME_MAX_LEN)
                    name[length++] = (char)toupper((unsigned char)*p);
                ++p;
            }
            if(*p == '$' || *p == '%')
                name[length++] = *p++;
            else if(*p == '!' || *p == '#')
                ++p;
            name[length] = '\0';
            keyword = find_keyword(name);

            if(!keyword) {
                const char *const *list = after_operand ? infix : jumps;
                for(k = 0; list[k] && !split; ++k) {
                    const size_t n = strlen(list[k]);
                    if(strncmp(name, list[k], n) != 0 || name[n] == '\0')
                        continue;
                    if(!after_operand &&
                       strspn(name + n, "0123456789") != strlen(name + n))
                        continue;
                    keyword = find_keyword(list[k]);
                    p = start + n;
                    split = true;
                }
            }
            if(!keyword && strncmp(name, "REM", 3) == 0) {
                /* REMARKABLE PROGRAM... is a remark in Microsoft BASIC. */
                const char *look = p;
                while(*look == ' ')
                    ++look;
                if(*look != '=' && *look != '(')
                    keyword = KW_REM;
            }
            if(keyword == KW_REM)
                break;
            if(keyword == KW_DATA) {
                const char *from = p;
                bool quoted = false;
                while(*p && *p != '\r' && *p != '\n' &&
                      (quoted || *p != ':')) {
                    if(*p == '"')
                        quoted = !quoted;
                    ++p;
                }
                emit(T_KW, KW_DATA, 0, 0);
                emit(T_DATA, 0, add_const(from, (int)(p - from)), 0);
            }
            else if(keyword == KW_DEFINT || keyword == KW_DEFLNG) {
                p = apply_deftype(p, '%');
            }
            else if(keyword == KW_DEFSNG || keyword == KW_DEFDBL) {
                p = apply_deftype(p, ' ');
            }
            else if(keyword == KW_DEFSTR) {
                p = apply_deftype(p, '$');
            }
            else if(keyword) {
                emit(T_KW, keyword, 0, 0);
            }
            else {
                const char *look = p;
                while(*look == ' ')
                    ++look;
                if(ntoks == first_token && *look == ':') {
                    syms[find_sym(name)].label_line = nlines;
                    p = look + 1;
                }
                else {
                    const bool is_fn = name[0] == 'F' && name[1] == 'N' &&
                                       length > 2;
                    emit(is_fn ? T_FN : T_VAR, 0, find_sym(name), 0);
                    operand = true;
                }
            }
        }
        else {
            int id = c;
            ++p;
            if(c == '?') {
                emit(T_KW, KW_PRINT, 0, 0);
                after_operand = false;
                continue;
            }
            if((c == '<' && *p == '=') || (c == '=' && *p == '<')) {
                id = P_LE;
                ++p;
            }
            else if((c == '>' && *p == '=') || (c == '=' && *p == '>')) {
                id = P_GE;
                ++p;
            }
            else if((c == '<' && *p == '>') || (c == '>' && *p == '<')) {
                id = P_NE;
                ++p;
            }
            emit(T_PUNCT, id, 0, 0);
            operand = c == ')';
        }
        after_operand = operand;
    }
    emit(T_EOL, 0, 0, 0);
    ++nlines;
}

/* ------------------------------------------------------------------------ */
/* Runtime helpers                                                           */

static bool break_requested(void) {
    plat_poll();
    return plat_break_pressed();
}

static void update_status(void) {
    char text[SCR_STATUS_MAX + 1];
    const double now = plat_seconds();

    if(now < next_status_at)
        return;
    next_status_at = now + 0.1;
    if(cur_line < nlines && lines[cur_line].number >= 0) {
        snprintf(text, sizeof(text), " RUN   Line %d (Ln %d)   Esc Break",
                 lines[cur_line].number, lines[cur_line].source_line);
    }
    else {
        snprintf(text, sizeof(text), " RUN   Ln %d   Esc Break",
                 cur_line < nlines ? lines[cur_line].source_line : 0);
    }
    scr_set_status(text);
}

static void poll(void) {
    update_status();
    if(break_requested()) {
        result->status = BASIC_BREAK;
        snprintf(result->message, sizeof(result->message), "Break");
        longjmp(bail, 1);
    }
}

static void check_av(int status) {
    if(status == AV_ILLEGAL)
        illegal_call();
    if(status == AV_BREAK) {
        result->status = BASIC_BREAK;
        snprintf(result->message, sizeof(result->message), "Break");
        longjmp(bail, 1);
    }
}

static void out_chars(const char *s, int len) {
    int i;
    for(i = 0; i < len; ++i)
        scr_putc((unsigned char)s[i]);
    plat_debug_output(s, (size_t)len);
}

static void out_text(const char *s) {
    out_chars(s, (int)strlen(s));
}

static int fmt_number(double v, char *out) {
    char *zero;

    if(v == floor(v) && fabs(v) < 1e15)
        snprintf(out, 40, "%.0f", v);
    else
        snprintf(out, 40, "%.7G", v);
    zero = out[0] == '-' ? out + 1 : out;
    if(zero[0] == '0' && zero[1] == '.')
        memmove(zero, zero + 1, strlen(zero));
    if(strcmp(out, "-0") == 0)
        strcpy(out, "0");
    return (int)strlen(out);
}

static int to_int(double n) {
    if(n != n || n > 2147483647.0 || n < -2147483648.0)
        fail("Overflow");
    /* Halves round up, as in GW-BASIC; listings index arrays with them. */
    return (int)floor(n + 0.5);
}

static double parse_number(const char *s, int len, bool *complete) {
    char buffer[80];
    char *end;
    int i = 0, n = 0;
    double value;

    while(i < len && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    if(i + 1 < len && s[i] == '&') {
        const int base = toupper((unsigned char)s[i + 1]) == 'H' ? 16 :
                         toupper((unsigned char)s[i + 1]) == 'O' ? 8 : 0;
        if(base) {
            for(i += 2; i < len && n < 70 && isxdigit((unsigned char)s[i]);)
                buffer[n++] = s[i++];
            buffer[n] = '\0';
            value = (double)strtol(buffer, &end, base);
            if(complete)
                *complete = *end == '\0' && i == len;
            return value;
        }
    }
    for(; i < len && n < 70; ++i) {
        const int c = toupper((unsigned char)s[i]);
        if(c == ' ')
            continue;
        if(!(isdigit(c) || c == '.' || c == '+' || c == '-' || c == 'E' ||
             c == 'D'))
            break;
        buffer[n++] = c == 'D' ? 'E' : (char)c;
    }
    buffer[n] = '\0';
    value = strtod(buffer, &end);
    if(end == buffer)
        value = 0;
    if(complete) {
        while(i < len && s[i] == ' ')
            ++i;
        *complete = *end == '\0' && i == len;
    }
    return value;
}

static int line_of_pc(int at) {
    int low = 0, high = nlines - 1;
    while(low < high) {
        const int mid = (low + high + 1) / 2;
        if(lines[mid].start <= at)
            low = mid;
        else
            high = mid - 1;
    }
    return low;
}

static int compare_line_numbers(const void *a, const void *b) {
    const line_t *la = &lines[*(const int *)a], *lb = &lines[*(const int *)b];
    if(la->number != lb->number)
        return la->number < lb->number ? -1 : 1;
    return *(const int *)a - *(const int *)b;
}

static int find_line_number(int number) {
    int low = 0, high = nlines - 1;
    while(low <= high) {
        const int mid = (low + high) / 2;
        const int value = lines[sorted_lines[mid]].number;
        if(value == number)
            return sorted_lines[mid];
        if(value < number)
            low = mid + 1;
        else
            high = mid - 1;
    }
    return -1;
}

/* Token inspection. */
static bool at_punct(int id) {
    return toks[pc].type == T_PUNCT && toks[pc].id == id;
}

static bool at_kw(int id) {
    return toks[pc].type == T_KW && toks[pc].id == id;
}

static bool at_name(const char *name) {
    return toks[pc].type == T_VAR && strcmp(syms[toks[pc].i].name, name) == 0;
}

static bool accept_punct(int id) {
    if(!at_punct(id))
        return false;
    ++pc;
    return true;
}

static bool accept_kw(int id) {
    if(!at_kw(id))
        return false;
    ++pc;
    return true;
}

static void expect_punct(int id) {
    if(!accept_punct(id))
        syntax_error();
}

static void expect_kw(int id) {
    if(!accept_kw(id))
        syntax_error();
}

static bool at_statement_end(void) {
    return toks[pc].type == T_EOL || toks[pc].type == T_END ||
           at_punct(':') || at_kw(KW_ELSE);
}

static void skip_statement(void) {
    while(toks[pc].type != T_EOL && toks[pc].type != T_END && !at_punct(':'))
        ++pc;
}

static void skip_line(void) {
    while(toks[pc].type != T_EOL && toks[pc].type != T_END)
        ++pc;
}

static double num_expr(void) {
    val_t v = expr();
    if(v.is_str)
        type_mismatch();
    return v.n;
}

static val_t string_expr(void) {
    val_t v = expr();
    if(!v.is_str)
        type_mismatch();
    return v;
}

static int int_expr(void) {
    return to_int(num_expr());
}

/* ------------------------------------------------------------------------ */
/* Variables                                                                 */

static void dim_array(sym_t *sym, const int *bounds, int count) {
    int i, total = 1;

    for(i = 0; i < count; ++i) {
        if(bounds[i] < 0)
            fail("Subscript out of range");
        if((double)total * (bounds[i] + 1) > 4000000.0)
            out_of_memory();
        total *= bounds[i] + 1;
    }
    if(sym->astr) {
        for(i = 0; i < sym->total; ++i)
            free(sym->astr[i].s);
    }
    free(sym->astr);
    free(sym->anum);
    sym->astr = NULL;
    sym->anum = NULL;
    sym->ndims = 0;
    if(sym->is_str)
        sym->astr = calloc((size_t)total, sizeof(*sym->astr));
    else
        sym->anum = calloc((size_t)total, sizeof(*sym->anum));
    if(!sym->astr && !sym->anum)
        out_of_memory();
    sym->ndims = count;
    sym->total = total;
    memcpy(sym->dims, bounds, sizeof(int) * (size_t)count);
}

static int parse_subscripts(int *subs) {
    int count = 0;
    do {
        if(count == MAX_DIMS)
            fail("Subscript out of range");
        subs[count++] = int_expr();
    } while(accept_punct(','));
    expect_punct(')');
    return count;
}

static int array_index(sym_t *sym, const int *subs, int count) {
    int i, index = 0;

    if(!sym->ndims) {
        int bounds[MAX_DIMS];
        for(i = 0; i < count; ++i)
            bounds[i] = 10;
        dim_array(sym, bounds, count);
    }
    if(count != sym->ndims)
        fail("Subscript out of range");
    for(i = 0; i < count; ++i) {
        if(subs[i] < 0 || subs[i] > sym->dims[i])
            fail("Subscript out of range");
        index = index * (sym->dims[i] + 1) + subs[i];
    }
    return index;
}

static lval_t parse_lvalue(void) {
    lval_t lv;
    int subs[MAX_DIMS];

    if(toks[pc].type != T_VAR)
        syntax_error();
    lv.sym = &syms[toks[pc++].i];
    lv.index = -1;
    if(accept_punct('(') || accept_punct('['))
        lv.index = array_index(lv.sym, subs, parse_subscripts(subs));
    return lv;
}

static void assign(lval_t lv, val_t v) {
    sym_t *sym = lv.sym;

    if(sym->is_str != v.is_str)
        type_mismatch();
    if(sym->is_str) {
        str_set(lv.index < 0 ? &sym->str : &sym->astr[lv.index], v.s, v.len);
        return;
    }
    if(sym->is_int) {
        if(fabs(v.n) > 2147483647.0)
            fail("Overflow");
        v.n = floor(v.n + 0.5);
    }
    if(lv.index < 0)
        sym->num = v.n;
    else
        sym->anum[lv.index] = v.n;
}

static val_t load(lval_t lv) {
    sym_t *sym = lv.sym;
    str_t *s;

    if(!sym->is_str)
        return num_val(lv.index < 0 ? sym->num : sym->anum[lv.index]);
    s = lv.index < 0 ? &sym->str : &sym->astr[lv.index];
    return str_val(s->s ? s->s : "", s->len);
}

static void clear_variables(void) {
    int i, k;
    for(i = 0; i < nsyms; ++i) {
        sym_t *sym = &syms[i];
        free(sym->str.s);
        sym->str.s = NULL;
        sym->str.len = 0;
        sym->num = 0;
        if(sym->astr) {
            for(k = 0; k < sym->total; ++k)
                free(sym->astr[k].s);
        }
        free(sym->astr);
        free(sym->anum);
        sym->astr = NULL;
        sym->anum = NULL;
        sym->ndims = sym->total = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Expressions                                                               */

static double next_random(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    rnd_last = (rnd_state >> 8) / 16777216.0;
    return rnd_last;
}

static void seed_random(double seed) {
    uint64_t bits;
    memcpy(&bits, &seed, sizeof(bits));
    rnd_state = (uint32_t)(bits ^ bits >> 32) * 2654435761u;
    if(!rnd_state)
        rnd_state = 0x2545f491u;
    next_random();
}

static const char *inkey(int *len) {
    static const struct { int key; char code; } extended[] = {
        {K_UP, 72}, {K_DOWN, 80}, {K_LEFT, 75}, {K_RIGHT, 77}, {K_HOME, 71},
        {K_END, 79}, {K_PGUP, 73}, {K_PGDN, 81}, {K_INS, 82}, {K_DEL, 83},
        {K_F1, 59}, {K_F2, 60}, {K_F3, 61}, {K_F4, 62}, {K_F5, 63},
        {K_F6, 64}, {K_F7, 65}, {K_F8, 66}, {K_F9, 67}, {K_F10, 68},
        {K_F11, (char)133}, {K_F12, (char)134}
    };
    static char buffer[3];
    const int key = plat_key();
    size_t i;

    *len = 0;
    buffer[0] = buffer[1] = buffer[2] = '\0';
    if(key > 0 && key < 256) {
        buffer[0] = (char)key;
        *len = 1;
    }
    for(i = 0; i < sizeof(extended) / sizeof(extended[0]); ++i) {
        if(extended[i].key == key) {
            buffer[1] = extended[i].code;
            *len = 2;
        }
    }
    return buffer;
}

static int wait_key(void) {
    int key;
    scr.cursor_on = true;
    while((key = plat_key()) == 0) {
        poll();
        plat_sleep_ms(4);
    }
    scr.cursor_on = false;
    return key;
}

static int logical_x(double x) {
    if(window.on)
        x = (x - window.x1) / (window.x2 - window.x1) * (scr.log_w - 1);
    return to_int(x);
}

static int logical_y(double y) {
    if(window.on) {
        y = (y - window.y1) / (window.y2 - window.y1) * (scr.log_h - 1);
        if(window.flip_y)
            y = scr.log_h - 1 - y;
    }
    return to_int(y);
}

static val_t call_user_function(sym_t *fn) {
    sym_t saved[MAX_PARAMS];
    val_t args[MAX_PARAMS], value;
    int count = 0, i, return_pc;

    if(fn->fn_pc < 0)
        fail("Undefined user function");
    if(accept_punct('(')) {
        do {
            if(count == MAX_PARAMS)
                syntax_error();
            args[count++] = expr();
        } while(accept_punct(','));
        expect_punct(')');
    }
    if(count != fn->fn_nparams)
        syntax_error();
    for(i = 0; i < count; ++i) {
        sym_t *param = &syms[fn->fn_params[i]];
        lval_t lv = {param, -1};
        saved[i] = *param;
        param->str.s = NULL;
        param->str.len = 0;
        assign(lv, args[i]);
    }
    return_pc = pc;
    pc = fn->fn_pc;
    value = expr();
    if(value.is_str != fn->is_str)
        type_mismatch();
    if(value.is_str)
        value = tmp_str(value.s, value.len);
    pc = return_pc;
    for(i = 0; i < count; ++i) {
        sym_t *param = &syms[fn->fn_params[i]];
        free(param->str.s);
        param->str = saved[i].str;
        param->num = saved[i].num;
    }
    return value;
}

static val_t call_function(int id) {
    val_t a, b;
    double x;
    int n, m, i;
    char buffer[48];
    char *out;

    switch(id) {
        case KW_RND:
            x = 1;
            if(accept_punct('(')) {
                x = num_expr();
                expect_punct(')');
            }
            if(x < 0)
                seed_random(x);
            return num_val(x == 0 ? rnd_last : next_random());
        case KW_TIMER:
            return num_val(plat_clock_seconds());
        case KW_CSRLIN:
            return num_val(scr.cur_row + 1);
        case KW_INKEYS: {
            const char *key;
            if(++poll_counter % 4 == 0)
                poll();
            key = inkey(&n);
            return tmp_str(key, n);
        }
        case KW_DATES:
        case KW_TIMES: {
            time_t now = time(NULL);
            struct tm *parts = localtime(&now);
            if(!parts)
                return tmp_str("", 0);
            strftime(buffer, sizeof(buffer),
                     id == KW_DATES ? "%m-%d-%Y" : "%H:%M:%S", parts);
            return tmp_str(buffer, (int)strlen(buffer));
        }
        default:
            break;
    }

    expect_punct('(');
    switch(id) {
        case KW_ABS: a = num_val(fabs(num_expr())); break;
        case KW_ATN: a = num_val(atan(num_expr())); break;
        case KW_COS: a = num_val(cos(num_expr())); break;
        case KW_SIN: a = num_val(sin(num_expr())); break;
        case KW_TAN: a = num_val(tan(num_expr())); break;
        case KW_EXP:
            x = num_expr();
            if(x > 709)
                fail("Overflow");
            a = num_val(exp(x));
            break;
        case KW_LOG:
            x = num_expr();
            if(x <= 0)
                illegal_call();
            a = num_val(log(x));
            break;
        case KW_SQR:
            x = num_expr();
            if(x < 0)
                illegal_call();
            a = num_val(sqrt(x));
            break;
        case KW_INT: a = num_val(floor(num_expr())); break;
        case KW_FIX: a = num_val(trunc(num_expr())); break;
        case KW_CINT:
        case KW_CLNG: a = num_val(to_int(num_expr())); break;
        case KW_CSNG:
        case KW_CDBL: a = num_val(num_expr()); break;
        case KW_SGN:
            x = num_expr();
            a = num_val(x > 0 ? 1 : x < 0 ? -1 : 0);
            break;
        case KW_LEN: a = num_val(string_expr().len); break;
        case KW_ASC:
            a = string_expr();
            if(!a.len)
                illegal_call();
            a = num_val((unsigned char)a.s[0]);
            break;
        case KW_VAL:
            a = string_expr();
            a = num_val(parse_number(a.s, a.len, NULL));
            break;
        case KW_INSTR:
            n = 1;
            a = expr();
            if(!a.is_str) {
                n = to_int(a.n);
                expect_punct(',');
                a = string_expr();
            }
            expect_punct(',');
            b = string_expr();
            if(n < 1)
                illegal_call();
            m = 0;
            if(b.len == 0)
                m = n <= a.len ? n : 0;
            for(i = n - 1; !m && i + b.len <= a.len; ++i) {
                if(memcmp(a.s + i, b.s, (size_t)b.len) == 0)
                    m = i + 1;
            }
            a = num_val(m);
            break;
        case KW_CHRS:
            n = int_expr();
            if(n < 0 || n > 255)
                illegal_call();
            buffer[0] = (char)n;
            a = tmp_str(buffer, 1);
            break;
        case KW_STRS:
            x = num_expr();
            buffer[0] = ' ';
            n = fmt_number(x, buffer + (x >= 0 ? 1 : 0)) + (x >= 0 ? 1 : 0);
            a = tmp_str(buffer, n);
            break;
        case KW_HEXS:
        case KW_OCTS:
            snprintf(buffer, sizeof(buffer), id == KW_HEXS ? "%X" : "%o",
                     (unsigned)to_int(num_expr()));
            a = tmp_str(buffer, (int)strlen(buffer));
            break;
        case KW_LEFTS:
        case KW_RIGHTS:
            a = string_expr();
            expect_punct(',');
            n = int_expr();
            if(n < 0)
                illegal_call();
            if(n > a.len)
                n = a.len;
            a = str_val(id == KW_LEFTS ? a.s : a.s + a.len - n, n);
            break;
        case KW_MIDS:
            a = string_expr();
            expect_punct(',');
            n = int_expr();
            m = MAX_STRING;
            if(accept_punct(','))
                m = int_expr();
            if(n < 1 || m < 0)
                illegal_call();
            if(n > a.len)
                n = a.len + 1;
            if(m > a.len - n + 1)
                m = a.len - n + 1;
            a = str_val(a.s + n - 1, m);
            break;
        case KW_STRINGS:
            n = int_expr();
            expect_punct(',');
            b = expr();
            if(b.is_str && !b.len)
                illegal_call();
            m = b.is_str ? (unsigned char)b.s[0] : to_int(b.n);
            if(n < 0 || n > MAX_STRING || m < 0 || m > 255)
                illegal_call();
            out = tmp_alloc(n);
            memset(out, m, (size_t)n);
            out[n] = '\0';
            a = str_val(out, n);
            break;
        case KW_SPACES:
            n = int_expr();
            if(n < 0 || n > MAX_STRING)
                illegal_call();
            out = tmp_alloc(n);
            memset(out, ' ', (size_t)n);
            out[n] = '\0';
            a = str_val(out, n);
            break;
        case KW_UCASES:
        case KW_LCASES:
            a = string_expr();
            a = tmp_str(a.s, a.len);
            for(i = 0; i < a.len; ++i) {
                ((char *)a.s)[i] = (char)(id == KW_UCASES ?
                    toupper((unsigned char)a.s[i]) :
                    tolower((unsigned char)a.s[i]));
            }
            break;
        case KW_LTRIMS:
            a = string_expr();
            while(a.len && a.s[0] == ' ') {
                ++a.s;
                --a.len;
            }
            break;
        case KW_RTRIMS:
            a = string_expr();
            while(a.len && a.s[a.len - 1] == ' ')
                --a.len;
            break;
        case KW_INPUTS:
            n = int_expr();
            if(n < 1 || n > 255)
                illegal_call();
            out = tmp_alloc(n);
            for(i = 0; i < n;) {
                const int key = wait_key();
                if(key < 256)
                    out[i++] = (char)key;
            }
            out[n] = '\0';
            a = str_val(out, n);
            break;
        case KW_POINT:
            x = num_expr();
            expect_punct(',');
            n = logical_x(x);
            a = num_val(scr_point(n, logical_y(num_expr())));
            break;
        case KW_POS:
            num_expr();
            a = num_val(scr.cur_col + 1);
            break;
        case KW_PEEK:
            num_expr();
            a = num_val(0);
            break;
        case KW_FRE:
            expr();
            a = num_val(8388608);
            break;
        case KW_STICK:
            n = int_expr();
            if(n < 0 || n > 3)
                illegal_call();
            a = num_val(plat_stick(n) + 128);
            break;
        case KW_STRIG:
            n = int_expr();
            if(n < 0 || n > 7)
                illegal_call();
            a = num_val(plat_button(n / 2) ? -1 : 0);
            break;
        default:
            syntax_error();
            a = num_val(0);
    }
    expect_punct(')');
    return a;
}

static val_t primary(void) {
    const tok_t *t = &toks[pc];
    val_t v;

    if(++expr_depth > MAX_EXPR_DEPTH)
        fail("Formula too complex");
    switch(t->type) {
        case T_NUM:
            ++pc;
            v = num_val(t->n);
            break;
        case T_STR:
            ++pc;
            v = str_val(consts[t->i].s, consts[t->i].len);
            break;
        case T_VAR:
            v = load(parse_lvalue());
            break;
        case T_FN:
            ++pc;
            v = call_user_function(&syms[t->i]);
            break;
        case T_KW:
            ++pc;
            if(t->id == KW_NOT) {
                v = expr_prec(7);
                if(v.is_str)
                    type_mismatch();
                v = num_val(~to_int(v.n));
            }
            else {
                v = call_function(t->id);
            }
            break;
        case T_PUNCT:
            ++pc;
            if(t->id == '(') {
                v = expr();
                expect_punct(')');
            }
            else if(t->id == '-' || t->id == '+') {
                v = expr_prec(13);
                if(v.is_str)
                    type_mismatch();
                if(t->id == '-')
                    v.n = -v.n;
            }
            else {
                syntax_error();
                v = num_val(0);
            }
            break;
        default:
            syntax_error();
            v = num_val(0);
    }
    --expr_depth;
    return v;
}

enum { OP_IMP = 1, OP_EQV, OP_XOR, OP_OR, OP_AND, OP_NOT, OP_REL, OP_ADD,
       OP_MOD, OP_IDIV, OP_MUL, OP_NEG, OP_POW };

static int binary_precedence(const tok_t *t) {
    if(t->type == T_KW) {
        switch(t->id) {
            case KW_IMP: return OP_IMP;
            case KW_EQV: return OP_EQV;
            case KW_XOR: return OP_XOR;
            case KW_OR: return OP_OR;
            case KW_AND: return OP_AND;
            case KW_MOD: return OP_MOD;
            default: return 0;
        }
    }
    if(t->type != T_PUNCT)
        return 0;
    switch(t->id) {
        case '=': case '<': case '>': case P_LE: case P_GE: case P_NE:
            return OP_REL;
        case '+': case '-': return OP_ADD;
        case '\\': return OP_IDIV;
        case '*': case '/': return OP_MUL;
        case '^': return OP_POW;
        default: return 0;
    }
}

static val_t apply_binary(const tok_t *op, val_t a, val_t b) {
    int ia, ib, order;

    if(a.is_str != b.is_str)
        type_mismatch();
    if(a.is_str) {
        char *joined;
        if(op->type != T_PUNCT)
            type_mismatch();
        if(op->id == '+') {
            if(a.len + b.len > MAX_STRING)
                fail("String too long");
            joined = tmp_alloc(a.len + b.len);
            memcpy(joined, a.s, (size_t)a.len);
            memcpy(joined + a.len, b.s, (size_t)b.len);
            joined[a.len + b.len] = '\0';
            return str_val(joined, a.len + b.len);
        }
        order = memcmp(a.s, b.s, (size_t)(a.len < b.len ? a.len : b.len));
        if(!order)
            order = a.len - b.len;
        switch(op->id) {
            case '=': return num_val(order == 0 ? -1 : 0);
            case '<': return num_val(order < 0 ? -1 : 0);
            case '>': return num_val(order > 0 ? -1 : 0);
            case P_LE: return num_val(order <= 0 ? -1 : 0);
            case P_GE: return num_val(order >= 0 ? -1 : 0);
            case P_NE: return num_val(order != 0 ? -1 : 0);
            default: type_mismatch();
        }
    }
    if(op->type == T_KW) {
        if(op->id == KW_MOD) {
            ia = to_int(a.n);
            ib = to_int(b.n);
            if(!ib)
                fail("Division by zero");
            return num_val(ib == -1 ? 0 : ia % ib);
        }
        ia = to_int(a.n);
        ib = to_int(b.n);
        switch(op->id) {
            case KW_AND: return num_val(ia & ib);
            case KW_OR: return num_val(ia | ib);
            case KW_XOR: return num_val(ia ^ ib);
            case KW_EQV: return num_val(~(ia ^ ib));
            default: return num_val(~ia | ib);
        }
    }
    switch(op->id) {
        case '+': return num_val(a.n + b.n);
        case '-': return num_val(a.n - b.n);
        case '*': return num_val(a.n * b.n);
        case '/':
            if(b.n == 0)
                fail("Division by zero");
            return num_val(a.n / b.n);
        case '\\':
            ia = to_int(a.n);
            ib = to_int(b.n);
            if(!ib)
                fail("Division by zero");
            return num_val(ib == -1 ? -(double)ia : ia / ib);
        case '^':
            if(a.n == 0 && b.n < 0)
                fail("Division by zero");
            if(a.n < 0 && b.n != floor(b.n))
                illegal_call();
            return num_val(pow(a.n, b.n));
        case '=': return num_val(a.n == b.n ? -1 : 0);
        case '<': return num_val(a.n < b.n ? -1 : 0);
        case '>': return num_val(a.n > b.n ? -1 : 0);
        case P_LE: return num_val(a.n <= b.n ? -1 : 0);
        case P_GE: return num_val(a.n >= b.n ? -1 : 0);
        default: return num_val(a.n != b.n ? -1 : 0);
    }
}

static val_t expr_prec(int min_prec) {
    val_t left = primary();

    for(;;) {
        const tok_t *op = &toks[pc];
        const int prec = binary_precedence(op);
        val_t right;

        if(!prec || prec < min_prec)
            return left;
        ++pc;
        right = expr_prec(prec + 1);
        left = apply_binary(op, left, right);
    }
}

static val_t expr(void) {
    return expr_prec(1);
}

/* ------------------------------------------------------------------------ */
/* PRINT and INPUT                                                           */

static void print_value(val_t v) {
    char buffer[48];
    int len;

    if(v.is_str) {
        out_chars(v.s, v.len);
        return;
    }
    buffer[0] = ' ';
    len = fmt_number(v.n, buffer + (v.n >= 0 ? 1 : 0)) + (v.n >= 0 ? 1 : 0);
    buffer[len++] = ' ';
    if(scr.cur_col > 0 && scr.cur_col + len > scr.cols)
        out_text("\n");
    out_chars(buffer, len);
}

static void pad_to_column(int column) {
    while(scr.cur_col < column && scr.cur_col < scr.cols - 1)
        out_text(" ");
}

static bool numeric_field_at(const char *f, int len, int i) {
    if(f[i] == '#')
        return true;
    if(f[i] == '.' && i + 1 < len && f[i + 1] == '#')
        return true;
    if(i + 1 < len && ((f[i] == '*' && f[i + 1] == '*') ||
                       (f[i] == '$' && f[i + 1] == '$')))
        return true;
    if(f[i] == '+' && i + 1 < len)
        return numeric_field_at(f, len, i + 1);
    return false;
}

static int using_number(const char *f, int len, int i, double value) {
    bool plus = false, stars = false, dollar = false, commas = false;
    bool exponent = false, trailing_minus = false, trailing_plus = false;
    int before = 0, after = -1, width, n;
    char digits[640], text[660];
    char *p;
    bool negative = value < 0;

    if(f[i] == '+') {
        plus = true;
        ++i;
    }
    if(i + 1 < len && f[i] == '*' && f[i + 1] == '*') {
        stars = true;
        before += 2;
        i += 2;
    }
    if(i + 1 < len && f[i] == '$' && f[i + 1] == '$') {
        dollar = true;
        before += 2;
        i += 2;
    }
    else if(stars && i < len && f[i] == '$') {
        dollar = true;
        before += 1;
        ++i;
    }
    while(i < len && (f[i] == '#' || f[i] == ',')) {
        if(f[i] == ',')
            commas = true;
        ++before;
        ++i;
    }
    if(i < len && f[i] == '.') {
        after = 0;
        ++i;
        while(i < len && f[i] == '#') {
            ++after;
            ++i;
        }
    }
    if(i + 3 < len && memcmp(f + i, "^^^^", 4) == 0) {
        exponent = true;
        i += 4;
    }
    if(!plus && i < len && f[i] == '+') {
        trailing_plus = true;
        ++i;
    }
    else if(!plus && i < len && f[i] == '-') {
        trailing_minus = true;
        ++i;
    }
    if(after > 60)
        after = 60;

    if(exponent) {
        snprintf(digits, sizeof(digits), "%.*E", after < 0 ? 0 : after,
                 fabs(value));
    }
    else {
        snprintf(digits, sizeof(digits), "%.*f", after < 0 ? 0 : after,
                 fabs(value));
        if(after == 0)
            strcat(digits, ".");
        if(digits[0] == '0' && digits[1] == '.' && before <= 1 &&
           after > 0 && (negative || plus))
            memmove(digits, digits + 1, strlen(digits));
        if(commas) {
            char grouped[640];
            const char *dot = strchr(digits, '.');
            const int whole = dot ? (int)(dot - digits) : (int)strlen(digits);
            int out = 0, k;
            for(k = 0; k < whole; ++k) {
                grouped[out++] = digits[k];
                if((whole - k - 1) % 3 == 0 && k != whole - 1)
                    grouped[out++] = ',';
            }
            strcpy(grouped + out, digits + whole);
            strcpy(digits, grouped);
        }
    }
    p = text;
    if(negative && !trailing_minus && !trailing_plus)
        *p++ = '-';
    else if(plus)
        *p++ = negative ? '-' : '+';
    if(dollar)
        *p++ = '$';
    strcpy(p, digits);
    if(trailing_plus || trailing_minus)
        strcat(p, negative ? "-" : trailing_plus ? "+" : " ");

    width = before + (after >= 0 ? after + 1 : 0) + (exponent ? 4 : 0) +
            (plus || trailing_plus || trailing_minus ? 1 : 0);
    n = (int)strlen(text);
    if(n > width)
        out_text("%");
    for(; n < width; ++n)
        out_text(stars ? "*" : " ");
    out_text(text);
    return i;
}

static void print_using(void) {
    val_t format = string_expr();
    const char *f = format.s;
    const int len = format.len;
    int i = 0;
    bool newline = true, any_field = false;

    expect_punct(';');
    if(!len)
        illegal_call();
    while(!at_statement_end()) {
        val_t v = expr();
        int guard = 0;

        /* Literal text up to the next field, wrapping to reuse the format. */
        for(;;) {
            if(i >= len) {
                if(++guard > 1 && !any_field)
                    illegal_call();
                if(guard > 2)
                    illegal_call();
                i = 0;
            }
            if(f[i] == '_' && i + 1 < len) {
                out_chars(f + i + 1, 1);
                i += 2;
                continue;
            }
            if(f[i] == '!' || f[i] == '&' || f[i] == '\\' ||
               numeric_field_at(f, len, i))
                break;
            out_chars(f + i++, 1);
        }
        any_field = true;
        if(f[i] == '!' || f[i] == '&' || f[i] == '\\') {
            int width = 1, k;
            if(!v.is_str)
                type_mismatch();
            if(f[i] == '&') {
                width = v.len;
                ++i;
            }
            else if(f[i] == '\\') {
                int end = i + 1;
                while(end < len && f[end] == ' ')
                    ++end;
                if(end < len && f[end] == '\\') {
                    width = end - i + 1;
                    i = end + 1;
                }
                else {
                    ++i;
                }
            }
            else {
                ++i;
            }
            for(k = 0; k < width; ++k)
                out_chars(k < v.len ? v.s + k : " ", 1);
        }
        else {
            if(v.is_str)
                type_mismatch();
            i = using_number(f, len, i, v.n);
        }
        newline = true;
        if(accept_punct(';') || accept_punct(','))
            newline = false;
    }
    while(i < len && f[i] != '!' && f[i] != '&' && f[i] != '\\' &&
          !numeric_field_at(f, len, i)) {
        if(f[i] == '_' && i + 1 < len)
            ++i;
        out_chars(f + i++, 1);
    }
    if(newline)
        out_text("\n");
}

static void do_print(void) {
    bool newline = true;

    if(accept_kw(KW_USING)) {
        print_using();
        return;
    }
    while(!at_statement_end()) {
        if(accept_punct(';')) {
            newline = false;
        }
        else if(accept_punct(',')) {
            const int zone = (scr.cur_col / 14 + 1) * 14;
            if(zone + 14 > scr.cols)
                out_text("\n");
            else
                pad_to_column(zone);
            newline = false;
        }
        else if(at_kw(KW_TAB) || at_kw(KW_SPC)) {
            const bool tab = toks[pc++].id == KW_TAB;
            int n;
            expect_punct('(');
            n = int_expr();
            expect_punct(')');
            if(n < 0)
                n = 0;
            if(tab) {
                n = n ? (n - 1) % scr.cols : 0;
                if(scr.cur_col > n)
                    out_text("\n");
                pad_to_column(n);
            }
            else {
                for(n %= scr.cols; n > 0; --n)
                    out_text(" ");
            }
            newline = true;
        }
        else {
            print_value(expr());
            newline = true;
        }
    }
    if(newline)
        out_text("\n");
}

static int read_line(char *buffer, int max) {
    int len = 0;

    for(;;) {
        const int key = wait_key();
        if(key == 13)
            break;
        if(key == 8) {
            if(!len)
                continue;
            --len;
            if(scr.cur_col > 0) {
                --scr.cur_col;
            }
            else if(scr.cur_row > 0) {
                --scr.cur_row;
                scr.cur_col = scr.cols - 1;
            }
            scr_draw_char(scr.cur_col, scr.cur_row, ' ', scr.fg, scr.bg);
        }
        else if(key >= 32 && key < 256 && len < max) {
            buffer[len++] = (char)key;
            scr_putc(key);
        }
    }
    buffer[len] = '\0';
    plat_debug_output(buffer, (size_t)len);
    out_text("\n");
    return len;
}

static void do_input(bool whole_line) {
    static char buffer[256];
    val_t prompt = str_val("", 0);
    bool question = !whole_line;
    int list_pc;

    accept_punct(';');
    if(toks[pc].type == T_STR) {
        prompt = str_val(consts[toks[pc].i].s, consts[toks[pc].i].len);
        ++pc;
        if(accept_punct(','))
            question = false;
        else
            expect_punct(';');
    }
    list_pc = pc;
    for(;;) {
        int len, at = 0;
        bool redo = false;

        pc = list_pc;
        out_chars(prompt.s, prompt.len);
        if(question)
            out_text("? ");
        len = read_line(buffer, sizeof(buffer) - 1);
        if(whole_line) {
            lval_t lv = parse_lvalue();
            if(!lv.sym->is_str)
                type_mismatch();
            assign(lv, str_val(buffer, len));
            return;
        }
        /* Validate the whole reply before assigning any of it. */
        for(;;) {
            lval_t lv = parse_lvalue();
            int start, end;
            bool quoted = false;

            while(at < len && buffer[at] == ' ')
                ++at;
            if(at < len && buffer[at] == '"') {
                quoted = true;
                start = ++at;
                while(at < len && buffer[at] != '"')
                    ++at;
                end = at;
                if(at < len)
                    ++at;
                while(at < len && buffer[at] != ',')
                    ++at;
            }
            else {
                start = at;
                while(at < len && buffer[at] != ',')
                    ++at;
                end = at;
                while(end > start && buffer[end - 1] == ' ')
                    --end;
            }
            if(lv.sym->is_str) {
                assign(lv, str_val(buffer + start, end - start));
            }
            else {
                bool complete = true;
                double value = 0;
                if(end > start)
                    value = parse_number(buffer + start, end - start,
                                         &complete);
                if(quoted || !complete) {
                    redo = true;
                    break;
                }
                assign(lv, num_val(value));
            }
            if(!accept_punct(',')) {
                if(at < len)
                    redo = true;
                break;
            }
            if(at >= len) {
                redo = true;
                break;
            }
            ++at;
        }
        if(!redo)
            return;
        out_text("?Redo from start\n");
    }
}

/* ------------------------------------------------------------------------ */
/* Control flow                                                              */

static void jump_to_line(int line) {
    cur_line = line;
    pc = lines[line].start;
}

static int parse_target(void) {
    int line = -1;

    if(toks[pc].type == T_NUM) {
        line = find_line_number((int)toks[pc].n);
        if(line < 0)
            fail("Undefined line number %d", (int)toks[pc].n);
    }
    else if(toks[pc].type == T_VAR) {
        line = syms[toks[pc].i].label_line;
        if(line < 0)
            fail("Label not defined: %s", syms[toks[pc].i].name);
    }
    else {
        syntax_error();
    }
    ++pc;
    return line;
}

static void push_frame(frame_t frame) {
    if(nframes == MAX_FRAMES)
        out_of_memory();
    frames[nframes++] = frame;
}

/* Index of a loop frame with this head above the innermost GOSUB, or -1. */
static int find_loop_frame(int type, int head) {
    int i;
    for(i = nframes - 1; i >= 0 && frames[i].type != F_GOSUB; --i) {
        if(frames[i].type == type && frames[i].pc == head)
            return i;
    }
    return -1;
}

static void enter_loop(int type, int head, int line) {
    const int existing = find_loop_frame(type, head);
    frame_t frame = {0};

    if(existing >= 0) {
        nframes = existing + 1;
        return;
    }
    frame.type = (uint8_t)type;
    frame.pc = head;
    frame.line = line;
    push_frame(frame);
}

static void leave_loop(int type, int head) {
    const int existing = find_loop_frame(type, head);
    if(existing >= 0)
        nframes = existing;
}

static int top_frame_of(int type, const char *error) {
    int i;
    for(i = nframes - 1; i >= 0 && frames[i].type != F_GOSUB; --i) {
        if(frames[i].type == type)
            return i;
    }
    fail("%s", error);
    return -1;
}

static void do_next(void);

/* Moves past the NEXT, WEND or LOOP closing the construct around `pc`. */
static void skip_past(int open_kw, int close_kw, const char *error) {
    int depth = 0;

    for(;; ++pc) {
        const tok_t *t = &toks[pc];
        if(t->type == T_END)
            fail("%s", error);
        if(t->type != T_KW)
            continue;
        if(t->id == open_kw) {
            /* EXIT FOR, DO WHILE and LOOP WHILE do not open anything. */
            const tok_t *before = &toks[pc > 0 ? pc - 1 : 0];
            if(pc == 0 || before->type != T_KW ||
               (before->id != KW_EXIT && before->id != KW_DO &&
                before->id != KW_LOOP))
                ++depth;
        }
        else if(t->id == close_kw) {
            const int next_at = pc;
            int closes = 1;
            if(close_kw == KW_NEXT) {
                closes = 0;
                while(toks[pc + 1].type == T_VAR) {
                    ++closes;
                    ++pc;
                    if(toks[pc + 1].type == T_PUNCT &&
                       toks[pc + 1].id == ',' && toks[pc + 2].type == T_VAR)
                        ++pc;
                    else
                        break;
                }
                if(!closes)
                    closes = 1;
            }
            if(depth < closes) {
                cur_line = line_of_pc(next_at);
                if(depth + 1 < closes) {
                    /* NEXT J, I: leaving J still steps the loops after it. */
                    pc = next_at + 1 + 2 * (depth + 1);
                    do_next();
                }
                else {
                    ++pc;
                    skip_statement();
                }
                return;
            }
            depth -= closes;
        }
    }
}

static bool line_is_block_if(int at) {
    while(toks[at].type != T_EOL && toks[at].type != T_END)
        ++at;
    return toks[at - 1].type == T_KW && toks[at - 1].id == KW_THEN;
}

/* From a false block IF (or a finished branch), finds where to resume. */
static void skip_block(bool stop_at_else) {
    int depth = 0, line;

    for(line = cur_line + 1; line < nlines; ++line) {
        const int start = lines[line].start;
        const tok_t *t = &toks[start];

        if(t->type != T_KW)
            continue;
        if(t->id == KW_IF) {
            if(line_is_block_if(start))
                ++depth;
        }
        else if(t->id == KW_ENDIF ||
                (t->id == KW_END && toks[start + 1].type == T_KW &&
                 toks[start + 1].id == KW_IF)) {
            if(depth-- == 0) {
                cur_line = line;
                pc = start;
                skip_line();
                return;
            }
        }
        else if(depth == 0 && stop_at_else && t->id == KW_ELSE) {
            cur_line = line;
            pc = start + 1;
            return;
        }
        else if(depth == 0 && stop_at_else && t->id == KW_ELSEIF) {
            cur_line = line;
            pc = start + 1;
            if(num_expr() != 0) {
                expect_kw(KW_THEN);
                return;
            }
        }
    }
    fail("IF without END IF");
}

static void do_if(void) {
    const double condition = num_expr();
    int depth = 0;

    if(!at_kw(KW_GOTO))
        expect_kw(KW_THEN);
    if(toks[pc].type == T_EOL) {
        if(condition == 0)
            skip_block(true);
        return;
    }
    if(condition != 0) {
        if(accept_kw(KW_GOTO) || toks[pc].type == T_NUM)
            jump_to_line(parse_target());
        return;
    }
    for(; toks[pc].type != T_EOL && toks[pc].type != T_END; ++pc) {
        if(toks[pc].type != T_KW)
            continue;
        if(toks[pc].id == KW_IF)
            ++depth;
        else if(toks[pc].id == KW_ELSE && depth-- == 0) {
            ++pc;
            if(toks[pc].type == T_NUM)
                jump_to_line(parse_target());
            return;
        }
    }
}

static void do_for(void) {
    frame_t frame = {0};
    lval_t lv = parse_lvalue();
    double start;
    int i;

    if(lv.index >= 0 || lv.sym->is_str)
        syntax_error();
    expect_punct('=');
    start = num_expr();
    expect_kw(KW_TO);
    frame.limit = num_expr();
    frame.step = accept_kw(KW_STEP) ? num_expr() : 1;
    assign(lv, num_val(start));

    for(i = nframes - 1; i >= 0 && frames[i].type != F_GOSUB; --i) {
        if(frames[i].type == F_FOR && frames[i].var == lv.sym) {
            nframes = i;
            break;
        }
    }
    if(frame.step >= 0 ? start > frame.limit : start < frame.limit) {
        skip_past(KW_FOR, KW_NEXT, "FOR without NEXT");
        return;
    }
    frame.type = F_FOR;
    frame.var = lv.sym;
    frame.pc = pc;
    frame.line = cur_line;
    push_frame(frame);
}

static void do_next(void) {
    do {
        sym_t *var = NULL;
        frame_t *frame;
        lval_t lv;

        if(toks[pc].type == T_VAR)
            var = &syms[toks[pc++].i];
        while(nframes > 0 && frames[nframes - 1].type != F_GOSUB &&
              (frames[nframes - 1].type != F_FOR ||
               (var && frames[nframes - 1].var != var)))
            --nframes;
        if(!nframes || frames[nframes - 1].type != F_FOR)
            fail("NEXT without FOR");
        frame = &frames[nframes - 1];
        lv.sym = frame->var;
        lv.index = -1;
        assign(lv, num_val(frame->var->num + frame->step));
        if(frame->step >= 0 ? frame->var->num <= frame->limit :
                              frame->var->num >= frame->limit) {
            pc = frame->pc;
            cur_line = frame->line;
            return;
        }
        --nframes;
    } while(accept_punct(','));
}

static void do_gosub(int target) {
    frame_t frame = {0};

    skip_statement();
    frame.type = F_GOSUB;
    frame.pc = pc;
    frame.line = cur_line;
    push_frame(frame);
    jump_to_line(target);
}

static void do_on(void) {
    int choice, target = -1, index = 1;
    bool gosub;

    choice = int_expr();
    if(choice < 0 || choice > 255)
        illegal_call();
    if(accept_kw(KW_GO)) {
        gosub = !accept_kw(KW_TO);
        if(gosub && !at_name("SUB"))
            syntax_error();
        if(gosub)
            ++pc;
    }
    else {
        gosub = at_kw(KW_GOSUB);
        if(!gosub && !at_kw(KW_GOTO))
            fail("Unsupported ON statement");
        ++pc;
    }
    do {
        if(index++ == choice)
            target = parse_target();
        else if(toks[pc].type == T_NUM || toks[pc].type == T_VAR)
            ++pc;
        else
            syntax_error();
    } while(accept_punct(','));
    if(target < 0)
        return;
    if(gosub)
        do_gosub(target);
    else
        jump_to_line(target);
}

static bool loop_condition(bool *present) {
    bool until;
    double value;

    *present = at_kw(KW_WHILE) || at_kw(KW_UNTIL);
    if(!*present)
        return true;
    until = toks[pc++].id == KW_UNTIL;
    value = num_expr();
    return until ? value == 0 : value != 0;
}

/* ------------------------------------------------------------------------ */
/* DATA                                                                      */

static void add_data_item(const char *s, int len, bool quoted, int line) {
    if(ndata == cap_data) {
        cap_data = cap_data ? cap_data * 2 : 256;
        data_items = xrealloc(data_items,
                              sizeof(*data_items) * (size_t)cap_data);
    }
    data_items[ndata].text.s = NULL;
    data_items[ndata].text.len = 0;
    str_set(&data_items[ndata].text, s, len);
    data_items[ndata].quoted = quoted;
    data_items[ndata].line = line;
    ++ndata;
}

static void collect_data(const char *s, int len, int line) {
    int at = 0;

    for(;;) {
        int start, end;
        bool quoted = false;

        while(at < len && (s[at] == ' ' || s[at] == '\t'))
            ++at;
        if(at < len && s[at] == '"') {
            quoted = true;
            start = ++at;
            while(at < len && s[at] != '"')
                ++at;
            end = at;
            while(at < len && s[at] != ',')
                ++at;
        }
        else {
            start = at;
            while(at < len && s[at] != ',')
                ++at;
            end = at;
            while(end > start && (s[end - 1] == ' ' || s[end - 1] == '\t'))
                --end;
        }
        add_data_item(s + start, end - start, quoted, line);
        if(at >= len)
            break;
        ++at;
    }
}

static void do_read(void) {
    do {
        lval_t lv = parse_lvalue();
        data_item_t *item;

        if(data_at >= ndata)
            fail("Out of DATA");
        item = &data_items[data_at++];
        if(lv.sym->is_str) {
            assign(lv, str_val(item->text.s, item->text.len));
        }
        else {
            bool complete = true;
            double value = parse_number(item->text.s, item->text.len,
                                        &complete);
            if(item->quoted || !complete || !item->text.len)
                fail("Syntax error in DATA");
            assign(lv, num_val(value));
        }
    } while(accept_punct(','));
}

/* Registers every DEF FN and DATA item before the program starts. */
static void scan_definitions(void) {
    int at, line = 0;

    for(at = 0; at < ntoks; ++at) {
        const tok_t *t = &toks[at];
        while(line + 1 < nlines && lines[line + 1].start <= at)
            ++line;
        if(t->type == T_DATA) {
            collect_data(consts[t->i].s, consts[t->i].len, line);
        }
        else if(t->type == T_KW && t->id == KW_DEF &&
                toks[at + 1].type == T_FN) {
            sym_t *fn = &syms[toks[at + 1].i];
            int look = at + 2;

            fn->fn_nparams = 0;
            if(toks[look].type == T_PUNCT && toks[look].id == '(') {
                ++look;
                while(toks[look].type == T_VAR &&
                      fn->fn_nparams < MAX_PARAMS) {
                    fn->fn_params[fn->fn_nparams++] = toks[look++].i;
                    if(toks[look].type == T_PUNCT && toks[look].id == ',')
                        ++look;
                }
                if(toks[look].type != T_PUNCT || toks[look].id != ')')
                    continue;
                ++look;
            }
            if(toks[look].type == T_PUNCT && toks[look].id == '=')
                fn->fn_pc = look + 1;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Graphics and sound statements                                             */

static void parse_point(int *x, int *y) {
    const bool relative = accept_kw(KW_STEP);
    double px, py;

    expect_punct('(');
    px = num_expr();
    expect_punct(',');
    py = num_expr();
    expect_punct(')');
    if(relative) {
        av_get_position(x, y);
        *x += to_int(px);
        *y += to_int(py);
    }
    else {
        *x = logical_x(px);
        *y = logical_y(py);
    }
}

static int color_arg(int fallback) {
    int color;
    if(at_statement_end() || at_punct(','))
        return fallback;
    color = int_expr();
    if(color < 0 || color > 255)
        illegal_call();
    return color;
}

static void do_pset(bool preset) {
    int x, y, color = preset ? scr.bg : scr.fg;

    parse_point(&x, &y);
    if(accept_punct(','))
        color = color_arg(color);
    scr_pset(x, y, color);
    av_set_position(x, y);
}

static void do_line(void) {
    int x1, y1, x2, y2, color = scr.fg;
    bool box = false, fill = false;

    if(at_punct('-'))
        av_get_position(&x1, &y1);
    else
        parse_point(&x1, &y1);
    expect_punct('-');
    av_set_position(x1, y1);
    parse_point(&x2, &y2);
    if(accept_punct(',')) {
        color = color_arg(color);
        if(accept_punct(',')) {
            if(at_name("B"))
                box = true;
            else if(at_name("BF"))
                box = fill = true;
            else if(!at_statement_end() && !at_punct(','))
                syntax_error();
            if(box)
                ++pc;
            if(accept_punct(','))
                num_expr();     /* line style is not drawn */
        }
    }
    if(box)
        scr_box(x1, y1, x2, y2, color, fill);
    else
        scr_line(x1, y1, x2, y2, color);
    av_set_position(x2, y2);
}

static void do_circle(void) {
    int x, y, color = scr.fg;
    double radius, start = 0, end = 2 * 3.14159265358979323846;
    double aspect = 4.0 * scr.log_h / (3.0 * scr.log_w);
    bool arc = false;

    parse_point(&x, &y);
    expect_punct(',');
    radius = num_expr();
    if(window.on)
        radius = fabs(radius / (window.x2 - window.x1) * (scr.log_w - 1));
    if(accept_punct(',')) {
        color = color_arg(color);
        if(accept_punct(',')) {
            if(!at_punct(',') && !at_statement_end()) {
                start = num_expr();
                arc = true;
            }
            if(accept_punct(',')) {
                if(!at_punct(',') && !at_statement_end()) {
                    end = num_expr();
                    arc = true;
                }
                if(accept_punct(','))
                    aspect = num_expr();
            }
        }
    }
    if(fabs(start) > 6.2832 || fabs(end) > 6.2832)
        illegal_call();
    if(!arc)
        end = 2 * 3.14159265358979323846;
    scr_circle(x, y, radius, color, start, end, aspect);
    av_set_position(x, y);
}

static void do_paint(void) {
    int x, y, color = scr.fg, border;

    parse_point(&x, &y);
    if(accept_punct(',') && !at_punct(',') && !at_statement_end()) {
        val_t v = expr();
        if(!v.is_str) {
            color = to_int(v.n);
            if(color < 0 || color > 255)
                illegal_call();
        }
    }
    border = color;
    if(accept_punct(','))
        border = color_arg(border);
    scr_paint(x, y, color, border);
}

static void do_palette(void) {
    static const int level[4] = {0, 85, 170, 255};
    int attribute, color;

    if(at_statement_end()) {
        scr_reset_palette();
        return;
    }
    if(at_kw(KW_USING))
        fail("PALETTE USING is not supported");
    attribute = int_expr();
    expect_punct(',');
    color = to_int(num_expr());
    if(attribute < 0 || attribute > 255 || color < -1)
        illegal_call();
    if(color == -1)
        return;
    if(scr.mode == 12 || scr.mode == 13) {
        scr_set_palette(attribute, (color & 63) * 255 / 63,
                        (color >> 8 & 63) * 255 / 63,
                        (color >> 16 & 63) * 255 / 63);
    }
    else {
        color &= 63;
        scr_set_palette(attribute,
                        level[(color >> 2 & 1) * 2 + (color >> 5 & 1)],
                        level[(color >> 1 & 1) * 2 + (color >> 4 & 1)],
                        level[(color & 1) * 2 + (color >> 3 & 1)]);
    }
}

static void do_color(void) {
    int first = -1, second = -1;

    if(!at_punct(',') && !at_statement_end())
        first = int_expr();
    if(accept_punct(',')) {
        if(!at_punct(',') && !at_statement_end())
            second = int_expr();
        if(accept_punct(',') && !at_statement_end())
            int_expr();
    }
    if(first > 255 || second > 255)
        illegal_call();
    if(scr.mode == 1) {
        if(first >= 0)
            scr.bg = first & 15;
        return;
    }
    if(first >= 0)
        scr.fg = scr.mode == 13 ? first : first & 15;
    if(second >= 0)
        scr.bg = scr.mode == 13 ? second : second & 15;
}

static void do_screen(void) {
    const int mode = int_expr();

    while(accept_punct(',')) {
        if(!at_punct(',') && !at_statement_end())
            num_expr();
    }
    if(!scr_mode_supported(mode))
        illegal_call();
    scr_set_mode(mode);
    window.on = false;
    av_set_position(scr.log_w / 2, scr.log_h / 2);
}

static void do_window(void) {
    bool screen_order = accept_kw(KW_SCREEN);
    double x1, y1, x2, y2;

    if(at_statement_end()) {
        window.on = false;
        return;
    }
    expect_punct('(');
    x1 = num_expr();
    expect_punct(',');
    y1 = num_expr();
    expect_punct(')');
    expect_punct('-');
    expect_punct('(');
    x2 = num_expr();
    expect_punct(',');
    y2 = num_expr();
    expect_punct(')');
    if(x1 == x2 || y1 == y2)
        illegal_call();
    window.on = true;
    window.flip_y = !screen_order;
    window.x1 = x1 < x2 ? x1 : x2;
    window.x2 = x1 < x2 ? x2 : x1;
    window.y1 = y1 < y2 ? y1 : y2;
    window.y2 = y1 < y2 ? y2 : y1;
}

static void do_locate(void) {
    int row = scr.cur_row + 1, col = scr.cur_col + 1;

    if(!at_punct(',') && !at_statement_end())
        row = int_expr();
    if(accept_punct(',')) {
        if(!at_punct(',') && !at_statement_end())
            col = int_expr();
        while(accept_punct(',')) {
            if(!at_punct(',') && !at_statement_end())
                num_expr();
        }
    }
    if(row < 1 || col < 1 || row > 255 || col > 255)
        illegal_call();
    scr_locate(row - 1, col - 1);
}

static void do_sleep(void) {
    double until = -1;

    if(!at_statement_end()) {
        const double seconds = num_expr();
        if(seconds > 0)
            until = plat_seconds() + seconds;
    }
    while(until < 0 || plat_seconds() < until) {
        poll();
        if(plat_key())
            break;
        plat_sleep_ms(5);
    }
}

static void do_swap(void) {
    lval_t a = parse_lvalue(), b;
    val_t va, vb;

    expect_punct(',');
    b = parse_lvalue();
    if(a.sym->is_str != b.sym->is_str)
        type_mismatch();
    va = load(a);
    vb = load(b);
    if(va.is_str) {
        va = tmp_str(va.s, va.len);
        vb = tmp_str(vb.s, vb.len);
    }
    assign(a, vb);
    assign(b, va);
}

static void do_dim(void) {
    accept_kw(KW_SHARED);
    do {
        sym_t *sym;
        int bounds[MAX_DIMS], count;

        if(toks[pc].type != T_VAR)
            syntax_error();
        sym = &syms[toks[pc++].i];
        if(accept_punct('(')) {
            count = parse_subscripts(bounds);
            dim_array(sym, bounds, count);
        }
        if(accept_kw(KW_AS)) {
            if(toks[pc].type != T_VAR)
                syntax_error();
            ++pc;
        }
    } while(accept_punct(','));
}

static void do_mid_assignment(void) {
    lval_t lv;
    val_t current, replacement;
    int start, count = MAX_STRING;
    char *copy;

    expect_punct('(');
    lv = parse_lvalue();
    if(!lv.sym->is_str)
        type_mismatch();
    expect_punct(',');
    start = int_expr();
    if(accept_punct(','))
        count = int_expr();
    expect_punct(')');
    expect_punct('=');
    replacement = string_expr();
    current = load(lv);
    if(start < 1 || start > current.len || count < 0)
        illegal_call();
    if(count > replacement.len)
        count = replacement.len;
    if(count > current.len - start + 1)
        count = current.len - start + 1;
    copy = tmp_alloc(current.len);
    memcpy(copy, current.s, (size_t)current.len);
    memcpy(copy + start - 1, replacement.s, (size_t)count);
    assign(lv, str_val(copy, current.len));
}

/* ------------------------------------------------------------------------ */
/* Statement dispatch                                                        */

/* Returns false when the program ends. */
static bool statement(void) {
    const tok_t *t = &toks[pc];
    const int head = pc, head_line = cur_line;
    bool present;
    val_t v;
    int i;

    if(t->type == T_VAR) {
        lval_t lv = parse_lvalue();
        expect_punct('=');
        assign(lv, expr());
        return true;
    }
    if(t->type != T_KW)
        syntax_error();
    ++pc;
    switch(t->id) {
        case KW_LET:
        case KW_CONST: {
            lval_t lv = parse_lvalue();
            expect_punct('=');
            assign(lv, expr());
            break;
        }
        case KW_PRINT:
            do_print();
            break;
        case KW_INPUT:
            do_input(false);
            break;
        case KW_LINE:
            if(accept_kw(KW_INPUT))
                do_input(true);
            else
                do_line();
            break;
        case KW_IF:
            do_if();
            return true;
        case KW_ELSE:
        case KW_ELSEIF:
            if(head == lines[cur_line].start)
                skip_block(false);
            else
                skip_line();
            return true;
        case KW_ENDIF:
            break;
        case KW_END:
            if(accept_kw(KW_IF))
                break;
            if(!at_statement_end())
                fail("SUB and FUNCTION are not supported");
            return false;
        case KW_STOP:
        case KW_SYSTEM:
            return false;
        case KW_FOR:
            do_for();
            return true;
        case KW_NEXT:
            do_next();
            return true;
        case KW_WHILE:
            if(num_expr() != 0) {
                enter_loop(F_WHILE, head, head_line);
            }
            else {
                leave_loop(F_WHILE, head);
                skip_past(KW_WHILE, KW_WEND, "WHILE without WEND");
            }
            return true;
        case KW_WEND:
            i = top_frame_of(F_WHILE, "WEND without WHILE");
            nframes = i + 1;
            pc = frames[i].pc;
            cur_line = frames[i].line;
            return true;
        case KW_DO:
            if(loop_condition(&present)) {
                enter_loop(F_DO, head, head_line);
            }
            else {
                leave_loop(F_DO, head);
                skip_past(KW_DO, KW_LOOP, "DO without LOOP");
            }
            return true;
        case KW_LOOP:
            i = top_frame_of(F_DO, "LOOP without DO");
            if(loop_condition(&present)) {
                nframes = i + 1;
                pc = frames[i].pc;
                cur_line = frames[i].line;
            }
            else {
                nframes = i;
            }
            return true;
        case KW_EXIT:
            if(accept_kw(KW_FOR)) {
                nframes = top_frame_of(F_FOR, "EXIT FOR without FOR");
                skip_past(KW_FOR, KW_NEXT, "FOR without NEXT");
            }
            else if(accept_kw(KW_DO)) {
                nframes = top_frame_of(F_DO, "EXIT DO without DO");
                skip_past(KW_DO, KW_LOOP, "DO without LOOP");
            }
            else {
                fail("SUB and FUNCTION are not supported");
            }
            return true;
        case KW_GO:
            if(accept_kw(KW_TO)) {
                jump_to_line(parse_target());
            }
            else {
                if(!at_name("SUB"))
                    syntax_error();
                ++pc;
                do_gosub(parse_target());
            }
            return true;
        case KW_GOTO:
            jump_to_line(parse_target());
            return true;
        case KW_GOSUB:
            do_gosub(parse_target());
            return true;
        case KW_RETURN:
            while(nframes > 0 && frames[nframes - 1].type != F_GOSUB)
                --nframes;
            if(!nframes)
                fail("RETURN without GOSUB");
            --nframes;
            pc = frames[nframes].pc;
            cur_line = frames[nframes].line;
            return true;
        case KW_ON:
            do_on();
            return true;
        case KW_DATA:
            ++pc;
            break;
        case KW_READ:
            do_read();
            break;
        case KW_RESTORE:
            data_at = 0;
            if(!at_statement_end()) {
                const int line = parse_target();
                while(data_at < ndata && data_items[data_at].line < line)
                    ++data_at;
            }
            break;
        case KW_DIM:
            do_dim();
            break;
        case KW_ERASE:
            do {
                int none[1] = {0};
                if(toks[pc].type != T_VAR)
                    syntax_error();
                dim_array(&syms[toks[pc].i], none, 0);
                syms[toks[pc++].i].ndims = 0;
            } while(accept_punct(','));
            break;
        case KW_CLEAR:
            skip_statement();
            clear_variables();
            nframes = 0;
            data_at = 0;
            break;
        case KW_RANDOMIZE:
            seed_random(at_statement_end() ? plat_clock_seconds() :
                                             num_expr());
            break;
        case KW_SWAP:
            do_swap();
            break;
        case KW_MIDS:
            do_mid_assignment();
            break;
        case KW_CLS:
            if(!at_statement_end())
                num_expr();
            scr_cls();
            break;
        case KW_LOCATE:
            do_locate();
            break;
        case KW_COLOR:
            do_color();
            break;
        case KW_SCREEN:
            do_screen();
            break;
        case KW_PSET:
            do_pset(false);
            break;
        case KW_PRESET:
            do_pset(true);
            break;
        case KW_CIRCLE:
            do_circle();
            break;
        case KW_PAINT:
            do_paint();
            break;
        case KW_DRAW:
            v = string_expr();
            check_av(av_draw(v.s, v.len, scr.fg));
            break;
        case KW_PALETTE:
            do_palette();
            break;
        case KW_WINDOW:
            do_window();
            break;
        case KW_SOUND: {
            const double hz = num_expr();
            expect_punct(',');
            check_av(av_sound(hz, num_expr(), break_requested));
            break;
        }
        case KW_BEEP:
            check_av(av_beep(break_requested));
            break;
        case KW_PLAY:
            v = string_expr();
            check_av(av_play(v.s, v.len, break_requested));
            break;
        case KW_SLEEP:
            do_sleep();
            break;
        case KW_KEY:
            if(accept_kw(KW_OFF))
                scr.status_on = false;
            else if(accept_kw(KW_ON))
                scr.status_on = true;
            else
                skip_statement();
            scr.status_dirty = true;
            break;
        case KW_DEF:
        case KW_WIDTH:
        case KW_VIEW:
        case KW_POKE:
        case KW_OUT:
        case KW_WAIT:
        case KW_OPTION:
        case KW_DECLARE:
            skip_statement();
            break;
        default:
            syntax_error();
    }
    if(!at_statement_end())
        syntax_error();
    return true;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */

static void free_program(void) {
    int i;

    clear_variables();
    for(i = 0; i < nconsts; ++i)
        free(consts[i].s);
    for(i = 0; i < ndata; ++i)
        free(data_items[i].text.s);
    free(toks);
    free(lines);
    free(sorted_lines);
    free(consts);
    free(syms);
    free(data_items);
    toks = NULL;
    lines = NULL;
    sorted_lines = NULL;
    consts = NULL;
    syms = NULL;
    data_items = NULL;
    ntoks = cap_toks = nlines = cap_lines = nconsts = cap_consts = 0;
    nsyms = cap_syms = ndata = cap_data = 0;
    tmp_release(true);
}

static void load_program(const char *source) {
    int source_line = 1, i;

    while(*source) {
        const char *end = strchr(source, '\n');
        const size_t length = end ? (size_t)(end - source) : strlen(source);
        char *copy = xrealloc(NULL, length + 1);

        memcpy(copy, source, length);
        copy[length] = '\0';
        cur_line = nlines;
        result->source_line = source_line;
        tokenize_line(copy, source_line++);
        free(copy);
        source += length + (end ? 1 : 0);
    }
    emit(T_END, 0, 0, 0);
    emit(T_END, 0, 0, 0);
    emit(T_END, 0, 0, 0);

    sorted_lines = xrealloc(NULL, sizeof(int) * (size_t)(nlines + 1));
    for(i = 0; i < nlines; ++i)
        sorted_lines[i] = i;
    qsort(sorted_lines, (size_t)nlines, sizeof(int), compare_line_numbers);
    scan_definitions();
}

void basic_run(const char *source, basic_result_t *out) {
    result = out;
    memset(result, 0, sizeof(*result));
    result->line_number = -1;

    memset(deftype, ' ', sizeof(deftype));
    nframes = 0;
    data_at = 0;
    pc = cur_line = 0;
    expr_depth = 0;
    poll_counter = 0;
    next_status_at = 0;
    window.on = false;
    rnd_state = 0x2545f491u;
    rnd_last = 0;

    scr_set_mode(0);
    scr.status_on = true;
    scr.cursor_on = false;
    av_reset();
    plat_flush_keys();

    if(setjmp(bail) == 0) {
        load_program(source);
        pc = 0;
        cur_line = 0;
        while(cur_line < nlines) {
            const tok_t *t = &toks[pc];
            if(t->type == T_EOL) {
                if(++cur_line < nlines)
                    pc = lines[cur_line].start;
                continue;
            }
            if(t->type == T_END)
                break;
            if(t->type == T_PUNCT && t->id == ':') {
                ++pc;
                continue;
            }
            expr_depth = 0;
            if(!statement())
                break;
            if(arena && (arena->used || arena->next))
                tmp_release(false);
            if((++poll_counter & 31) == 0)
                poll();
        }
        check_av(av_wait_sound(break_requested));
    }
    if(result->status != BASIC_OK && cur_line < nlines) {
        result->source_line = lines[cur_line].source_line;
        result->line_number = lines[cur_line].number;
    }
    scr.cursor_on = false;
    plat_tone_stop();
    free_program();
}
