/*
 * The full-screen program editor, its dialogs, and the run loop around the
 * interpreter. Portable: all input, storage and display go through
 * platform.h and screen.h.
 */
#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic.h"
#include "platform.h"
#include "screen.h"

#define MAX_LINES 6000
#define MAX_LINE_LEN 255
#define TAB_WIDTH 4
#define GUTTER 5                    /* line-number margin, in columns */

enum {
    C_BACK = 1, C_TEXT = 15, C_GUTTER_BACK = 1, C_GUTTER = 11,
    C_TITLE_BACK = 7, C_TITLE = 0, C_BOX_BACK = 7, C_BOX = 0,
    C_FIELD_BACK = 0, C_FIELD = 15, C_SELECT_BACK = 3, C_SELECT = 15
};

static char *text_lines[MAX_LINES];
static int line_count;
static int cur_row, cur_col;        /* cursor in the document */
static int want_col;                /* column kept while moving vertically */
static int top_row, left_col;       /* scroll position */
static bool modified;
static char program_name[PLAT_NAME_MAX + 1];
static char notice[160];

/* ------------------------------------------------------------------------ */
/* Document                                                                  */

static char *new_line(const char *text, size_t length) {
    char *line = malloc(MAX_LINE_LEN + 1);
    if(!line)
        return NULL;
    if(length > MAX_LINE_LEN)
        length = MAX_LINE_LEN;
    memcpy(line, text, length);
    line[length] = '\0';
    return line;
}

static void clear_document(void) {
    int i;
    for(i = 0; i < line_count; ++i)
        free(text_lines[i]);
    line_count = 0;
    text_lines[line_count++] = new_line("", 0);
    cur_row = cur_col = want_col = top_row = left_col = 0;
    modified = false;
}

static void set_document(const char *text, size_t size) {
    size_t at = 0;
    int i;

    for(i = 0; i < line_count; ++i)
        free(text_lines[i]);
    line_count = 0;
    while(at < size && line_count < MAX_LINES) {
        size_t end = at, length, k;
        char *line;

        while(end < size && text[end] != '\n')
            ++end;
        length = end - at;
        if(length && text[at + length - 1] == '\r')
            --length;
        line = new_line(text + at, length);
        if(!line)
            break;
        for(k = 0; line[k]; ++k) {
            if(line[k] == '\t' || (unsigned char)line[k] < 32)
                line[k] = ' ';
        }
        text_lines[line_count++] = line;
        at = end + 1;
    }
    if(!line_count)
        text_lines[line_count++] = new_line("", 0);
    cur_row = cur_col = want_col = top_row = left_col = 0;
    modified = false;
}

static char *document_text(size_t *size) {
    size_t total = 1, at = 0;
    char *text;
    int i;

    for(i = 0; i < line_count; ++i)
        total += strlen(text_lines[i]) + 1;
    text = malloc(total);
    if(!text)
        return NULL;
    for(i = 0; i < line_count; ++i) {
        const size_t length = strlen(text_lines[i]);
        memcpy(text + at, text_lines[i], length);
        at += length;
        text[at++] = '\n';
    }
    text[at] = '\0';
    *size = at;
    return text;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */

static int edit_rows(void) {
    return scr.rows - 2;            /* title row above, status row below */
}

static int edit_cols(void) {
    return scr.cols - GUTTER;
}

static void draw_status(void) {
    char text[200];

    if(notice[0]) {
        snprintf(text, sizeof(text), " %s", notice);
    }
    else {
        snprintf(text, sizeof(text),
                 " Ln %d, Col %d  F1 Help F2 Examples F5 Run F6 Save F7 Load"
                 " F9 New", cur_row + 1, cur_col + 1);
    }
    scr_set_status(text);
}

static void draw_editor(void) {
    char title[SCR_STATUS_MAX + 1], number[16];
    int row, col;

    scr.bg = C_BACK;
    snprintf(title, sizeof(title), " Dreamcast BASIC - %s%s",
             program_name[0] ? program_name : "UNTITLED",
             modified ? " *" : "");
    scr_fill_cells(0, 0, scr.cols, 1, C_TITLE_BACK);
    scr_draw_text(0, 0, title, C_TITLE, C_TITLE_BACK);

    for(row = 0; row < edit_rows(); ++row) {
        const int doc_row = top_row + row;
        const char *line = doc_row < line_count ? text_lines[doc_row] : "";
        const int length = (int)strlen(line);

        if(doc_row < line_count)
            snprintf(number, sizeof(number), "%4d ", doc_row + 1);
        else
            snprintf(number, sizeof(number), "     ");
        scr_draw_text(0, row + 1, number, C_GUTTER, C_GUTTER_BACK);
        for(col = 0; col < edit_cols(); ++col) {
            const int doc_col = left_col + col;
            scr_draw_char(GUTTER + col, row + 1,
                          doc_col < length ? (unsigned char)line[doc_col] :
                                             ' ',
                          C_TEXT, C_BACK);
        }
    }
    scr.cur_row = cur_row - top_row + 1;
    scr.cur_col = GUTTER + cur_col - left_col;
    scr.cursor_on = true;
    draw_status();
}

static void keep_cursor_visible(void) {
    const int length = (int)strlen(text_lines[cur_row]);

    if(cur_col > length)
        cur_col = length;
    if(cur_row < top_row)
        top_row = cur_row;
    if(cur_row >= top_row + edit_rows())
        top_row = cur_row - edit_rows() + 1;
    if(cur_col < left_col)
        left_col = cur_col;
    if(cur_col >= left_col + edit_cols())
        left_col = cur_col - edit_cols() + 1;
}

static void enter_editor_screen(void) {
    scr_set_mode(0);
    scr.status_on = true;
    scr.bg = C_BACK;
    scr.fg = C_TEXT;
    scr_cls();
}

/* ------------------------------------------------------------------------ */
/* Dialogs                                                                   */

static int wait_key(void) {
    int key;
    while((key = plat_key()) == 0) {
        plat_poll();
        plat_sleep_ms(5);
    }
    plat_break_pressed();
    return key;
}

static void draw_box(int col, int row, int width, int height,
                     const char *title) {
    int i;

    scr_fill_cells(col, row, width, height, C_BOX_BACK);
    for(i = 0; i < width; ++i) {
        scr_draw_char(col + i, row, 196, C_BOX, C_BOX_BACK);
        scr_draw_char(col + i, row + height - 1, 196, C_BOX, C_BOX_BACK);
    }
    scr_draw_char(col + 1, row, ' ', C_BOX, C_BOX_BACK);
    scr_draw_text(col + 2, row, title, C_BOX, C_BOX_BACK);
    scr_draw_char(col + 2 + (int)strlen(title), row, ' ', C_BOX, C_BOX_BACK);
}

static void message_box(const char *title, const char *const *body,
                        int count) {
    int width = (int)strlen(title) + 6, i;
    int col, row;

    for(i = 0; i < count; ++i) {
        if((int)strlen(body[i]) + 4 > width)
            width = (int)strlen(body[i]) + 4;
    }
    if(width > scr.cols)
        width = scr.cols;
    col = (scr.cols - width) / 2;
    row = (scr.rows - count - 4) / 2;
    scr.cursor_on = false;
    draw_box(col, row, width, count + 4, title);
    for(i = 0; i < count; ++i)
        scr_draw_text(col + 2, row + 2 + i, body[i], C_BOX, C_BOX_BACK);
    scr_set_status(" Press any key");
    wait_key();
}

static void alert(const char *title, const char *text) {
    const char *body[1];
    body[0] = text;
    message_box(title, body, 1);
}

static bool confirm(const char *title, const char *question) {
    const char *body[1];
    int col, row, width = (int)strlen(question) + 4, key;

    body[0] = question;
    if(width < (int)strlen(title) + 6)
        width = (int)strlen(title) + 6;
    col = (scr.cols - width) / 2;
    row = (scr.rows - 5) / 2;
    scr.cursor_on = false;
    draw_box(col, row, width, 5, title);
    scr_draw_text(col + 2, row + 2, body[0], C_BOX, C_BOX_BACK);
    scr_set_status(" Y Yes   N No");
    do {
        key = toupper(wait_key());
    } while(key != 'Y' && key != 'N' && key != 27 && key != 13);
    return key == 'Y' || key == 13;
}

/*
 * Asks for a program name, with an optional list to pick from. Up and Down
 * copy a listed name into the field. Returns false when cancelled.
 */
static bool name_dialog(const char *title, char names[][PLAT_NAME_MAX + 1],
                        int count, char *name) {
    const int visible = count > 10 ? 10 : count;
    const int width = 40, height = 6 + (count ? visible + 2 : 0);
    const int col = (scr.cols - width) / 2, row = (scr.rows - height) / 2;
    int selected = -1, first = 0, length = (int)strlen(name), i;

    for(i = 0; i < count; ++i) {
        if(strcmp(names[i], name) == 0)
            selected = i;
    }
    for(;;) {
        int key;

        if(selected >= 0) {
            if(selected < first)
                first = selected;
            if(selected >= first + visible)
                first = selected - visible + 1;
        }
        draw_box(col, row, width, height, title);
        scr_draw_text(col + 2, row + 2, "Name:", C_BOX, C_BOX_BACK);
        scr_fill_cells(col + 8, row + 2, PLAT_NAME_MAX + 1, 1, C_FIELD_BACK);
        scr_draw_text(col + 8, row + 2, name, C_FIELD, C_FIELD_BACK);
        scr_draw_text(col + 8 + PLAT_NAME_MAX + 1, row + 2, ".BAS", C_BOX,
                      C_BOX_BACK);
        if(count) {
            scr_draw_text(col + 2, row + 4, "On this VMU / disc:", C_BOX,
                          C_BOX_BACK);
            for(i = 0; i < visible; ++i) {
                const int item = first + i;
                const bool hot = item == selected;
                scr_fill_cells(col + 4, row + 5 + i, PLAT_NAME_MAX + 6, 1,
                               hot ? C_SELECT_BACK : C_BOX_BACK);
                scr_draw_text(col + 5, row + 5 + i, names[item],
                              hot ? C_SELECT : C_BOX,
                              hot ? C_SELECT_BACK : C_BOX_BACK);
            }
        }
        scr.cur_row = row + 2;
        scr.cur_col = col + 8 + length;
        scr.cursor_on = true;
        scr_set_status(count ?
            " Type a name or pick with Up/Down   Enter OK   Esc Cancel" :
            " Type a name   Enter OK   Esc Cancel");

        key = wait_key();
        if(key == 27) {
            scr.cursor_on = false;
            return false;
        }
        if(key == 13 && length > 0) {
            scr.cursor_on = false;
            return true;
        }
        if((key == K_UP || key == K_DOWN) && count) {
            if(selected < 0)
                selected = key == K_DOWN ? 0 : count - 1;
            else
                selected = (selected + (key == K_DOWN ? 1 : count - 1)) %
                           count;
            strcpy(name, names[selected]);
            length = (int)strlen(name);
        }
        else if(key == 8 && length > 0) {
            name[--length] = '\0';
            selected = -1;
        }
        else if(key < 256 && (isalnum(key) || key == '_' || key == '-') &&
                (length < PLAT_NAME_MAX || selected >= 0)) {
            if(selected >= 0)
                length = 0;         /* typing replaces a picked name */
            name[length++] = (char)toupper(key);
            name[length] = '\0';
            selected = -1;
        }
    }
}

static bool discard_changes(void) {
    return !modified ||
           confirm("Unsaved changes", "Discard the changes to this program?");
}

static void save_program(void) {
    static char names[PLAT_LIST_MAX][PLAT_NAME_MAX + 1];
    char name[PLAT_NAME_MAX + 1];
    const char *error;
    char *text;
    size_t size;
    int count, i;
    bool exists = false;

    strcpy(name, program_name);
    count = plat_storage_list(names, PLAT_LIST_MAX);
    if(!name_dialog("Save program to VMU", names, count, name))
        return;
    for(i = 0; i < count; ++i)
        exists = exists || strcmp(names[i], name) == 0;
    if(exists && strcmp(name, program_name) != 0 &&
       !confirm("Save program to VMU", "Replace the existing program?"))
        return;
    text = document_text(&size);
    if(!text) {
        alert("Save failed", "Out of memory");
        return;
    }
    scr_set_status(" Saving to VMU...");
    plat_poll();
    error = plat_storage_save(name, text, size);
    free(text);
    if(error) {
        alert("Save failed", error);
        return;
    }
    strcpy(program_name, name);
    modified = false;
    snprintf(notice, sizeof(notice), "Saved %s.BAS to VMU", name);
}

static void load_program(bool example) {
    static char names[PLAT_LIST_MAX][PLAT_NAME_MAX + 1];
    char name[PLAT_NAME_MAX + 1] = "";
    const char *error;
    char *text = NULL;
    size_t size = 0;
    int count;

    if(!discard_changes())
        return;
    count = example ? plat_examples_list(names, PLAT_LIST_MAX) :
                      plat_storage_list(names, PLAT_LIST_MAX);
    if(!count) {
        alert(example ? "Examples" : "Load program from VMU",
              example ? "No examples are available" :
                        "No BASIC programs were found on a VMU");
        return;
    }
    if(example)
        strcpy(name, names[0]);
    if(!name_dialog(example ? "Load example" : "Load program from VMU",
                    names, count, name))
        return;
    scr_set_status(" Loading...");
    plat_poll();
    error = example ? plat_example_load(name, &text, &size) :
                      plat_storage_load(name, &text, &size);
    if(error) {
        alert("Load failed", error);
        return;
    }
    set_document(text, size);
    free(text);
    strcpy(program_name, example ? "" : name);
    snprintf(notice, sizeof(notice), "Loaded %s.BAS", name);
}

static void show_help(void) {
    static const char *const body[] = {
        "Type a BASIC program, then press F5 to run it.",
        "",
        "F1  This help            F6  Save to VMU",
        "F2  Load an example      F7  Load from VMU",
        "F5  Run the program      F9  New program",
        "",
        "Ctrl+Y deletes a line. Esc or Ctrl+C stops a running",
        "program. Line numbers are optional; GOTO and GOSUB",
        "accept line numbers or labels.",
        "",
        "Text:     PRINT [USING], INPUT, LOCATE, COLOR, CLS",
        "Graphics: SCREEN, PSET, LINE, CIRCLE, PAINT, DRAW,",
        "          PALETTE, WINDOW, POINT()",
        "Sound:    PLAY, SOUND, BEEP",
        "Input:    INKEY$, INPUT$, STICK(), STRIG(), TIMER",
        "",
        "KEY OFF hides this status bar while a program runs."
    };
    message_box("Dreamcast BASIC", body,
                (int)(sizeof(body) / sizeof(body[0])));
}

/* ------------------------------------------------------------------------ */
/* Running                                                                   */

static void run_program(void) {
    basic_result_t outcome;
    char status[160];
    size_t size;
    char *text = document_text(&size);

    if(!text) {
        alert("Run", "Out of memory");
        return;
    }
    scr.cursor_on = false;
    basic_run(text, &outcome);
    free(text);

    scr.status_on = true;
    if(outcome.status == BASIC_OK) {
        snprintf(status, sizeof(status),
                 " Program ended - press any key to return to the editor");
    }
    else if(outcome.line_number >= 0) {
        snprintf(status, sizeof(status), " %s in line %d - press any key",
                 outcome.message, outcome.line_number);
    }
    else {
        snprintf(status, sizeof(status), " %s at Ln %d - press any key",
                 outcome.message, outcome.source_line);
    }
    printf("Dreamcast BASIC:%s\n", status);
    scr_set_status(status);
    scr.status_dirty = true;
    plat_flush_keys();
    wait_key();

    if(outcome.status != BASIC_OK && outcome.source_line >= 1 &&
       outcome.source_line <= line_count) {
        cur_row = outcome.source_line - 1;
        cur_col = want_col = 0;
        if(outcome.line_number >= 0) {
            snprintf(notice, sizeof(notice), "%s in line %d (Ln %d)",
                     outcome.message, outcome.line_number,
                     outcome.source_line);
        }
        else {
            snprintf(notice, sizeof(notice), "%s at Ln %d", outcome.message,
                     outcome.source_line);
        }
    }
    enter_editor_screen();
}

/* ------------------------------------------------------------------------ */
/* Editing                                                                   */

static void insert_char(int ch) {
    char *line = text_lines[cur_row];
    const int length = (int)strlen(line);

    if(length >= MAX_LINE_LEN)
        return;
    memmove(line + cur_col + 1, line + cur_col,
            (size_t)(length - cur_col + 1));
    line[cur_col++] = (char)ch;
    modified = true;
}

static void split_line(void) {
    char *line = text_lines[cur_row];
    char *rest;

    if(line_count >= MAX_LINES)
        return;
    rest = new_line(line + cur_col, strlen(line + cur_col));
    if(!rest)
        return;
    line[cur_col] = '\0';
    memmove(&text_lines[cur_row + 2], &text_lines[cur_row + 1],
            sizeof(char *) * (size_t)(line_count - cur_row - 1));
    text_lines[++cur_row] = rest;
    ++line_count;
    cur_col = 0;
    modified = true;
}

static void remove_line(int row) {
    free(text_lines[row]);
    memmove(&text_lines[row], &text_lines[row + 1],
            sizeof(char *) * (size_t)(line_count - row - 1));
    --line_count;
    if(!line_count)
        text_lines[line_count++] = new_line("", 0);
    modified = true;
}

/* Joins `row + 1` onto the end of `row` when the result fits. */
static bool join_lines(int row) {
    char *line = text_lines[row];
    const size_t length = strlen(line);

    if(row + 1 >= line_count ||
       length + strlen(text_lines[row + 1]) > MAX_LINE_LEN)
        return false;
    strcpy(line + length, text_lines[row + 1]);
    remove_line(row + 1);
    return true;
}

static void edit_key(int key) {
    char *line = text_lines[cur_row];
    const int length = (int)strlen(line);
    bool vertical = false;

    notice[0] = '\0';
    switch(key) {
        case K_LEFT:
            if(cur_col > 0)
                --cur_col;
            else if(cur_row > 0)
                cur_col = (int)strlen(text_lines[--cur_row]);
            break;
        case K_RIGHT:
            if(cur_col < length)
                ++cur_col;
            else if(cur_row + 1 < line_count) {
                ++cur_row;
                cur_col = 0;
            }
            break;
        case K_UP:
            if(cur_row > 0)
                --cur_row;
            vertical = true;
            break;
        case K_DOWN:
            if(cur_row + 1 < line_count)
                ++cur_row;
            vertical = true;
            break;
        case K_PGUP:
            cur_row -= edit_rows() - 1;
            if(cur_row < 0)
                cur_row = 0;
            vertical = true;
            break;
        case K_PGDN:
            cur_row += edit_rows() - 1;
            if(cur_row >= line_count)
                cur_row = line_count - 1;
            vertical = true;
            break;
        case K_HOME:
            cur_col = 0;
            break;
        case K_END:
            cur_col = length;
            break;
        case K_CTRL_HOME:
            cur_row = cur_col = 0;
            break;
        case K_CTRL_END:
            cur_row = line_count - 1;
            cur_col = (int)strlen(text_lines[cur_row]);
            break;
        case 13:
            split_line();
            break;
        case 8:
            if(cur_col > 0) {
                memmove(line + cur_col - 1, line + cur_col,
                        (size_t)(length - cur_col + 1));
                --cur_col;
                modified = true;
            }
            else if(cur_row > 0) {
                const int joined_at = (int)strlen(text_lines[cur_row - 1]);
                if(join_lines(cur_row - 1)) {
                    --cur_row;
                    cur_col = joined_at;
                }
            }
            break;
        case K_DEL:
            if(cur_col < length) {
                memmove(line + cur_col, line + cur_col + 1,
                        (size_t)(length - cur_col));
                modified = true;
            }
            else {
                join_lines(cur_row);
            }
            break;
        case 25:                    /* Ctrl+Y */
            remove_line(cur_row);
            if(cur_row >= line_count)
                cur_row = line_count - 1;
            cur_col = 0;
            break;
        case 9:
            do {
                insert_char(' ');
            } while(cur_col % TAB_WIDTH);
            break;
        default:
            if(key >= 32 && key < 127)
                insert_char(key);
            break;
    }
    if(vertical) {
        const int target = (int)strlen(text_lines[cur_row]);
        cur_col = want_col < target ? want_col : target;
    }
    else {
        want_col = cur_col;
    }
}

void app_init(void) {
    scr_init();
    clear_document();
    program_name[0] = '\0';
    notice[0] = '\0';
    enter_editor_screen();
}

void app_load_text(const char *name, const char *text, size_t size) {
    set_document(text, size);
    snprintf(program_name, sizeof(program_name), "%s", name);
}

void app_run_now(void) {
    run_program();
}

void app_step(void) {
    int key;

    keep_cursor_visible();
    draw_editor();
    key = wait_key();
    switch(key) {
        case K_F1:
            show_help();
            break;
        case K_F2:
            load_program(true);
            break;
        case K_F5:
            run_program();
            break;
        case K_F6:
            save_program();
            break;
        case K_F7:
            load_program(false);
            break;
        case K_F9:
            if(discard_changes()) {
                clear_document();
                program_name[0] = '\0';
            }
            break;
        default:
            edit_key(key);
            break;
    }
}
