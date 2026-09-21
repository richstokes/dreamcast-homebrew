/*
 * Host harness: runs the interpreter, or the whole editor application, with
 * scripted keys, virtual time and a directory standing in for the VMU.
 *
 *   host-basic run PROGRAM.bas [options]    run one program
 *   host-basic app [options]                drive the editor
 *
 * Options: --keys FILE   keys to type; {F5}, {UP}, {ESC}... name special keys,
 *                        {SNAP file.ppm} saves the screen, {WAIT ms} passes
 *                        time, {IDLE n} answers the next n key polls with
 *                        "no key" so INKEY$ loops get to run
 *          --snap FILE   save the final screen as a PPM
 *          --tones       log every queued tone
 *          --store DIR   directory used as the VMU
 *          --examples DIR
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../app.h"
#include "../basic.h"
#include "../platform.h"
#include "../screen.h"

static char *script;
static size_t script_at;
static const char *store_dir = ".";
static const char *examples_dir = "../examples";
static bool log_tones;
static bool break_flag;
static double now_seconds;
static double tone_ends_at;
static long idle_polls;
static int empty_reads;
static long idle_reads;

static char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    char *data;
    long length;

    if(!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)length + 1);
    if(data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    if(data) {
        data[length] = '\0';
        if(size)
            *size = (size_t)length;
    }
    return data;
}

static void save_ppm(const char *path) {
    static uint8_t mask[SCR_CELL_H][SCR_W];
    FILE *file = fopen(path, "wb");
    int x, y;

    if(!file) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    scr_status_mask(mask);
    fprintf(file, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for(y = 0; y < SCR_H; ++y) {
        const int status_row = y - scr_status_y();
        for(x = 0; x < SCR_W; ++x) {
            uint32_t rgb = scr.palette[scr.pixels[y * SCR_W + x]];
            if(scr.status_on && status_row >= 0 && status_row < SCR_CELL_H)
                rgb = mask[status_row][x] ? 0x000000 : 0x00aaaa;
            fputc((int)(rgb >> 16 & 255), file);
            fputc((int)(rgb >> 8 & 255), file);
            fputc((int)(rgb & 255), file);
        }
    }
    fclose(file);
}

void plat_poll(void) {
    now_seconds += 0.0002;
    if(++idle_polls > 300000) {
        printf("\n[host: program is not reading input; stopping]\n");
        exit(3);
    }
}

static int special_key(const char *name) {
    static const struct { const char *name; int key; } table[] = {
        {"UP", K_UP}, {"DOWN", K_DOWN}, {"LEFT", K_LEFT}, {"RIGHT", K_RIGHT},
        {"HOME", K_HOME}, {"END", K_END}, {"PGUP", K_PGUP},
        {"PGDN", K_PGDN}, {"INS", K_INS}, {"DEL", K_DEL},
        {"CTRL_HOME", K_CTRL_HOME}, {"CTRL_END", K_CTRL_END}, {"F1", K_F1},
        {"F2", K_F2}, {"F3", K_F3}, {"F4", K_F4}, {"F5", K_F5}, {"F6", K_F6}, {"F7", K_F7},
        {"F9", K_F9}, {"ESC", 27}, {"ENTER", 13}, {"BS", 8}, {"TAB", 9},
        {"CTRL_C", 3}, {"CTRL_Y", 25}, {"LBRACE", '{'}
    };
    size_t i;
    for(i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if(strcmp(table[i].name, name) == 0)
            return table[i].key;
    }
    fprintf(stderr, "unknown key {%s}\n", name);
    exit(2);
}

int plat_key(void) {
    for(;;) {
        char name[256];
        char *close;
        int ch;

        if(idle_reads > 0) {
            --idle_reads;
            return 0;
        }
        if(!script || !script[script_at]) {
            if(++empty_reads < 200000)
                return 0;
            printf("\n[host: input exhausted]\n");
            if(getenv("HOST_FINAL_SNAP"))
                save_ppm(getenv("HOST_FINAL_SNAP"));
            exit(0);
        }
        idle_polls = 0;
        ch = (unsigned char)script[script_at++];
        if(ch == '\r')
            continue;
        if(ch == '\n')
            return 13;
        if(ch != '{')
            return ch;
        close = strchr(script + script_at, '}');
        if(!close || (size_t)(close - (script + script_at)) >= sizeof(name)) {
            fprintf(stderr, "unterminated { in key script\n");
            exit(2);
        }
        memcpy(name, script + script_at, (size_t)(close - script - script_at));
        name[close - script - script_at] = '\0';
        script_at = (size_t)(close - script) + 1;
        if(script[script_at] == '\n')
            ++script_at;            /* a directive may sit on its own line */
        if(strncmp(name, "SNAP ", 5) == 0) {
            save_ppm(name + 5);
            continue;
        }
        if(strncmp(name, "IDLE ", 5) == 0) {
            idle_reads = atol(name + 5);
            continue;
        }
        if(strncmp(name, "WAIT ", 5) == 0) {
            now_seconds += atof(name + 5) / 1000.0;
            continue;
        }
        ch = special_key(name);
        if(ch == 27 || ch == 3)
            break_flag = true;
        return ch;
    }
}

void plat_flush_keys(void) { break_flag = false; }

bool plat_break_pressed(void) {
    const bool pressed = break_flag;
    break_flag = false;
    return pressed;
}

double plat_seconds(void) { return now_seconds; }
double plat_clock_seconds(void) { return 36000.0 + now_seconds; }
void plat_sleep_ms(int ms) { now_seconds += ms / 1000.0; }

void plat_tone(double hz, int ms) {
    if(log_tones)
        printf("[tone %.1fHz %dms]\n", hz, ms);
    if(tone_ends_at < now_seconds)
        tone_ends_at = now_seconds;
    tone_ends_at += ms / 1000.0;
}

int plat_tone_queued_ms(void) {
    return tone_ends_at > now_seconds ?
           (int)((tone_ends_at - now_seconds) * 1000.0 + 0.999) : 0;
}

void plat_tone_stop(void) { tone_ends_at = 0; }
int plat_stick(int axis) { (void)axis; return 0; }
bool plat_button(int button) { (void)button; return false; }

static int list_directory(const char *path, char names[][PLAT_NAME_MAX + 1],
                          int max) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    int count = 0, i, k;

    if(!dir)
        return 0;
    while(count < max && (entry = readdir(dir)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        const size_t length = dot ? (size_t)(dot - entry->d_name) : 0;
        if(!dot || (strcmp(dot, ".BAS") != 0 && strcmp(dot, ".bas") != 0) ||
           length < 1 || length > PLAT_NAME_MAX)
            continue;
        for(k = 0; k < (int)length; ++k) {
            const char c = entry->d_name[k];
            names[count][k] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        names[count++][length] = '\0';
    }
    closedir(dir);
    for(i = 1; i < count; ++i) {
        for(k = i; k > 0 && strcmp(names[k - 1], names[k]) > 0; --k) {
            char swap[PLAT_NAME_MAX + 1];
            strcpy(swap, names[k]);
            strcpy(names[k], names[k - 1]);
            strcpy(names[k - 1], swap);
        }
    }
    return count;
}

static const char *load_from(const char *dir, const char *name, char **text,
                             size_t *size) {
    char path[1024], lower[PLAT_NAME_MAX + 1];
    int i;

    snprintf(path, sizeof(path), "%s/%s.BAS", dir, name);
    *text = read_file(path, size);
    if(!*text) {
        for(i = 0; name[i]; ++i)
            lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ?
                              name[i] + 32 : name[i]);
        lower[i] = '\0';
        snprintf(path, sizeof(path), "%s/%s.bas", dir, lower);
        *text = read_file(path, size);
    }
    return *text ? NULL : "File not found";
}

int plat_storage_list(char names[][PLAT_NAME_MAX + 1], int max) {
    return list_directory(store_dir, names, max);
}

const char *plat_storage_save(const char *name, const char *text,
                              size_t size) {
    char path[1024];
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s.BAS", store_dir, name);
    file = fopen(path, "wb");
    if(!file)
        return "Cannot write file";
    fwrite(text, 1, size, file);
    fclose(file);
    return NULL;
}

const char *plat_storage_load(const char *name, char **text, size_t *size) {
    return load_from(store_dir, name, text, size);
}

int plat_examples_list(char names[][PLAT_NAME_MAX + 1], int max) {
    return list_directory(examples_dir, names, max);
}

const char *plat_example_load(const char *name, char **text, size_t *size) {
    return load_from(examples_dir, name, text, size);
}

void plat_debug_output(const char *text, size_t size) {
    fwrite(text, 1, size, stdout);
}

int main(int argc, char **argv) {
    const char *program = NULL, *snap = NULL;
    const bool app_mode = argc > 1 && strcmp(argv[1], "app") == 0;
    int i;

    if(argc < 2 || (!app_mode && (strcmp(argv[1], "run") != 0 || argc < 3))) {
        fprintf(stderr, "usage: %s run PROGRAM.bas|app [options]\n", argv[0]);
        return 2;
    }
    i = 2;
    if(!app_mode)
        program = argv[i++];
    for(; i < argc; ++i) {
        if(strcmp(argv[i], "--tones") == 0)
            log_tones = true;
        else if(i + 1 >= argc) {
            fprintf(stderr, "%s needs a value\n", argv[i]);
            return 2;
        }
        else if(strcmp(argv[i], "--keys") == 0)
            script = read_file(argv[++i], NULL);
        else if(strcmp(argv[i], "--snap") == 0)
            snap = argv[++i];
        else if(strcmp(argv[i], "--store") == 0)
            store_dir = argv[++i];
        else if(strcmp(argv[i], "--examples") == 0)
            examples_dir = argv[++i];
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if(snap)
        setenv("HOST_FINAL_SNAP", snap, 1);

    if(app_mode) {
        app_init();
        for(;;)
            app_step();
    }
    else {
        basic_result_t outcome;
        char *source = read_file(program, NULL);

        if(!source) {
            fprintf(stderr, "cannot read %s\n", program);
            return 2;
        }
        scr_init();
        basic_run(source, &outcome);
        free(source);
        if(outcome.status == BASIC_ERROR) {
            if(outcome.line_number >= 0)
                printf("\n[%s in line %d]\n", outcome.message,
                       outcome.line_number);
            else
                printf("\n[%s at Ln %d]\n", outcome.message,
                       outcome.source_line);
        }
        else if(outcome.status == BASIC_BREAK) {
            printf("\n[Break]\n");
        }
        if(snap)
            save_ppm(snap);
        return outcome.status == BASIC_ERROR ? 1 : 0;
    }
}
