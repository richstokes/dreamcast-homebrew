/* LemonPlay: a DLNA media client for the Dreamcast.

   Screens: network start, server list (SSDP discovery or a typed address),
   folder browser, and the player. Anything that waits on the network runs
   as a job on a worker thread so the screen keeps drawing. */
#include "bench.h"
#include "gfx.h"
#include "http.h"
#include "input.h"
#include "netlink.h"
#include "player.h"
#include "settings.h"
#include "ui.h"
#include "upnp.h"

#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

#ifndef LP_START_SECONDS
#define LP_START_SECONDS 0
#endif

#define ROWS 10
#define ROW_H 30
#define LIST_Y (HEADER_H + 14)
#define MAX_DEPTH 16

enum { SCREEN_NETWORK, SCREEN_SERVERS, SCREEN_ADDRESS, SCREEN_BROWSE };
enum { JOB_NONE, JOB_DISCOVER, JOB_DESCRIBE, JOB_BROWSE };

static int screen = SCREEN_NETWORK;

/* Servers: discovered ones, then "Search again" and "Enter an address". */
static server_t servers[MAX_SERVERS];
static int server_count;
static int server_sel;
static server_t current;

static uint8_t address[4] = {192, 168, 1, 2};
static uint16_t port = 8200;
static int address_field;

/* Browsing */
static entry_t entries[MAX_ENTRIES];
static int entry_count;
static struct {
    char id[64];
    char title[120];
    int sel, top;
} stack[MAX_DEPTH];
static int depth;
static uint64_t sel_since;

static char message[96];
static uint64_t message_until;
static int message_is_error;

/* Worker job */
static volatile int job = JOB_NONE;
static volatile int job_done;
static int job_dropped;         /* the soft reset left the screen that wanted it */
static int job_result;
static int remembered_lookup;   /* JOB_DESCRIBE is for the saved server, not a typed one */
static char job_arg[256];
static server_t job_server;

static void say(const char *text) {
    snprintf(message, sizeof(message), "%s", text);
    message_until = timer_ms_gettime64() + 5000;
    message_is_error = 1;
}

static void cycle_picture(void) {
    settings.picture = (settings.picture + 1) % PICTURE_MODES;
    settings_changed();
    snprintf(message, sizeof(message), "Picture %s: %s", picture_name(settings.picture), picture_help(settings.picture));
    message_until = timer_ms_gettime64() + 3000;
    message_is_error = 0;
}

/* Fill the address screen from a saved server URL. */
static void address_from(const char *location) {
    url_t u;
    unsigned a, b, c, d;
    if(url_parse(location, &u) == 0 && sscanf(u.host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        address[0] = a;
        address[1] = b;
        address[2] = c;
        address[3] = d;
        port = u.port;
    }
}

static void *job_thread(void *unused) {
    (void)unused;
    switch(job) {
        case JOB_DISCOVER:
            job_result = upnp_discover(servers, MAX_SERVERS, 3000);
            break;
        case JOB_DESCRIBE:
            job_result = upnp_describe(job_arg, &job_server);
            break;
        case JOB_BROWSE:
            job_result = upnp_browse(&current, job_arg, entries, MAX_ENTRIES);
            break;
    }
    job_done = 1;
    return NULL;
}

static void job_start(int which) {
    job = which;
    job_done = 0;
    thd_create(1, job_thread, NULL);
}

static void browse(const char *id) {
    snprintf(job_arg, sizeof(job_arg), "%s", id);
    entry_count = 0;
    job_start(JOB_BROWSE);
}

static void open_server(const server_t *s) {
    current = *s;
    if(strcmp(settings.server, s->location) && strlen(s->location) < sizeof(settings.server)) {
        snprintf(settings.server, sizeof(settings.server), "%s", s->location);
        settings_changed();
    }
    depth = 0;
    snprintf(stack[0].id, sizeof(stack[0].id), "0");
    snprintf(stack[0].title, sizeof(stack[0].title), "%s", s->name);
    stack[0].sel = stack[0].top = 0;
    screen = SCREEN_BROWSE;
    browse("0");
}

static void job_finish(void) {
    int which = job;
    job = JOB_NONE;
    if(job_dropped) {
        job_dropped = 0;
        return;
    }
    if(which == JOB_DISCOVER) {
        server_count = job_result;
        server_sel = 0;
#ifdef LP_SERVER
        /* Builds with a server address add it when discovery missed it. */
        int listed = 0;
        for(int i = 0; i < server_count; i++) listed |= !strcmp(servers[i].location, LP_SERVER);
        if(!listed) {
            snprintf(job_arg, sizeof(job_arg), "%s", LP_SERVER);
            job_start(JOB_DESCRIBE);
            return;
        }
#endif
        printf("LemonPlay: found %d media server(s)\n", server_count);
#ifndef LP_SERVER
        /* The last server opened, in case discovery can't see it. */
        if(settings.server[0]) {
            int listed = 0;
            for(int i = 0; i < server_count; i++) listed |= !strcmp(servers[i].location, settings.server);
            if(!listed) {
                snprintf(job_arg, sizeof(job_arg), "%s", settings.server);
                job_start(JOB_DESCRIBE);
                remembered_lookup = 1;
                return;
            }
        }
#endif
    }
    else if(which == JOB_DESCRIBE) {
        int remembered = remembered_lookup;
        remembered_lookup = 0;
        if(remembered) {
            if(job_result == 0 && server_count < MAX_SERVERS) servers[server_count++] = job_server;
            return;
        }
        if(job_result == 0) {
            int dup = 0;
            for(int i = 0; i < server_count; i++) dup |= !strcmp(servers[i].location, job_server.location);
            if(!dup && server_count < MAX_SERVERS) servers[server_count++] = job_server;
#if !defined(LP_QA) && !defined(LP_UI_TOUR)
            open_server(&job_server);
#else
            screen = SCREEN_SERVERS;
#endif
        }
        else {
            say("No media server answered at that address");
            screen = SCREEN_SERVERS;
        }
    }
    else if(which == JOB_BROWSE) {
        if(job_result < 0) {
            entry_count = 0;
            say("Could not read this folder");
            return;
        }
        entry_count = job_result;
        /* A root holding a single folder (ZeroConfigDLNA's "Media Library")
           opens straight into it. */
        if(depth == 0 && entry_count == 1 && entries[0].kind == ENTRY_FOLDER && depth + 1 < MAX_DEPTH) {
            depth++;
            snprintf(stack[depth].id, sizeof(stack[depth].id), "%s", entries[0].id);
            snprintf(stack[depth].title, sizeof(stack[depth].title), "%s", entries[0].title);
            stack[depth].sel = stack[depth].top = 0;
            browse(entries[0].id);
            return;
        }
        if(stack[depth].sel >= entry_count) stack[depth].sel = entry_count ? entry_count - 1 : 0;
        sel_since = timer_ms_gettime64();
    }
}

/* A+B+X+Y+Start away from the server list: straight back to it. */
static void go_home(void) {
    if(screen == SCREEN_NETWORK) return;
    if(job != JOB_NONE) job_dropped = 1;
    screen = SCREEN_SERVERS;
}

/* ---- Playing ---- */

static int playable(const entry_t *e) {
    return e->playable && (e->kind == ENTRY_VIDEO || e->kind == ENTRY_AUDIO);
}

static double play_start = LP_START_SECONDS;

static void play_from(int index) {
    char error[96];
    while(index >= 0 && index < entry_count) {
        entry_t *e = &entries[index];
        stack[depth].sel = index;
        printf("LemonPlay: playing %s\n", e->title);
        play_result_t r = player_play(e, play_start, error, sizeof(error));
        if(r == PLAY_FAILED) {
            printf("LemonPlay: playback failed: %s\n", error);
            say(error);
            break;
        }
        if(r == PLAY_STOPPED) break;
        if(r == PLAY_RESET) {
            go_home();
            break;
        }
        int step = r == PLAY_PREVIOUS ? -1 : 1;
        int next = index + step;
        while(next >= 0 && next < entry_count && !(playable(&entries[next]) && entries[next].kind == e->kind)) next += step;
        index = next;
#if (defined(LP_QA) && !defined(LP_QA_END)) || defined(LP_BENCH)
        break;
#endif
    }
    sel_since = timer_ms_gettime64();
}

/* ---- Input ---- */

static void list_move(int *sel, int *top, int count, const input_t *in) {
    int old = *sel;
    if(in->pressed & CONT_DPAD_UP) (*sel)--;
    if(in->pressed & CONT_DPAD_DOWN) (*sel)++;
    if(in->pressed & BTN_LTRIG) *sel -= ROWS;
    if(in->pressed & BTN_RTRIG) *sel += ROWS;
    if(*sel < 0) *sel = (in->pressed & CONT_DPAD_UP) && old == 0 ? count - 1 : 0;
    if(*sel >= count) *sel = (in->pressed & CONT_DPAD_DOWN) && old == count - 1 ? 0 : count - 1;
    if(*sel < 0) *sel = 0;
    if(*sel < *top) *top = *sel;
    if(*sel >= *top + ROWS) *top = *sel - ROWS + 1;
    if(*sel != old) sel_since = timer_ms_gettime64();
}

static void update(const input_t *in) {
    if(job != JOB_NONE) {
        if(job_done) job_finish();
        if(job != JOB_NONE) {
            if(screen == SCREEN_BROWSE && (in->pressed & CONT_B) && job == JOB_BROWSE) {
                /* Let it finish in the background; its result is dropped. */
            }
            return;
        }
    }

    switch(screen) {
        case SCREEN_NETWORK:
#ifdef LP_UI_TOUR
            /* Hold the start screen long enough to photograph. */
            if(timer_ms_gettime64() < 6000) break;
#endif
            if(link_stage() == LINK_UP) {
                screen = SCREEN_SERVERS;
                job_start(JOB_DISCOVER);
            }
            break;

        case SCREEN_SERVERS: {
            int count = server_count + 2;
            int top = 0;
            list_move(&server_sel, &top, count, in);
            if(in->pressed & CONT_START) cycle_picture();
            if(in->pressed & CONT_A) {
                if(server_sel < server_count) open_server(&servers[server_sel]);
                else if(server_sel == server_count) job_start(JOB_DISCOVER);
                else {
                    if(settings.server[0]) address_from(settings.server);
                    screen = SCREEN_ADDRESS;
                }
            }
            break;
        }

        case SCREEN_ADDRESS:
            if(in->pressed & CONT_DPAD_LEFT) address_field = (address_field + 4) % 5;
            if(in->pressed & CONT_DPAD_RIGHT) address_field = (address_field + 1) % 5;
            if(in->pressed & (CONT_DPAD_UP | CONT_DPAD_DOWN | BTN_LTRIG | BTN_RTRIG)) {
                int d = (in->pressed & CONT_DPAD_UP) ? 1 : (in->pressed & CONT_DPAD_DOWN) ? -1 : (in->pressed & BTN_RTRIG) ? 10 : -10;
                if(address_field < 4) address[address_field] = (uint8_t)(address[address_field] + d);
                else port = (uint16_t)(port + d);
            }
            if(in->pressed & CONT_B) screen = SCREEN_SERVERS;
            if(in->pressed & CONT_A) {
                snprintf(job_arg, sizeof(job_arg), "http://%d.%d.%d.%d:%u/description.xml",
                         address[0], address[1], address[2], address[3], port);
                job_start(JOB_DESCRIBE);
            }
            break;

        case SCREEN_BROWSE: {
            if(in->pressed & CONT_B) {
                if(depth == 0 || (depth == 1 && !strcmp(stack[0].id, "0") && !strcmp(stack[1].id, "1"))) {
                    screen = SCREEN_SERVERS;
                }
                else {
                    depth--;
                    browse(stack[depth].id);
                }
                break;
            }
            if(in->pressed & CONT_Y) {
                browse(stack[depth].id);
                break;
            }
            if(in->pressed & CONT_START) cycle_picture();
            if(!entry_count) break;
            list_move(&stack[depth].sel, &stack[depth].top, entry_count, in);
            if(in->pressed & CONT_A) {
                entry_t *e = &entries[stack[depth].sel];
                if(e->kind == ENTRY_FOLDER) {
                    if(depth + 1 < MAX_DEPTH) {
                        depth++;
                        snprintf(stack[depth].id, sizeof(stack[depth].id), "%s", e->id);
                        snprintf(stack[depth].title, sizeof(stack[depth].title), "%s", e->title);
                        stack[depth].sel = stack[depth].top = 0;
                        browse(e->id);
                    }
                }
                else if(playable(e)) {
                    play_from(stack[depth].sel);
                }
                else if(e->kind == ENTRY_VIDEO || e->kind == ENTRY_AUDIO) {
                    say("Run the server with --dreamcast-mode to play this");
                }
                else {
                    say("LemonPlay can't show this kind of file yet");
                }
            }
            break;
        }
    }
}

/* ---- Drawing ---- */

static void format_duration(char *out, size_t size, int secs) {
    if(secs < 0) out[0] = '\0';
    else ui_format_time(out, (int)size, secs);
}

/* The selected row scrolls when its title is too long to fit. */
static void row_text(float x, float y, uint32_t col, const char *text, float max_w, int selected) {
    int fit = (int)(max_w / FONT_W);
    int len = strlen(text);
    if(!selected || len <= fit) {
        gfx_text_clip(x, y, col, 1, text, max_w);
        return;
    }
    uint64_t t = timer_ms_gettime64() - sel_since;
    int span = len - fit + 6;
    int off = t < 1200 ? 0 : (int)((t - 1200) / 160) % span;
    off -= 3;
    if(off < 0) off = 0;
    if(off > len - fit) off = len - fit;
    char part[160];
    snprintf(part, sizeof(part), "%.*s", fit, text + off);
    gfx_text(x, y, col, 1, part);
}

/* A list row: highlight, icon, title (scrolling if selected) and an
   optional right-hand note. */
static void draw_row(float y, int icon, uint32_t icon_col, const char *title, uint32_t col,
                     const char *note, int selected) {
    if(selected) {
        gfx_pill(SAFE_X - 10, y - 3, SCREEN_W - 2 * SAFE_X + 20, ROW_H, COL_SELECT);
        gfx_rect_alpha(SAFE_X - 10 + 6, y + 3, 3, ROW_H - 12, COL_LEMON);
    }
    gfx_icon(icon, SAFE_X + 4, y, 24, icon_col);
    float note_w = note && note[0] ? gfx_text_width(note, 1) + 16 : 0;
    row_text(SAFE_X + 38, y, selected ? COL_LEMON : col, title, SCREEN_W - SAFE_X - (SAFE_X + 38) - note_w, selected);
    if(note_w) gfx_text(SCREEN_W - SAFE_X - note_w + 16, y, COL_MUTED, 1, note);
}

static void draw_messages(void) {
    if(timer_ms_gettime64() < message_until) ui_toast(message, message_is_error);
}

static void picture_hint(void) {
    char label[24];
    snprintf(label, sizeof(label), "Picture %s", picture_name(settings.picture));
    ui_hint_right('S', label);
}

static void draw_network(void) {
    gfx_icon(ICON_LEMON, (SCREEN_W - 128) / 2, 96, 128, 0xffffffff);
    gfx_text((SCREEN_W - gfx_text_width("LemonPlay", 2)) / 2, 236, COL_LEMON, 2, "LemonPlay");
    const char *status = link_status();
    float w = gfx_text_width(status, 1);
    if(link_stage() == LINK_FAILED) {
        gfx_icon(ICON_WARNING, (SCREEN_W - w) / 2 - 32, 320, 24, COL_ERROR);
        gfx_text((SCREEN_W - w) / 2, 320, COL_ERROR, 1, status);
        const char *help = "LemonPlay needs a Broadband Adapter";
        gfx_text((SCREEN_W - gfx_text_width(help, 1)) / 2, 352, COL_MUTED, 1, help);
    }
    else {
        ui_spinner(SCREEN_W / 2, 314);
        gfx_text((SCREEN_W - w) / 2, 348, COL_MUTED, 1, status);
    }
}

static void draw_servers(void) {
    int count = server_count + 2;
    if(job == JOB_DISCOVER || job == JOB_DESCRIBE) {
        const char *what = job == JOB_DISCOVER ? "Looking for media servers" : "Connecting";
        ui_spinner(SCREEN_W / 2, 200);
        gfx_text((SCREEN_W - gfx_text_width(what, 1)) / 2, 234, COL_MUTED, 1, what);
    }
    else {
        for(int i = 0; i < count; i++) {
            float y = LIST_Y + i * (ROW_H + 6);
            if(i < server_count) {
                draw_row(y, ICON_SERVER, COL_LEMON, servers[i].name, COL_TEXT, NULL, i == server_sel);
            }
            else if(i == server_count) {
                draw_row(y, ICON_SEARCH, COL_MUTED, "Search again", COL_MUTED, NULL, i == server_sel);
            }
            else {
                draw_row(y, ICON_PLUS, COL_MUTED, "Enter an address", COL_MUTED, NULL, i == server_sel);
            }
        }
        if(!server_count) {
            const char *none = "No servers found. Is ZeroConfigDLNA running?";
            gfx_text(SAFE_X + 38, LIST_Y + count * (ROW_H + 6) + 16, COL_MUTED, 1, none);
        }
    }
    const char *status = link_status();
    gfx_text(SCREEN_W - SAFE_X - gfx_text_width(status, 0.75f), FOOTER_Y - 40, COL_MUTED, 0.75f, status);
    draw_messages();
    ui_footer();
    ui_hint('A', "Select");
    picture_hint();
}

static void draw_address(void) {
    const char *prompt = "Address and port of the DLNA server";
    gfx_text((SCREEN_W - gfx_text_width(prompt, 1)) / 2, 140, COL_MUTED, 1, prompt);
    /* Boxes for the four numbers and the port, centred as a group. */
    char field[5][8];
    float widths[5], total = 0;
    for(int i = 0; i < 5; i++) {
        if(i < 4) snprintf(field[i], sizeof(field[i]), "%d", address[i]);
        else snprintf(field[i], sizeof(field[i]), "%u", port);
        widths[i] = gfx_text_width(i < 4 ? "000" : "00000", 1.5f) + 20;
        total += widths[i] + (i < 4 ? 18 : 0);
    }
    float x = (SCREEN_W - total) / 2, y = 196;
    for(int i = 0; i < 5; i++) {
        int sel = i == address_field;
        if(sel) gfx_rect_alpha(x - 2, y - 2, widths[i] + 4, 56, COL_LEMON);
        gfx_rect_alpha(x, y, widths[i], 52, sel ? COL_SELECT : COL_PANEL);
        float tw = gfx_text_width(field[i], 1.5f);
        gfx_text(x + (widths[i] - tw) / 2, y + 8, sel ? COL_LEMON : COL_TEXT, 1.5f, field[i]);
        x += widths[i];
        if(i < 4) {
            gfx_text(x + 3, y + 8, COL_MUTED, 1.5f, i < 3 ? "." : ":");
            x += 18;
        }
    }
    const char *help = "Up/Down to change, L/R by ten";
    gfx_text((SCREEN_W - gfx_text_width(help, 1)) / 2, 280, COL_MUTED, 1, help);
    if(job == JOB_DESCRIBE) {
        ui_spinner(SCREEN_W / 2, 336);
    }
    draw_messages();
    ui_footer();
    ui_hint('A', "Connect");
    ui_hint('B', "Back");
}

static void draw_browse(void) {
    int sel = stack[depth].sel, top = stack[depth].top;
    if(job == JOB_BROWSE) {
        ui_spinner(SCREEN_W / 2, 220);
    }
    else if(!entry_count) {
        const char *empty = "Nothing to play in this folder";
        gfx_icon(ICON_FOLDER, (SCREEN_W - 48) / 2, 170, 48, COL_LEMON_DIM);
        gfx_text((SCREEN_W - gfx_text_width(empty, 1)) / 2, 236, COL_MUTED, 1, empty);
    }
    else {
        for(int r = 0; r < ROWS && top + r < entry_count; r++) {
            const entry_t *e = &entries[top + r];
            int icon = e->kind == ENTRY_FOLDER ? ICON_FOLDER : e->kind == ENTRY_AUDIO ? ICON_NOTE : ICON_FILM;
            uint32_t icon_col = e->kind == ENTRY_FOLDER ? COL_LEMON : e->kind == ENTRY_AUDIO ? 0xff8fd18au : 0xff7fbde8u;
            uint32_t col = COL_TEXT;
            if(e->kind != ENTRY_FOLDER && !playable(e)) {
                icon_col = 0xff5a5547;
                col = COL_MUTED;
            }
            char dur[16];
            format_duration(dur, sizeof(dur), e->duration);
            draw_row(LIST_Y + r * ROW_H, icon, icon_col, e->title, col, dur, top + r == sel);
        }
        if(entry_count > ROWS) {
            float h = ROWS * ROW_H - 6;
            float bar = h * ROWS / entry_count;
            if(bar < 16) bar = 16;
            gfx_rect_alpha(SCREEN_W - SAFE_X + 14, LIST_Y + (h - bar) * top / (entry_count - ROWS), 3, bar, COL_LEMON_DIM);
        }
    }
    draw_messages();
    ui_footer();
    ui_hint('A', "Open");
    ui_hint('B', "Back");
    ui_hint('Y', "Refresh");
    picture_hint();
}

static void draw(void) {
    gfx_begin();
    switch(screen) {
        case SCREEN_NETWORK:
            gfx_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            draw_network();
            break;
        case SCREEN_SERVERS:
            ui_header("Media servers");
            draw_servers();
            break;
        case SCREEN_ADDRESS:
            ui_header("Server address");
            draw_address();
            break;
        case SCREEN_BROWSE: {
            char path[200] = "";
            for(int i = 0; i <= depth; i++) {
                if(i == 1 && !strcmp(stack[1].id, "1") && !strcmp(stack[0].id, "0")) continue;
                size_t n = strlen(path);
                snprintf(path + n, sizeof(path) - n, "%s%s", n ? " / " : "", stack[i].title);
            }
            ui_header(path);
            draw_browse();
            break;
        }
    }
    gfx_end();
}

#ifdef LP_UI_TOUR
/* Screenshot tour of every screen. Each state is printed as "LP TOUR: name"
   and held for a few seconds so a script can capture the Flycast window. */
static void tour_step(void) {
    static int step;
    static uint64_t next;
    uint64_t ms = timer_ms_gettime64();
    if(job != JOB_NONE || ms < next) return;
    switch(step) {
        case 0:
            if(screen != SCREEN_SERVERS || !server_count) return;
            printf("LP TOUR: servers\n");
            break;
        case 1:
            screen = SCREEN_ADDRESS;
            address_field = 1;
            printf("LP TOUR: address\n");
            break;
        case 2: {
            int pick = 0;
#ifdef LP_SERVER
            for(int i = 0; i < server_count; i++) {
                if(!strcmp(servers[i].location, LP_SERVER)) pick = i;
            }
#endif
            open_server(&servers[pick]);
            step++;
            next = ms + 300;
            return;
        }
        case 3:
            if(screen != SCREEN_BROWSE || depth == 0) return;
            stack[depth].sel = entry_count > 3 ? 3 : 0;
            sel_since = ms;
            printf("LP TOUR: browse\n");
            break;
        case 4:
            say("Run the server with --dreamcast-mode to play this");
            printf("LP TOUR: toast\n");
            break;
        case 5:
            cycle_picture();
            printf("LP TOUR: picture-toast\n");
            break;
        case 6:
            settings.picture = PICTURE_FIT;
            message_until = 0;
            for(int i = 0; i < entry_count; i++) {
                if(playable(&entries[i])) {
                    printf("LP TOUR: playing\n");
                    play_from(i);
                    break;
                }
            }
            printf("LP TOUR: done\n");
            break;
        default:
            return;
    }
    step++;
    next = ms + 3000;
}
#endif

#if defined(LP_QA) || defined(LP_BENCH) || defined(LP_DECODE_BENCH)
/* Scripted runs open the first folders and play the first video. With
   LP_QA_LINGER (ms) they pause on each screen for screenshots. */
#ifndef LP_QA_LINGER
#define LP_QA_LINGER 0
#endif
/* LP_QA_END plays the first folder through from its first file. */
#ifdef LP_QA_END
#define QA_ANY_KIND 1
#else
#define QA_ANY_KIND 0
#endif
static void qa_step(void) {
    static int played;
    static uint64_t linger_until;
    static int linger_screen = -1, linger_depth = -1;
    if(job != JOB_NONE || played) return;
    if(LP_QA_LINGER && (screen != linger_screen || depth != linger_depth)) {
        linger_screen = screen;
        linger_depth = depth;
        linger_until = timer_ms_gettime64() + LP_QA_LINGER;
        printf("LP QA: screen %d depth %d\n", screen, depth);
    }
    if(timer_ms_gettime64() < linger_until) return;
    if(screen == SCREEN_SERVERS && server_count) {
        int pick = 0;
#ifdef LP_SERVER
        for(int i = 0; i < server_count; i++) {
            if(!strcmp(servers[i].location, LP_SERVER)) pick = i;
        }
#endif
        open_server(&servers[pick]);
        return;
    }
    if(screen != SCREEN_BROWSE) return;
    for(int i = 0; i < entry_count; i++) {
        if(playable(&entries[i]) && (entries[i].kind == ENTRY_VIDEO || QA_ANY_KIND)) {
            played = 1;
#ifdef LP_DECODE_BENCH
            bench_run(entries[i].url);
#elif defined(LP_BENCH)
            /* Server profiles to try, each played for LP_BENCH_SECONDS on the
               console from several points in the file. An empty query is
               the server's default. */
            static const struct {
                const char *query;
                double start;
            } runs[] = {
                {"", 300}, {"", 600}, {"", 900},
            };
            for(size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
                player_extra_query = runs[k].query;
                play_start = runs[k].start;
                play_from(i);
            }
#else
            play_from(i);
#endif
            printf("LP QA: done\n");
            return;
        }
    }
    for(int i = 0; i < entry_count; i++) {
        if(entries[i].kind == ENTRY_FOLDER && depth + 1 < MAX_DEPTH) {
            depth++;
            snprintf(stack[depth].id, sizeof(stack[depth].id), "%s", entries[i].id);
            stack[depth].sel = stack[depth].top = 0;
            browse(entries[i].id);
            return;
        }
    }
    played = 1;
    printf("LP QA: FAIL no playable video\n");
}
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("LemonPlay starting\n");
    gfx_init();
    snd_stream_init();
    settings_load();
    link_start();

    for(;;) {
        input_t in;
        input_poll(&in, screen == SCREEN_NETWORK || screen == SCREEN_SERVERS);
        if(in.reset == SOFTRESET_QUIT) break;
        if(in.reset == SOFTRESET_TITLE) go_home();
        update(&in);
        settings_poll();
#if defined(LP_QA) || defined(LP_BENCH) || defined(LP_DECODE_BENCH)
        qa_step();
#endif
#ifdef LP_UI_TOUR
        tour_step();
#endif
        draw();
    }
    return 0;
}
