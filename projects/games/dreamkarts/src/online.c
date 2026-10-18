/* DreamKarts online multiplayer: name entry, connecting, the lobby browser
   and the lobby room, plus the glue that turns a lobby race into karts.

   Each console drives only its own kart and sends it to the server; the
   others are drawn from the network about 110 ms in the past. The server
   decides the start time, the places and the points; this side just
   follows its lobby state: waiting (the lobby room), countdown, racing,
   results, then back to the room. */
#include "online.h"
#include "audio.h"
#include "fx.h"
#include "hud.h"
#include "kart.h"
#include "models.h"
#include "net.h"
#include "netlink.h"
#include "render.h"
#include "save.h"
#include "spectate.h"
#include "title.h"
#include "track.h"
#include "world.h"
#include <string.h>

enum { OS_NAME, OS_CONNECT, OS_BROWSER, OS_LOBBY, OS_ERROR };

#define INTERP_MS 110
#define DOWNLOAD_URL "GITHUB.COM/RICHSTOKES/DREAMCAST-HOMEBREW"

static int screen = OS_NAME;
static float screen_t;
static char name[NET_NAME_LEN] = "CHAO";
static int key_row, key_col;
static bool name_typed;        /* edited since entering; a saved name arriving late stays out */
static int browse_sel;
static int lobby_row;
static int my_colour = -1;
static int sel_track, sel_laps = 3, sel_cc = 1;
static bool was_leader;
static int raced_id = -1;      /* race id this console has set up */
static bool leave_prompt;
static bool watch_prompt;       /* lobby browser: "watch this lobby?" */
static char kart_names[MAX_KARTS][NET_NAME_LEN];
static const char *error_line1 = "", *error_line2 = "";
static float fly_t;
static int results_seen;       /* races this session that reached results */
static const pvr_poly_hdr_t *h_flat;

static const char *const key_rows[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-. "};
static const int laps_options[3] = {3, 5, 10};
static const char *const cc_names[] = {"50cc", "100cc", "150cc"};

void online_init(void) {
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
}

static void set_screen(int s) {
    screen = s;
    screen_t = 0;
    leave_prompt = false;
    watch_prompt = false;
    if(s != OS_LOBBY) spectate_set(false);
}

void online_enter(void) {
    game.online = true;
    game_set_state(ST_ONLINE);
    set_screen(OS_NAME);
    key_row = key_col = 0;
    name_typed = false;
    save_load_name();
}

static void back_to_menu(void) {
    net_end();
    game.online = false;
    game.laps = DEFAULT_LAPS;
    race_setup();
    game_set_state(ST_MAIN);
}

static void show_error(const char *l1, const char *l2) {
    error_line1 = l1;
    error_line2 = l2;
    set_screen(OS_ERROR);
    audio_sfx(DK_SFX_MENU_BACK, 200, 128);
}

/* ------------------------------------------------------------------ */
/* Race glue                                                           */

/* Line up the lobby's racers: our kart is the local player, everyone else
   is remote. No CPU karts online. */
static void setup_race(const NetLobby *l) {
    if(l->track != game.track) {
        game.track = l->track;
        track_init(game.track);
        world_build();
    }
    game.cc = l->cc;
    game.laps = l->laps;
    game.mode = MODE_GP;
    game.num_players = 1;
    int order[NET_MAX_PLAYERS], n = 0;
    for(int g = 0; g < NET_MAX_PLAYERS; g++)
        for(int i = 0; i < l->count; i++)
            if((l->players[i].flags & NPF_RACING) && l->players[i].grid == g) order[n++] = i;
    kart_count = n;
    for(int g = 0; g < n; g++) {
        const NetPlayer *p = &l->players[order[g]];
        Kart *k = &karts[g];
        memset(k, 0, sizeof(*k));
        k->remote = p->slot != l->you;
        k->net_slot = p->slot;
        kart_apply_colour(k, p->colour % COLOUR_COUNT);
        memcpy(kart_names[g], p->name, NET_NAME_LEN);
        k->name = kart_names[g];
        kart_place_on_grid(k, g);
        models_paint_kart(g);
        if(!k->remote) game.pl[0].kart = game.player = g;
    }
    spectate_set(net_spectating());
    if(net_spectating()) game.pl[0].kart = game.player = 0;   /* the director picks */
    race_reset();
    if(net_spectating()) spectate_begin();
    raced_id = l->race_id;
    audio_music_start();
    audio_music_target(150);
}

static float synced_countdown(const NetLobby *l) {
    int64_t to_green = (int64_t)l->green_ms - (int64_t)net_now();
    return 3.0f - (float)to_green * 0.001f;
}

float online_countdown(void) {
    return synced_countdown(net_lobby());
}

float online_race_time(void) {
    return (float)((int64_t)net_now() - (int64_t)net_lobby()->green_ms) * 0.001f;
}

void online_pack_kart(const Kart *me, NetKart *nk) {
    memset(nk, 0, sizeof(*nk));
    nk->flags = (me->finished ? NKF_FINISHED : 0) | (me->grounded ? NKF_GROUNDED : 0) |
                (me->boost_t > 0 ? NKF_BOOST : 0) | (me->drift == DRIFT_ACTIVE ? NKF_DRIFT : 0) |
                (me->drift == DRIFT_ACTIVE && me->drift_dir > 0 ? NKF_DRIFT_RIGHT : 0) |
                (me->wave_t > 0 ? NKF_WAVE : 0);
    nk->x = me->pos.x;
    nk->y = me->pos.y;
    nk->z = me->pos.z;
    nk->heading = me->heading;
    nk->speed = me->speed;
    nk->steer = me->steer;
    nk->stage = me->drift == DRIFT_ACTIVE ? me->stage : 0;
    nk->lap = me->lap;
    nk->dist = me->tp.s;
    nk->race_time = me->finished ? me->finish_time : fmaxf(game.race_time, 0);
}

void online_remote_karts(float dt) {
    static bool waving[MAX_KARTS];   /* NKF_WAVE last time, to start waves on its rising edge */
    uint64_t at = net_now() - INTERP_MS;
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(!k->remote) continue;
        NetKart nk;
        if(!net_remote_kart(k->net_slot, at, &nk)) continue;
        int dir = (nk.flags & NKF_DRIFT) ? ((nk.flags & NKF_DRIFT_RIGHT) ? 1 : -1) : 0;
        kart_remote_update(k, v3(nk.x, nk.y, nk.z), nk.heading, nk.speed, nk.steer, dir, nk.stage,
                           (nk.flags & NKF_BOOST) != 0, (nk.flags & NKF_GROUNDED) != 0, dt);
        /* A wave starts on the flag's rising edge and the arm stays up while
           the flag does (the sender can keep waving). */
        bool wave = (nk.flags & NKF_WAVE) != 0;
        if(wave && !waving[i]) {
            kart_wave(k);
#ifdef DK_AUTOTEST
            printf("DK ONLINE: slot %d waves\n", k->net_slot);
#endif
        }
        else if(wave) {
            k->wave_t = fmaxf(k->wave_t, 0.3f);
        }
        waving[i] = wave;
        k->lap = nk.lap;
        k->progress = (float)nk.lap * track.length + nk.dist;
        if((nk.flags & NKF_FINISHED) && !k->finished) {
            k->finished = true;
            k->finish_time = nk.race_time;
        }
    }
    /* Ours, for the next KART packet (a watcher has none). */
    if(net_spectating()) return;
    NetKart nk;
    online_pack_kart(&karts[game.player], &nk);
    net_set_kart(&nk, raced_id);
}

/* Follow the server through countdown, race and results. Returns true
   while it owns the game state. */
static void follow_lobby(void) {
    const NetLobby *l = net_lobby();
    const NetPlayer *me = net_me();
    bool racing = me && (me->flags & NPF_RACING);
    bool watching = net_spectating();
    if((racing || watching) && (l->state == NLS_COUNTDOWN || l->state == NLS_RACING) && raced_id != l->race_id) {
        setup_race(l);
        /* A watcher arriving mid-race goes straight to it. */
        game_set_state(watching && l->state == NLS_RACING ? ST_RACE : ST_COUNTDOWN);
        game.countdown = synced_countdown(l);
        return;
    }
    if(raced_id == l->race_id && l->state == NLS_RESULTS && game.state != ST_RESULTS &&
       (game.state == ST_RACE || game.state == ST_COUNTDOWN)) {
        game_set_state(ST_RESULTS);
        audio_music_target(130);
        results_seen++;
        return;
    }
    if(l->state == NLS_WAITING && (game.state == ST_RACE || game.state == ST_RESULTS || game.state == ST_COUNTDOWN)) {
        raced_id = -1;
        game_set_state(ST_ONLINE);
        set_screen(OS_LOBBY);
        fly_t = 0;
        audio_kart_loops(0, NULL, DK_DT);
    }
}

/* Called every frame from the main loop while game.online. */
void online_tick(void) {
    net_update();
    int ns = net_state();
    if(ns == NET_LOST || ns == NET_REJECTED) {
        if(screen != OS_ERROR || game.state != ST_ONLINE) {
            bool old = ns == NET_REJECTED && net_reject_reason() == NREJ_TOO_OLD;
            if(game.state != ST_ONLINE) game_set_state(ST_ONLINE);
            show_error(net_message(), old ? "PLEASE DOWNLOAD THE LATEST VERSION" : "");
        }
        return;
    }
    if(ns == NET_LOBBY) follow_lobby();
    else if(game.state != ST_ONLINE && ns == NET_BROWSE) {
        game_set_state(ST_ONLINE);   /* left mid-race */
        set_screen(OS_BROWSER);
    }
}

/* Mid-race Start: offer to leave (the race carries on behind the prompt). */
bool online_race_input(void) {
    if(!leave_prompt) {
        if(pad.pressed & BTN_START) {
            leave_prompt = true;
            audio_sfx(DK_SFX_MENU_SELECT, 180, 128);
        }
        return false;
    }
    if(pad.pressed & BTN_A) {
        leave_prompt = false;
        net_leave();
        raced_id = -1;
        game_set_state(ST_ONLINE);
        set_screen(OS_BROWSER);
        return true;
    }
    if(pad.pressed & (BTN_B | BTN_START)) {
        leave_prompt = false;
        audio_sfx(DK_SFX_MENU_BACK, 180, 128);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */

static int key_count(int row) {
    return row < 4 ? (int)strlen(key_rows[row]) : 2;
}

static void name_update(void) {
    char saved[NET_NAME_LEN];
    if(save_take_name(saved, sizeof(saved)) && !name_typed) strcpy(name, saved);
    int len = (int)strlen(name);
    if(input_menu_up()) key_row = (key_row + 4) % 5;
    if(input_menu_down()) key_row = (key_row + 1) % 5;
    if(input_menu_left()) key_col--;
    if(input_menu_right()) key_col++;
    int n = key_count(key_row);
    key_col = (key_col % n + n) % n;
    if(input_menu_up() || input_menu_down() || input_menu_left() || input_menu_right())
        audio_sfx(DK_SFX_MENU_MOVE, 160, 128);
    bool done = (pad.pressed & BTN_START) != 0;
    if(pad.pressed & BTN_A) {
        if(key_row < 4) {
            if(len < NET_NAME_LEN - 1) {
                name[len] = key_rows[key_row][key_col];
                name[len + 1] = '\0';
                name_typed = true;
                audio_sfx(DK_SFX_MENU_MOVE, 200, 128);
            }
        }
        else if(key_col == 0 && len > 0) {
            name[len - 1] = '\0';
            name_typed = true;
            audio_sfx(DK_SFX_MENU_BACK, 160, 128);
        }
        else if(key_col == 1) {
            done = true;
        }
    }
    if(pad.pressed & BTN_B) {
        if(len > 0) {
            name[len - 1] = '\0';
            name_typed = true;
            audio_sfx(DK_SFX_MENU_BACK, 160, 128);
        }
        else {
            audio_sfx(DK_SFX_MENU_BACK, 200, 128);
            back_to_menu();
            return;
        }
    }
    /* A name needs at least one letter or digit. */
    bool any = false;
    for(const char *c = name; *c; c++)
        if(*c != ' ' && *c != '-' && *c != '.') any = true;
    if(done && any) {
        audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
        save_store_name(name);
        net_begin(name);
        set_screen(OS_CONNECT);
    }
}

static void connect_update(void) {
    if(net_state() == NET_BROWSE) {
        set_screen(OS_BROWSER);
        audio_sfx(DK_SFX_CHAO_HAPPY, 200, 128);
        return;
    }
    if(pad.pressed & BTN_B) back_to_menu();
}

static void browser_update(void) {
    net_want_list(true);
    if(input_menu_up()) {
        browse_sel = (browse_sel + NET_LOBBIES - 1) % NET_LOBBIES;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        browse_sel = (browse_sel + 1) % NET_LOBBIES;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(net_state() == NET_LOBBY) {
        set_screen(OS_LOBBY);
        spectate_set(net_spectating());
        my_colour = -1;
        was_leader = false;
        lobby_row = 0;
        fly_t = 0;
        audio_sfx(DK_SFX_CHAO, 200, 128);
        return;
    }
    /* Y: watch the lobby instead (full or not), once confirmed with Y. */
    if(watch_prompt) {
        if((pad.pressed & BTN_Y) && net_state() == NET_BROWSE) {
            watch_prompt = false;
            net_spectate(browse_sel);
            audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
        }
        else if(pad.pressed & (BTN_B | BTN_A | BTN_START)) {
            watch_prompt = false;
            audio_sfx(DK_SFX_MENU_BACK, 180, 128);
        }
        return;
    }
    if((pad.pressed & BTN_Y) && net_state() == NET_BROWSE) {
        watch_prompt = true;
        audio_sfx(DK_SFX_MENU_MOVE, 220, 128);
        return;
    }
    if(pad.pressed & BTN_A && net_state() == NET_BROWSE) {
        if(net_lobbies()[browse_sel].players >= NET_MAX_PLAYERS || (net_lobbies()[browse_sel].flags & NLF_DEMO)) {
            audio_sfx(DK_SFX_MENU_BACK, 200, 128);
            watch_prompt = true;   /* full: offer to watch */
        }
        else {
            net_join(browse_sel);
            audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
        }
    }
    if(pad.pressed & BTN_B) {
        if(net_state() == NET_JOINING) net_leave();
        else back_to_menu();
    }
}

static bool colour_taken(const NetLobby *l, int c) {
    for(int i = 0; i < l->count; i++)
        if(l->players[i].slot != l->you && l->players[i].colour == c) return true;
    return false;
}

enum { LR_TRACK, LR_LAPS, LR_CC, LR_COLOUR, LR_START };

static int lobby_rows(bool leader, int *rows) {
    int n = 0;
    if(leader) {
        rows[n++] = LR_TRACK;
        rows[n++] = LR_LAPS;
        rows[n++] = LR_CC;
    }
    rows[n++] = LR_COLOUR;
    if(leader) rows[n++] = LR_START;
    return n;
}

static void lobby_update(void) {
    const NetLobby *l = net_lobby();
    const NetPlayer *me = net_me();
    if(net_state() == NET_BROWSE) {
        set_screen(OS_BROWSER);
        return;
    }
    if(net_spectating()) {
        /* Watching: the lobby's track behind the standings, until a race. */
        if(l->track != game.track && l->state == NLS_WAITING) {
            game_load_track(l->track);
            fly_t = 0;
        }
        if(pad.pressed & BTN_B) {
            audio_sfx(DK_SFX_MENU_BACK, 200, 128);
            net_leave();
            set_screen(OS_BROWSER);
        }
        return;
    }
    if(!me) return;
    bool leader = l->leader == l->you;
    if(leader && !was_leader) {
        /* Becoming leader: start editing from the lobby's settings. */
        sel_track = l->track;
        sel_laps = l->laps;
        sel_cc = l->cc;
        lobby_row = 0;
    }
    was_leader = leader;
    if(my_colour < 0) my_colour = me->colour;
    if(l->track != game.track && l->state == NLS_WAITING) {
        game_load_track(l->track);
        fly_t = 0;
    }
    int rows[6], n = lobby_rows(leader, rows);
    if(lobby_row >= n) lobby_row = n - 1;
    if(l->state == NLS_WAITING) {
        if(input_menu_up()) {
            lobby_row = (lobby_row + n - 1) % n;
            audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
        }
        if(input_menu_down()) {
            lobby_row = (lobby_row + 1) % n;
            audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
        }
        int dir = input_menu_left() ? -1 : input_menu_right() ? 1 : 0;
        int row = rows[lobby_row];
        if(dir) {
            if(row == LR_TRACK) sel_track = (sel_track + dir + TRACK_COUNT) % TRACK_COUNT;
            if(row == LR_LAPS) {
                int i = sel_laps == 10 ? 2 : sel_laps == 5 ? 1 : 0;
                sel_laps = laps_options[(i + dir + 3) % 3];
            }
            if(row == LR_CC) sel_cc = (sel_cc + dir + CC_COUNT) % CC_COUNT;
            if(row == LR_COLOUR) {
                int c = my_colour;
                for(int tries = 0; tries < COLOUR_COUNT; tries++) {
                    c = (c + dir + COLOUR_COUNT) % COLOUR_COUNT;
                    if(!colour_taken(l, c)) break;
                }
                my_colour = c;
                audio_chao(CHAO_HI, c, 150, 128);
            }
            if(row != LR_START) audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        }
        if((pad.pressed & (BTN_A | BTN_START)) && leader && (row == LR_START || (pad.pressed & BTN_START))) {
            if(l->count >= 2) {
                net_press_start();
                audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
            }
            else {
                audio_sfx(DK_SFX_MENU_BACK, 200, 128);
            }
        }
        else if((pad.pressed & BTN_A) && row != LR_START) {
            lobby_row = (lobby_row + 1) % n;
            audio_sfx(DK_SFX_MENU_MOVE, 200, 128);
        }
    }
    net_wish(my_colour, leader ? sel_track : l->track, leader ? sel_laps : l->laps, leader ? sel_cc : l->cc);
    if(pad.pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        net_leave();
        set_screen(OS_BROWSER);
    }
}

#ifdef DK_AT_NAME
/* VMU name test: report the name the card gave back (it should match
   DK_AT_NAME_WANT, saved by an earlier boot), then save DK_AT_NAME the way
   pressing Start does and give the worker time to write it. With
   DK_AT_NAME_CORRUPT it first overwrites the file with junk and checks a
   reload keeps the default. */
#include <dc/vmufs.h>
#define AT_STR_(x) #x
#define AT_STR(x) AT_STR_(x)
static void name_autotest(float dt) {
    static float t;
    static int step;
    static bool ok = true;
    t += dt;
    if(step == 0 && t > 3.0f) {
        printf("DK NAME: loaded %s\n", name);
#ifdef DK_AT_NAME_WANT
        ok = strcmp(name, AT_STR(DK_AT_NAME_WANT)) == 0;
#endif
#ifdef DK_AT_NAME_CORRUPT
        static uint8_t junk[1024];
        memset(junk, 0x5a, sizeof(junk));
        vmufs_write(maple_enum_type(0, MAPLE_FUNC_MEMCARD), "DREAMKARTS", junk, sizeof(junk), VMUFS_OVERWRITE);
        strcpy(name, "CHAO");
        save_load_name();
#endif
        step = 1;
    }
    if(step == 1 && t > 6.0f) {
#ifdef DK_AT_NAME_CORRUPT
        printf("DK NAME: after junk %s\n", name);
        ok = ok && strcmp(name, "CHAO") == 0;
#endif
        save_store_name(AT_STR(DK_AT_NAME));
        step = 2;
    }
    if(step == 2 && t > 11.0f) {
        printf("DK NAME: saved %s\nDK AUTOTEST: %s\n", AT_STR(DK_AT_NAME), ok ? "PASS" : "FAIL");
        step = 3;
    }
}
#endif

#ifdef DK_AT_SPECTATE
static uint32_t at_press;   /* buttons the spectate autotest presses next frame */
#endif

void online_update(float dt) {
#ifdef DK_AT_SPECTATE
    pad.pressed |= at_press;
    at_press = 0;
#endif
    screen_t += dt;
    fly_t += dt;
    switch(screen) {
        case OS_NAME: name_update(); break;
        case OS_CONNECT: connect_update(); break;
        case OS_BROWSER: browser_update(); break;
        case OS_LOBBY: lobby_update(); break;
        case OS_ERROR:
            if(screen_t > 0.5f && (pad.pressed & (BTN_A | BTN_B | BTN_START))) {
                audio_sfx(DK_SFX_MENU_BACK, 200, 128);
                back_to_menu();
            }
            break;
    }
#ifdef DK_AT_ONLINE
    online_autotest(dt);
#endif
#ifdef DK_AT_NAME
    name_autotest(dt);
#endif
}

void online_camera(Camera *cam, float dt) {
    (void)dt;
    /* A slow high orbit over the lobby's track. */
    float a = game.title_t * 0.06f;
    float in = clampf(fly_t * 0.8f, 0, 1);
    in = in * in * (3 - 2 * in);
    float r = lerpf(270.0f, 200.0f, in), h = lerpf(140.0f, 100.0f, in);
    cam->pos = v3(sinf(a) * r, h, cosf(a) * r * 0.8f);
    cam->fwd = vnorm(vsub(v3(0, 0, 0), cam->pos));
    cam->right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
    cam->up = vcross(cam->fwd, cam->right);
    cam->fov = 1.15f;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

#define PANEL_TOP argb(225, 30, 100, 220)
#define PANEL_BOT argb(225, 12, 36, 120)
#define GOLD   argb(255, 255, 210, 60)
#define SILVER argb(255, 216, 224, 234)
#define BRONZE argb(255, 216, 144, 74)

static void panel(float x0, float y0, float x1, float y1) {
    hud_panel(x0, y0, x1, y1, PANEL_TOP, PANEL_BOT, 15);
}

static void title_bar(const char *text) {
    hud_panel(150, 16, 490, 64, PANEL_TOP, PANEL_BOT, 15);
    hud_text(320, 20, 0.95f, ALIGN_CENTER, argb(255, 255, 220, 60), text);
}

static void hint(const char *text) {
    if(((int)(screen_t * 2.5f) & 1) == 0 || screen_t < 1.0f)
        hud_text(320, 446, 0.5f, ALIGN_CENTER, argb(230, 255, 255, 255), text);
}

/* A little cup: bowl, handles, stem and base, in gold, silver or bronze. */
static void trophy(float x, float y, float s, uint32_t col) {
    uint32_t dark = color_scale(col, 0.7f);
    r_hdr(h_flat);
    r_tri2d(x - 7 * s, y, x + 7 * s, y, x, y + 10 * s, col, col, dark, 16.8f);
    r_quad2d(x - 7 * s, y - 1 * s, x + 7 * s, y + 3 * s, 0, 0, 1, 1, col, 16.8f);
    r_quad2d(x - 10 * s, y + 1 * s, x - 7 * s, y + 5 * s, 0, 0, 1, 1, dark, 16.75f);
    r_quad2d(x + 7 * s, y + 1 * s, x + 10 * s, y + 5 * s, 0, 0, 1, 1, dark, 16.75f);
    r_quad2d(x - 1.5f * s, y + 8 * s, x + 1.5f * s, y + 13 * s, 0, 0, 1, 1, dark, 16.8f);
    r_quad2d(x - 5 * s, y + 13 * s, x + 5 * s, y + 16 * s, 0, 0, 1, 1, col, 16.8f);
}

static void swatch(float x, float y, int colour) {
    const KartColour *c = &kart_colours[colour % COLOUR_COUNT];
    r_hdr(h_flat);
    r_quad2d(x - 1, y - 1, x + 25, y + 15, 0, 0, 1, 1, 0xff0c1430, 16.4f);
    r_quad2d(x, y, x + 12, y + 14, 0, 0, 1, 1, (c->paint1 ? c->paint1 : 0xffe02828) | 0xff000000, 16.5f);
    r_quad2d(x + 12, y, x + 24, y + 14, 0, 0, 1, 1, c->ui | 0xff000000, 16.5f);
}

static uint32_t ping_colour(int ms) {
    return ms < 120 ? argb(255, 120, 255, 140) : ms < 250 ? argb(255, 255, 220, 80) : argb(255, 255, 110, 90);
}

static void draw_name(void) {
    title_bar("ONLINE");
    panel(100, 84, 540, 420);
    hud_text(320, 94, 0.7f, ALIGN_CENTER, argb(255, 200, 225, 255), "ENTER YOUR NAME");
    r_hdr(h_flat);
    r_quad2d(180, 128, 460, 168, 0, 0, 1, 1, argb(230, 8, 20, 60), 16.0f);
    char shown[NET_NAME_LEN + 2];
    snprintf(shown, sizeof(shown), "%s%s", name, ((int)(screen_t * 3) & 1) && strlen(name) < NET_NAME_LEN - 1 ? "_" : "");
    hud_text(320, 132, 0.95f, ALIGN_CENTER, 0xffffffff, shown);
    for(int r = 0; r < 5; r++) {
        int n = key_count(r);
        for(int c = 0; c < n; c++) {
            bool sel = r == key_row && c == key_col;
            float w = r < 4 ? 36 : 110, x = r < 4 ? 140 + c * 36 : 205 + c * 120, y = 186 + r * 40;
            r_hdr(h_flat);
            r_quad2d(x, y, x + w - 4, y + 34, 0, 0, 1, 1, sel ? argb(255, 255, 210, 60) : argb(200, 20, 50, 130), 16.1f);
            char label[8];
            if(r < 4) snprintf(label, sizeof(label), "%c", key_rows[r][c] == ' ' ? '_' : key_rows[r][c]);
            else snprintf(label, sizeof(label), "%s", c ? "DONE" : "DEL");
            hud_text(x + (w - 4) * 0.5f, y + 4, 0.62f, ALIGN_CENTER, sel ? argb(255, 20, 30, 80) : 0xffffffff, label);
        }
    }
    hint("A: TYPE   B: DELETE   START: DONE");
}

static void draw_connect(void) {
    title_bar("ONLINE");
    panel(110, 170, 530, 330);
    static const char *dots[] = {"", ".", "..", "..."};
    char buf[64];
    snprintf(buf, sizeof(buf), "CONNECTING%s", dots[(int)(screen_t * 3) & 3]);
    hud_text(320, 190, 0.95f, ALIGN_CENTER, argb(255, 255, 226, 70), buf);
    const char *what = net_state() == NET_HELLO ? "SAYING HELLO TO THE SERVER" : net_message();
    hud_text(320, 248, 0.55f, ALIGN_CENTER, 0xffffffff, what);
    if(link_is_modem())
        hud_text(320, 280, 0.45f, ALIGN_CENTER, argb(220, 200, 225, 255), "DIALING CAN TAKE UP TO A MINUTE");
    hint("B: CANCEL");
}

static void draw_browser(void) {
    title_bar("ONLINE LOBBIES");
    panel(40, 76, 600, 428);
    const NetLobbyInfo *ls = net_lobbies();
    hud_text(56, 82, 0.45f, ALIGN_LEFT, argb(220, 200, 225, 255), link_status());
    if(net_update_available())
        hud_text(584, 82, 0.45f, ALIGN_RIGHT, argb(255, 255, 220, 60), "NEW VERSION OUT!");
    for(int i = 0; i < NET_LOBBIES; i++) {
        const NetLobbyInfo *e = &ls[i];
        float y = 108 + i * 30;
        bool sel = i == browse_sel;
        if(sel) {
            r_hdr(h_flat);
            r_quad2d(52, y - 2, 588, y + 27, 0, 0, 1, 1, argb(150, 255, 255, 255), 15.5f);
        }
        uint32_t col = sel ? argb(255, 255, 255, 140) : 0xffffffff;
        char buf[48];
        snprintf(buf, sizeof(buf), "LOBBY %d", i + 1);
        hud_text(62, y, 0.58f, ALIGN_LEFT, col, buf);
        snprintf(buf, sizeof(buf), "%d/%d", e->players, NET_MAX_PLAYERS);
        hud_text(222, y, 0.58f, ALIGN_RIGHT, col, buf);
        const char *st = (e->flags & NLF_DEMO) ? "DEMO" : e->players == 0 ? "EMPTY" : e->players >= NET_MAX_PLAYERS ? "FULL"
                       : e->state == NLS_WAITING ? "OPEN" : "RACING";
        uint32_t stc = e->players == 0 ? argb(200, 170, 190, 220) : e->state == NLS_WAITING ? argb(255, 120, 255, 140)
                                                                                           : argb(255, 255, 190, 80);
        hud_text(240, y, 0.5f, ALIGN_LEFT, stc, st);
        if(e->players) {
            hud_text(340, y, 0.5f, ALIGN_LEFT, col, track_names[e->track % TRACK_COUNT]);
            hud_text(580, y, 0.5f, ALIGN_RIGHT, argb(220, 200, 225, 255), e->leader);
        }
    }
    if(net_state() == NET_JOINING) hint(net_spectating() ? "TUNING IN..." : "JOINING...");
    else if(net_message()[0] && !watch_prompt) hud_text(320, 446, 0.5f, ALIGN_CENTER, argb(255, 255, 160, 120), net_message());
    else hint("A: JOIN   Y: WATCH   B: BACK");
    const NetLobbyInfo *e = &ls[browse_sel];
    if(e->watchers && !watch_prompt) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d WATCHING", e->watchers);
        hud_text(584, 410, 0.42f, ALIGN_RIGHT, argb(220, 200, 225, 255), buf);
    }
    if(watch_prompt) {
        panel(130, 170, 510, 330);
        char buf[40];
        snprintf(buf, sizeof(buf), "WATCH LOBBY %d?", browse_sel + 1);
        hud_text(320, 186, 0.85f, ALIGN_CENTER, argb(255, 255, 220, 60), buf);
        hud_text(320, 232, 0.5f, ALIGN_CENTER, 0xffffffff, (e->flags & NLF_DEMO) ? "BOTS RACE HERE NONSTOP" : "YOU WILL SPECTATE, NOT RACE");
        hud_text(320, 258, 0.45f, ALIGN_CENTER, argb(220, 200, 225, 255), "THE CAMERAS FOLLOW EVERY RACE");
        hud_text(320, 292, 0.55f, ALIGN_CENTER, argb(255, 255, 255, 140), "Y: WATCH   B: CANCEL");
    }
}

/* Players ranked by points (then slot) for the lobby and results. */
static int ranked(const NetLobby *l, int *order) {
    int n = l->count;
    for(int i = 0; i < n; i++) order[i] = i;
    for(int i = 1; i < n; i++) {
        int v = order[i], j = i - 1;
        while(j >= 0) {
            const NetPlayer *a = &l->players[order[j]], *b = &l->players[v];
            if(b->points > a->points || (b->points == a->points && b->slot < a->slot)) {
                order[j + 1] = order[j];
                j--;
            }
            else break;
        }
        order[j + 1] = v;
    }
    return n;
}

static void draw_standings(const NetLobby *l, float x0, float y0, float x1) {
    int order[NET_MAX_PLAYERS], n = ranked(l, order);
    hud_text(x0 + 16, y0, 0.5f, ALIGN_LEFT, argb(220, 200, 225, 255), "PLAYER");
    hud_text(x1 - 96, y0, 0.5f, ALIGN_RIGHT, argb(220, 200, 225, 255), "PING");
    hud_text(x1 - 16, y0, 0.5f, ALIGN_RIGHT, argb(220, 200, 225, 255), "PTS");
    for(int r = 0; r < n; r++) {
        const NetPlayer *p = &l->players[order[r]];
        float y = y0 + 30 + r * 40;
        bool me = p->slot == l->you;
        if(me) {
            r_hdr(h_flat);
            r_quad2d(x0 + 8, y - 4, x1 - 8, y + 30, 0, 0, 1, 1, argb(90, 255, 255, 255), 15.5f);
        }
        if(p->points > 0 && r < 3) trophy(x0 + 26, y + 4, 1.1f, r == 0 ? GOLD : r == 1 ? SILVER : BRONZE);
        else {
            char rank[8];
            snprintf(rank, sizeof(rank), "%d", r + 1);
            hud_text(x0 + 26, y, 0.6f, ALIGN_CENTER, 0xffffffff, rank);
        }
        swatch(x0 + 46, y + 4, p->colour);
        bool lead = (p->flags & NPF_LEADER) != 0;
        hud_text(x0 + 80, lead ? y - 5 : y, 0.62f, ALIGN_LEFT, me ? argb(255, 255, 255, 140) : 0xffffffff, p->name);
        if(lead) hud_text(x0 + 82, y + 20, 0.36f, ALIGN_LEFT, argb(255, 255, 220, 60), "LOBBY LEADER");
        char buf[16];
        snprintf(buf, sizeof(buf), "%dms", p->ping);
        hud_text(x1 - 96, y + 3, 0.5f, ALIGN_RIGHT, ping_colour(p->ping), buf);
        snprintf(buf, sizeof(buf), "%d", p->points);
        hud_text(x1 - 16, y, 0.62f, ALIGN_RIGHT, argb(255, 255, 226, 70), buf);
    }
}

static void draw_lobby(void) {
    const NetLobby *l = net_lobby();
    char buf[64];
    snprintf(buf, sizeof(buf), "LOBBY %d", l->id + 1);
    title_bar(buf);
    panel(24, 76, 356, 428);
    draw_standings(l, 24, 86, 356);
    panel(372, 76, 616, 428);
    if(net_spectating()) {
        float y = 92;
        hud_text(494, y, 0.6f, ALIGN_CENTER, argb(255, 255, 220, 60), "SPECTATING");
        y += 40;
        static const char *const labels[] = {"TRACK", "LAPS", "SPEED"};
        char vb[24];
        for(int i = 0; i < 3; i++, y += 36) {
            hud_text(386, y, 0.5f, ALIGN_LEFT, argb(255, 190, 205, 235), labels[i]);
            const char *v = vb;
            if(i == 0) v = track_names[l->track % TRACK_COUNT];
            if(i == 1) snprintf(vb, sizeof(vb), "%d", l->laps);
            if(i == 2) v = cc_names[l->cc % CC_COUNT];
            hud_text(602, y + 2, i == 0 ? 0.42f : 0.5f, ALIGN_RIGHT, 0xffffffff, v);
        }
        snprintf(buf, sizeof(buf), "%d WATCHING", l->watchers);
        hud_text(494, y + 6, 0.5f, ALIGN_CENTER, argb(220, 200, 225, 255), buf);
        y = 340;
        if(l->flags & NLF_DEMO) hud_text(494, y, 0.42f, ALIGN_CENTER, argb(255, 120, 255, 140), "THE BOTS RACE NONSTOP");
        hud_text(494, y + 24, 0.45f, ALIGN_CENTER, l->state == NLS_WAITING ? 0xffffffff : argb(255, 255, 190, 80),
                 l->state == NLS_WAITING ? "NEXT RACE SOON" : l->state == NLS_RESULTS ? "RESULTS ARE IN" : "RACE IN PROGRESS");
        hint("B: STOP WATCHING");
        return;
    }
    bool leader = l->leader == l->you;
    int rows[6], n = lobby_rows(leader, rows);
    int show_track = leader ? sel_track : l->track, show_laps = leader ? sel_laps : l->laps;
    int show_cc = leader ? sel_cc : l->cc;
    const NetPlayer *me = net_me();
    float y = 92;
    hud_text(494, y, 0.55f, ALIGN_CENTER, argb(220, 200, 225, 255), leader ? "YOU ARE THE LEADER" : "RACE SETTINGS");
    y += 36;
    static const char *labels[] = {"TRACK", "LAPS", "SPEED", "COLOUR", "START RACE"};
    int shown_rows[5] = {LR_TRACK, LR_LAPS, LR_CC, LR_COLOUR, LR_START};
    for(int i = 0; i < 5; i++) {
        int row = shown_rows[i];
        if(row == LR_START && !leader) break;
        int sel_index = -1;
        for(int k = 0; k < n; k++)
            if(rows[k] == row) sel_index = k;
        bool sel = sel_index == lobby_row && sel_index >= 0 && l->state == NLS_WAITING;
        bool editable = sel_index >= 0;
        uint32_t col = sel ? argb(255, 255, 255, 140) : editable ? 0xffffffff : argb(255, 190, 205, 235);
        if(row == LR_START) {
            y += 8;
            const char *t = l->count >= 2 ? "START RACE" : "NEED 2 PLAYERS";
            float s = sel ? 0.8f + 0.05f * sinf(screen_t * 8) : 0.75f;
            hud_text(494, y, s, ALIGN_CENTER, l->count >= 2 ? col : argb(255, 255, 160, 120), t);
            break;
        }
        hud_text(386, y, 0.55f, ALIGN_LEFT, col, labels[i]);
        const char *v = "";
        char vb[24];
        if(row == LR_TRACK) v = track_names[show_track % TRACK_COUNT];
        if(row == LR_LAPS) { snprintf(vb, sizeof(vb), "%d", show_laps); v = vb; }
        if(row == LR_CC) v = cc_names[show_cc % CC_COUNT];
        if(row == LR_COLOUR) {
            int c = me ? me->colour : 0;
            swatch(560, y + 3, c);
            v = kart_colours[c % COLOUR_COUNT].name;
            hud_text(552, y + 2, 0.5f, ALIGN_RIGHT, col, v);
            v = "";
        }
        char shown[32];
        if(sel && row != LR_COLOUR && v[0]) {
            snprintf(shown, sizeof(shown), "< %s >", v);
            v = shown;
        }
        if(v[0]) hud_text(602, y + 2, row == LR_TRACK ? 0.45f : 0.5f, ALIGN_RIGHT, col, v);
        y += 40;
    }
    y = 360;
    if(l->state != NLS_WAITING) {
        hud_text(494, y, 0.5f, ALIGN_CENTER, argb(255, 255, 190, 80), "RACE IN PROGRESS");
        hud_text(494, y + 22, 0.42f, ALIGN_CENTER, 0xffffffff, "YOU WILL JOIN THE NEXT ONE");
    }
    else if(!leader) {
        const NetPlayer *ld = net_player(l->leader);
        snprintf(buf, sizeof(buf), "WAITING FOR %s", ld ? ld->name : "THE LEADER");
        hud_text(494, y, 0.48f, ALIGN_CENTER, 0xffffffff, buf);
        hud_text(494, y + 22, 0.45f, ALIGN_CENTER, argb(220, 200, 225, 255), "TO START THE RACE");
    }
    hint(leader ? "A: SELECT   LEFT/RIGHT: CHANGE   B: LEAVE" : "LEFT/RIGHT: COLOUR   B: LEAVE LOBBY");
}

static void draw_error(void) {
    title_bar("ONLINE");
    panel(30, 160, 610, 340);
    hud_text(320, 182, 0.56f, ALIGN_CENTER, argb(255, 255, 160, 120), error_line1);
    if(error_line2[0]) hud_text(320, 222, 0.55f, ALIGN_CENTER, 0xffffffff, error_line2);
    if(net_reject_reason() == NREJ_TOO_OLD) hud_text(320, 260, 0.5f, ALIGN_CENTER, argb(255, 255, 220, 60), DOWNLOAD_URL);
    hint("A: BACK");
}

void online_draw(void) {
    switch(screen) {
        case OS_NAME: draw_name(); break;
        case OS_CONNECT: draw_connect(); break;
        case OS_BROWSER: draw_browser(); break;
        case OS_LOBBY: draw_lobby(); break;
        case OS_ERROR: draw_error(); break;
    }
}

/* Over the race: the leave prompt, or who we are still waiting for. */
void online_draw_race(void) {
    const Kart *me = &karts[game.player];
    if(me->finished && game.state == ST_RACE && !net_spectating())
        hud_text(320, 300, 0.6f, ALIGN_CENTER, 0xffffffff, "WAITING FOR THE OTHERS...");
    if(leave_prompt) {
        panel(170, 180, 470, 300);
        hud_text(320, 196, 0.8f, ALIGN_CENTER, argb(255, 255, 220, 60), "LEAVE THE RACE?");
        hud_text(320, 250, 0.6f, ALIGN_CENTER, 0xffffffff, "A: LEAVE   B: KEEP RACING");
    }
}

void online_draw_results(void) {
    const NetLobby *l = net_lobby();
    float a = clampf(game.time / 0.5f, 0, 1);
    float x0 = 100 + (1 - a) * 440;
    hud_panel(x0, 60, x0 + 440, 420, PANEL_TOP, PANEL_BOT, 15);
    hud_text(x0 + 220, 70, 1.1f, ALIGN_CENTER, argb(255, 255, 220, 60), "RESULTS");
    hud_text(x0 + 300, 112, 0.45f, ALIGN_RIGHT, argb(220, 200, 225, 255), "RACE");
    hud_text(x0 + 420, 112, 0.45f, ALIGN_RIGHT, argb(220, 200, 225, 255), "TOTAL");
    static const int gained[NET_MAX_PLAYERS] = {10, 8, 6, 4, 2, 1};
    for(int place = 1; place <= NET_MAX_PLAYERS; place++) {
        for(int i = 0; i < l->count; i++) {
            const NetPlayer *p = &l->players[i];
            if(p->place != place || !(p->flags & NPF_RACING)) continue;
            float y = 136 + (place - 1) * 42;
            bool me = p->slot == l->you;
            if(me) {
                r_hdr(h_flat);
                r_quad2d(x0 + 12, y - 4, x0 + 428, y + 32, 0, 0, 1, 1, argb(80, 255, 255, 255), 15.5f);
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%s", place, hud_ordinal(place));
            hud_text(x0 + 24, y, 0.75f, ALIGN_LEFT, hud_place_color(place), buf);
            swatch(x0 + 90, y + 6, p->colour);
            hud_text(x0 + 122, y + 2, 0.65f, ALIGN_LEFT, me ? argb(255, 255, 255, 140) : 0xffffffff, p->name);
            snprintf(buf, sizeof(buf), "+%d", gained[place - 1]);
            hud_text(x0 + 300, y + 2, 0.65f, ALIGN_RIGHT, argb(255, 120, 255, 140), buf);
            snprintf(buf, sizeof(buf), "%d", p->points);
            hud_text(x0 + 420, y + 2, 0.65f, ALIGN_RIGHT, argb(255, 255, 226, 70), buf);
        }
    }
    hud_text(x0 + 220, 386, 0.5f, ALIGN_CENTER, 0xffffffff, "BACK TO THE LOBBY SOON");
}

const char *online_name(void) {
    return name;
}

#ifdef DK_AT_ONLINE
/* Online autotest: type a name, connect, join lobby DK_AT_LOBBY, start a
   race as leader once two are in, let the CPU drive, then report. */
#ifndef DK_AT_LOBBY
#ifdef DK_AT_SPECTATE
#define DK_AT_LOBBY 9   /* lobby 10: the server's demo lobby */
#else
#define DK_AT_LOBBY 8   /* lobby 9 (lobby 10 is the bots' demo) */
#endif
#endif
#if defined(DK_CAPTURE) && !defined(DK_HW)
#define AT_SHOT(flag, label) do { if(!(flag)) { flag = true; printf("DK SHOT %s\n", label); } } while(0)
#else
#define AT_SHOT(flag, label) do { (void)(flag); } while(0)
#endif
void online_autotest(float dt) {
    static float t;
    static bool reported, s_name, s_browser, s_lobby, s_after;
    t += dt;
    if(screen == OS_NAME && t > 0.6f) AT_SHOT(s_name, "online-name");
    if(screen == OS_NAME && t > 1.2f) {
        snprintf(name, sizeof(name), "AT");
        net_begin(name);
        set_screen(OS_CONNECT);
    }
    /* Test consoles are named after their session token, so two emulators
       started together still get different names. */
    if(screen == OS_CONNECT && net_state() == NET_HELLO && strcmp(name, "AT") == 0) {
        snprintf(name, sizeof(name), "AT%04u", (unsigned)(net_token() % 10000));
        net_rename(name);
        printf("DK ONLINE: connecting as %s\n", name);
    }
    if(screen == OS_BROWSER && screen_t > 1.0f) AT_SHOT(s_browser, "online-browser");
#ifdef DK_AT_SPECTATE
    /* Watch instead: Y on the lobby, then Y again to confirm. */
    static int y_presses;
    if(screen == OS_BROWSER && net_state() == NET_BROWSE && screen_t > 1.6f && y_presses < 2 &&
       screen_t > 1.6f + y_presses * 0.8f) {
        browse_sel = DK_AT_LOBBY;
        at_press = BTN_Y;
        if(y_presses++ == 0) AT_SHOT(s_lobby, "watch-prompt");
    }
    if(screen == OS_LOBBY && net_spectating()) {
        static bool s_watch;
        if(screen_t > 2.0f) AT_SHOT(s_watch, results_seen ? "watch-lobby-after" : "watch-lobby");
        if(results_seen > 0 && spectate_saw_grid() && !reported && screen_t > 2.5f) {
            printf("DK ONLINE: watched %d race(s), back in lobby %d with %d watching\n", results_seen, net_lobby()->id + 1,
                   net_lobby()->watchers);
            printf("DK AUTOTEST: PASS\n");
            reported = true;
        }
    }
    static bool s_err2;
    if(screen == OS_ERROR && screen_t > 1.0f) AT_SHOT(s_err2, "watch-error");
    if(screen == OS_ERROR && !reported && screen_t > 1.6f) {
        printf("DK ONLINE: error: %s\nDK AUTOTEST: FAIL\n", error_line1);
        reported = true;
    }
    return;
#endif
    if(screen == OS_BROWSER && net_state() == NET_BROWSE && screen_t > 1.6f) {
        browse_sel = DK_AT_LOBBY;
        net_join(DK_AT_LOBBY);
    }
    if(screen == OS_LOBBY) {
        const NetLobby *l = net_lobby();
        static float ready_t;
        ready_t = l->count >= 2 && l->leader == l->you && l->state == NLS_WAITING ? ready_t + dt : 0;
        if(l->count >= 2 && screen_t > 1.5f) AT_SHOT(s_lobby, "online-lobby");
#ifdef DK_AT_TRACK
        /* As leader, pick the race the way a player would. */
        if(l->leader == l->you) {
            sel_track = DK_AT_TRACK;
            sel_cc = 2;
        }
#endif
        if(results_seen > 0 && screen_t > 2.0f) AT_SHOT(s_after, "online-lobby-after");
        if(ready_t > 3.0f && results_seen == 0) {
            printf("DK ONLINE: starting a race with %d players\n", l->count);
            net_press_start();
            ready_t = -100;
        }
        if(results_seen > 0 && !reported && screen_t > 2.5f) {
            const NetPlayer *me = net_me();
            printf("DK ONLINE: race over, place %d, points %d, ping %d ms\n", me ? me->place : 0, me ? me->points : 0,
                   me ? me->ping : 0);
            printf("DK AUTOTEST: %s\n", me && me->place > 0 ? "PASS" : "FAIL");
            reported = true;
        }
    }
    static bool s_error;
    if(screen == OS_ERROR && screen_t > 1.0f) AT_SHOT(s_error, "online-error");
    if(screen == OS_ERROR && !reported && screen_t > 1.6f) {
        printf("DK ONLINE: error: %s\nDK AUTOTEST: FAIL\n", error_line1);
        reported = true;
    }
}
#endif
