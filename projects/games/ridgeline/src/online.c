/* Ridgeline online: up to four trucks on the mountain together.

   Ten lobbies of four. There is no race and no leader: joining a lobby
   puts the truck straight onto the mountain, and leaving takes it off;
   anyone can come and go at any time. Each console drives its own truck
   and sends it out; the others are drawn from the network a little in
   the past (net.c), their wheels set down on this console's ground. The
   lobby shares one time of day, kept by the server. Reaching the summit
   earns a trophy by the player's name for the rest of the session: in
   the roster, over their truck and on the lobby browser.

   The network link (Broadband adapter, W5500 or modem) is
   projects/common/netlink.c, shared with DreamKarts. */
#include "online.h"
#include "net.h"
#include "netlink.h"
#include "truck.h"
#include "camera.h"
#include "hud.h"
#include "audio.h"
#include "records.h"
#include "menu.h"
#include "fx.h"
#include "terrain.h"
#include "tod.h"
#include "engine/ui.h"
#include "sound_assets.h"
#ifdef RL_AT_ONLINE
#include "ai.h"
#include "engine/qa.h"
#endif

#ifdef RL_AT_ONLINE
#define AT_STR_(x) #x
#define AT_STR(x) AT_STR_(x)
#ifndef RL_AT_NAME
#define RL_AT_NAME AT-RL
#endif
#ifndef RL_AT_LOBBY
#define RL_AT_LOBBY 5           /* 1-10: the first five are Cascade Ridge's, where the test's bot drives */
#endif
#ifndef RL_AT_SUMMIT_AFTER
#define RL_AT_SUMMIT_AFTER 25   /* seconds on the mountain before jumping to the top */
#endif
#ifndef RL_AT_ONLINE_SECONDS
#define RL_AT_ONLINE_SECONDS 60
#endif
#ifndef RL_AT_EXPECT_TROPHIES
#define RL_AT_EXPECT_TROPHIES 1   /* players with a trophy in the lobby by the end */
#endif
#ifndef RL_AT_ROUTE
#define RL_AT_ROUTE 1
#endif
#ifndef RL_AT_SELECT_WAIT
#define RL_AT_SELECT_WAIT 8.0f  /* seconds on the truck select before joining */
#endif
#endif

enum { OS_NAME, OS_CONNECT, OS_BROWSER, OS_ERROR };

#define DOWNLOAD_URL "GITHUB.COM/RICHSTOKES/DREAMCAST-HOMEBREW"
#define TOAST_S 3.0f
/* From the end of a name's advance to its trophy (the cup is 1.2 s wide).
   The lettering's outline spills about 4 px past the advance. */
#define TROPHY_GAP 12.0f

static int screen = OS_NAME;
static float screen_t, fly_t;
static char name[NET_NAME_LEN] = "DRIVER";
static int key_row, key_col;
static int browse_sel;
static bool joining;
static bool summited;          /* reached the summit since going online */
static float spawn_t;          /* seconds since arriving on the mountain */
static const char *error_line1 = "", *error_line2 = "";
/* The roster last seen, to tell who came, went or reached the summit. */
static char seen_name[NET_MAX_PLAYERS][NET_NAME_LEN];
static uint8_t seen_flags[NET_MAX_PLAYERS];
static bool seen_here[NET_MAX_PLAYERS], seen_valid;
static char toast[48];
static float toast_t;

static const char *const key_rows[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-. "};

#ifdef RL_AT_ONLINE
static struct {
    float drive_t, last_log;
    bool jumped, reported, menu_open;
    bool unbuilt;     /* a remote truck drawn without its build (wheels at its centre) */
    uint32_t shots;   /* bit per screenshot taken (tools/capture.py) */
    float seen_m[NET_MAX_PLAYERS];   /* how far each slot's truck was seen to move */
    V3 seen_at[NET_MAX_PLAYERS];
    bool seen_any[NET_MAX_PLAYERS];
    int steps, pops, came, went, trophies_seen;
    float worst;
    V3 prev[MAX_TRUCKS];
    bool had[MAX_TRUCKS];
} at;
#endif

void online_init(void) {
}

static void set_screen(int s) {
    screen = s;
    screen_t = 0;
}

static void to_screen(int s) {
    if(game.state != ST_ONLINE) game_set_state(ST_ONLINE);
    set_screen(s);
}

void online_enter(void) {
    game.online = true;
    summited = false;
    joining = false;
    key_row = key_col = 0;
    records_load();   /* again, for a card put in since boot */
    if(records_name()[0]) snprintf(name, sizeof(name), "%s", records_name());
    to_screen(OS_NAME);
    netlink_set_server(RL_NET_HOST, RL_NET_FALLBACK_IP);
    netlink_prepare();   /* the adapter and DHCP while the name is typed */
}

void online_quit(void) {
    net_end();
    game.online = false;
    joining = false;
    audio_drive_stop();
    game.paused = false;
    game_set_state(ST_TITLE);
}

static void show_error(const char *l1, const char *l2) {
    error_line1 = l1;
    error_line2 = l2;
    if(game.state == ST_DRIVE) {
        audio_drive_stop();
        game.paused = false;
    }
    joining = false;
    to_screen(OS_ERROR);
    audio_sfx(RL_SFX_MENU_BACK, 200);
}

static void show_toast(const char *text) {
    snprintf(toast, sizeof(toast), "%s", text);
    toast_t = TOAST_S;
}

/* ------------------------------------------------------------------ */
/* Lobby glue                                                          */

/* The local truck index for a lobby slot: ours is trucks[0], the others
   fill 1-3 in slot order, so each keeps its index while it stays. */
static int truck_of(int slot) {
    int you = net_lobby()->you;
    return slot == you ? 0 : slot < you ? slot + 1 : slot;
}

static void park(Truck *t) {
    t->absent = true;
    t->horn = false;
    t->pos = v3(0, -1000, 0);
    t->vel = v3(0, 0, 0);
}

/* On the mountain: our truck at our slot's place on the start line, the
   other three slots remote (absent until their trucks arrive). */
static void drive_start(void) {
    const NetLobby *l = net_lobby();
    game_load_area(online_lobby_area(l->id));   /* (the truck select loaded it already) */
    /* No CPU trucks online, but keep the single player choice for later. */
    int company = game.companions;
    game.companions = 0;
    game_start_drive();
    game.companions = company;
    Truck *p = &trucks[0];
    const float *s = lvl_spawns[l->you % LVL_SPAWNS];
    truck_place(p, v3(s[0], 0, s[2]), s[3]);
    game.best_y = p->pos.y;
    camera_reset();
    game.ntrucks = MAX_TRUCKS;
    for(int i = 1; i < MAX_TRUCKS; i++) {
        Truck *t = &trucks[i];
        t->cpu = false;
        t->remote = true;
        t->lights = LIGHTS_OFF;
        park(t);
    }
    tod_set_hour(l->hour);
#ifdef RL_AT_ONLINE
    ai_start(0, RL_AT_ROUTE);
#endif
    char buf[24];
    snprintf(buf, sizeof(buf), "LOBBY %d", l->id + 1);
    hud_message_sub(buf);
    spawn_t = 0;
    joining = false;
    seen_valid = false;
    printf("RL ONLINE: on the mountain in lobby %d slot %d at %.2f h\n", l->id + 1, l->you, (double)l->hour);
}

/* Who came, went or reached the summit since the last lobby state. */
static void follow_roster(void) {
    const NetLobby *l = net_lobby();
    bool here[NET_MAX_PLAYERS] = {false};
    for(int i = 0; i < l->count; i++) {
        const NetPlayer *p = &l->players[i];
        int s = p->slot;
        here[s] = true;
        char buf[48];
        bool same = seen_here[s] && !strcmp(seen_name[s], p->name);
        if(seen_valid && s != l->you && !same) {
            snprintf(buf, sizeof(buf), "%.10s JOINED", p->name);
            show_toast(buf);
            audio_sfx(RL_SFX_CHIME, 150);
#ifdef RL_AT_ONLINE
            at.came++;
#endif
            printf("RL ONLINE: %s joined (slot %d)\n", p->name, s);
        }
        if(seen_valid && same && (p->flags & NPF_SUMMIT) && !(seen_flags[s] & NPF_SUMMIT) && s != l->you) {
            snprintf(buf, sizeof(buf), "%.10s REACHED THE SUMMIT", p->name);
            show_toast(buf);
            audio_sfx(RL_SFX_CHIME, 200);
            printf("RL ONLINE: %s reached the summit\n", p->name);
        }
        snprintf(seen_name[s], NET_NAME_LEN, "%s", p->name);
        seen_flags[s] = p->flags;
    }
    for(int s = 0; s < NET_MAX_PLAYERS; s++) {
        if(seen_valid && seen_here[s] && !here[s] && s != l->you) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.10s LEFT", seen_name[s]);
            show_toast(buf);
#ifdef RL_AT_ONLINE
            at.went++;
#endif
            printf("RL ONLINE: %s left (slot %d)\n", seen_name[s], s);
        }
        seen_here[s] = here[s];
    }
    seen_valid = true;
}

void online_tick(void) {
    net_update();
    if(toast_t > 0) toast_t -= 1.0f / 60.0f;
    int st = net_state();
    if(st == NET_LOST) {
        show_error("CONNECTION LOST", net_message());
        net_end();
        return;
    }
    if(st == NET_REJECTED) {
        if(net_reject_reason() == NREJ_TOO_OLD) show_error("PLEASE DOWNLOAD THE LATEST VERSION", DOWNLOAD_URL);
        else if(net_reject_reason() == NREJ_IDLE) show_error("YOU WERE IDLE", "AND LEFT THE LOBBY AFTER 10 MINUTES");
        else show_error(net_message(), "");
        net_end();
        return;
    }
    if(joining && game.state == ST_SELECT) {
        if(st == NET_LOBBY) drive_start();
        else if(st == NET_BROWSE) {
            /* Refused (full now): back to the list, saying why. */
            joining = false;
            to_screen(OS_BROWSER);
        }
        return;
    }
    if(game.state == ST_DRIVE && st == NET_LOBBY) follow_roster();
    /* The server restarted, or forgot us: back to the browser. */
    if(game.state == ST_DRIVE && st != NET_LOBBY) {
        audio_drive_stop();
        game.paused = false;
        to_screen(OS_BROWSER);
        show_toast("BACK TO THE LOBBIES");
    }
}

int online_lobby_pick(void) {
    return browse_sel;
}

void online_join(void) {
    if(joining || net_state() != NET_BROWSE) return;
    net_join(browse_sel, game.truck_model, game.livery);
    joining = true;
}

bool online_joining(void) {
    return joining;
}

void online_back_to_browser(void) {
    if(net_state() == NET_JOINING || net_state() == NET_LOBBY) net_leave();
    joining = false;
    to_screen(OS_BROWSER);
}

void online_leave_lobby(void) {
    net_leave();
    audio_drive_stop();
    game.paused = false;
    to_screen(OS_BROWSER);
}

void online_summit(void) {
    if(!summited) printf("RL ONLINE: summit reached, trophy claimed\n");
    summited = true;
}

/* Body axes to a quaternion (x, y, z, w). */
static void axes_quat(const V3 ax[3], float q[4]) {
    float m00 = ax[0].x, m01 = ax[1].x, m02 = ax[2].x;
    float m10 = ax[0].y, m11 = ax[1].y, m12 = ax[2].y;
    float m20 = ax[0].z, m21 = ax[1].z, m22 = ax[2].z;
    float tr = m00 + m11 + m22, s;
    if(tr > 0) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    }
    else if(m00 > m11 && m00 > m22) {
        s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    }
    else if(m11 > m22) {
        s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    }
    else {
        s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
}

static void quat_axes(const float q[4], V3 ax[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    ax[0] = v3(1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y));
    ax[1] = v3(2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x));
    ax[2] = v3(2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y));
}

static void pack_truck(const Truck *t, NetTruck *n) {
    n->flags = (uint8_t)((summited ? NTF_SUMMIT : 0) | ((t->lights & 3) << NTF_LIGHTS_SHIFT) | (t->horn ? NTF_HORN : 0) |
                         (t->brake > 0.2f ? NTF_BRAKE : 0) | (t->stalled ? NTF_STALLED : 0));
    n->pos = t->pos;
    n->vel = t->vel;
    axes_quat(t->ax, n->q);
    n->steer = 0.5f * (t->wh[WFL].steer + t->wh[WFR].steer);
    n->rpm = t->rpm;
    n->load = t->load;
    float slip = 0;
    for(int i = 0; i < 4; i++) slip = fmaxf(slip, t->wh[i].slip);
    n->slip = slip;
    n->gear = t->gear;
}

void online_drive_step(float dt) {
    const NetLobby *l = net_lobby();
    if(net_state() != NET_LOBBY) return;
    spawn_t += dt;
    uint64_t at_ms = net_draw_time(dt);
    for(int s = 0; s < NET_MAX_PLAYERS; s++) {
        if(s == l->you) continue;
        Truck *t = &trucks[truck_of(s)];
        const NetPlayer *p = net_player(s);
        if(!p) {
            if(!t->absent) {
                park(t);
                net_forget_slot(s);
            }
            continue;
        }
        /* Its build (wheels, ride height) on arrival and whenever the slot's
           truck changes. A slot never used yet is all zeros, model 0
           included, so comparing models alone left a Cascade 84 arriving
           there with every wheel at its centre and the body floating. */
        if(t->absent || t->model != p->model) {
            t->model = p->model;
            truck_spec(&t->spec, t->model);
        }
        t->livery = p->livery;
        NetTruck n;
        if(!net_remote_truck(s, at_ms, &n)) continue;
        V3 ax[3];
        quat_axes(n.q, ax);
        if(t->absent) {
            /* A fresh arrival: the wheels start at rest on the ground. */
            t->absent = false;
            for(int i = 0; i < 4; i++) t->wh[i].comp = t->wh[i].comp_prev = t->spec.sag * t->spec.travel;
        }
        truck_set_pose(t, n.pos, ax, n.vel, n.steer, dt);
        t->lights = (n.flags >> NTF_LIGHTS_SHIFT) & 3;
        t->horn = (n.flags & NTF_HORN) != 0;
        t->brake = (n.flags & NTF_BRAKE) ? 1.0f : 0.0f;
        t->stalled = (n.flags & NTF_STALLED) != 0;
        t->rpm = n.rpm;
        t->load = n.load;
        t->gear = n.gear;
        for(int i = 0; i < 4; i++) t->wh[i].slip = n.slip;
    }
    /* Arriving on top of a truck parked on our start place: move along. */
    Truck *me = &trucks[0];
    if(spawn_t < 2.0f)
        for(int i = 1; i < MAX_TRUCKS; i++)
            if(!trucks[i].absent && truck_overlap(me, &trucks[i]) > 0.05f) {
                truck_recover_at(me, me->pos);
                camera_reset();
                break;
            }
    NetTruck n;
    pack_truck(me, &n);
    net_set_truck(&n);
    /* Keep the lobby's time of day (it runs at the same pace here, so
       this only catches drift and the arrival). */
    float d = l->hour - tod.hour;
    if(d > 12) d -= 24;
    if(d < -12) d += 24;
    if(fabsf(d) > 0.1f) tod_set_hour(l->hour);
#ifdef RL_AT_ONLINE
    /* Remote motion: how far each step's move strays from the truck's own
       velocity (a pop or a stall). */
    for(int i = 1; i < MAX_TRUCKS; i++) {
        const Truck *t = &trucks[i];
        if(t->absent) {
            at.had[i] = false;
            continue;
        }
        if(at.had[i]) {
            V3 want = vmad(at.prev[i], t->vel, dt);
            float e = vlen(vsub(t->pos, want));
            at.steps++;
            if(e > 0.25f && e < 4.0f) at.pops++;
            if(e < 4.0f && e > at.worst) at.worst = e;
        }
        at.prev[i] = t->pos;
        at.had[i] = true;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Screens                                                             */

static int key_count(int row) {
    return row < 4 ? (int)strlen(key_rows[row]) : 2;
}

static void name_update(void) {
    int len = (int)strlen(name);
    /* A Dreamcast keyboard types the name too; Enter is Start. */
    const uint8_t *keys;
    int nk = input_keys(&keys);
    bool done = false;
    for(int i = 0; i < nk; i++) {
        uint8_t k = keys[i];
        if(k == KEY_ENTER) done = true;
        else if(k == KEY_BACKSPACE && len > 0) {
            name[--len] = '\0';
            audio_sfx(RL_SFX_MENU_BACK, 160);
        }
        else if(((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ' || k == '-' || k == '.') &&
                len < NET_NAME_LEN - 1) {
            name[len++] = (char)k;
            name[len] = '\0';
            audio_sfx(RL_SFX_MENU_MOVE, 200);
        }
    }
    bool moved = input_menu_up() || input_menu_down() || input_menu_left() || input_menu_right();
    if(input_menu_up()) key_row = (key_row + 4) % 5;
    if(input_menu_down()) key_row = (key_row + 1) % 5;
    if(input_menu_left()) key_col--;
    if(input_menu_right()) key_col++;
    int n = key_count(key_row);
    key_col = (key_col % n + n) % n;
    if(moved) audio_sfx(RL_SFX_MENU_MOVE, 160);
    if(pad.pressed & BTN_START) done = true;
    if(pad.pressed & BTN_A) {
        if(key_row < 4) {
            if(len < NET_NAME_LEN - 1) {
                name[len] = key_rows[key_row][key_col];
                name[len + 1] = '\0';
                audio_sfx(RL_SFX_MENU_MOVE, 200);
            }
        }
        else if(key_col == 0 && len > 0) {
            name[len - 1] = '\0';
            audio_sfx(RL_SFX_MENU_BACK, 160);
        }
        else if(key_col == 1) done = true;
    }
    if(pad.pressed & BTN_B) {
        if(len > 0) {
            name[len - 1] = '\0';
            audio_sfx(RL_SFX_MENU_BACK, 160);
        }
        else {
            audio_sfx(RL_SFX_MENU_BACK, 200);
            online_quit();
            return;
        }
    }
    /* A name needs a letter or digit, and no spaces either end. */
    bool any = false;
    for(const char *c = name; *c; c++)
        if(*c != ' ' && *c != '-' && *c != '.') any = true;
    if(done && any) {
        while(len > 0 && name[len - 1] == ' ') name[--len] = '\0';
        audio_sfx(RL_SFX_MENU_SELECT, 230);
        records_set_name(name);
        net_begin(name);
        set_screen(OS_CONNECT);
    }
}

/* From the browser to the truck select for the lobby picked. */
static void to_truck_select(void) {
    net_want_list(false);
    game_load_area(online_lobby_area(browse_sel));
    game_set_state(ST_SELECT);
    /* The truck parked in that lobby's light (a fresh one starts in the morning). */
    const NetLobbyInfo *e = &net_lobbies()[browse_sel];
    tod_set_hour(e->count ? e->hour : 9.0f);
}

static void browser_update(void) {
    net_want_list(true);
    if(input_menu_up()) {
        browse_sel = (browse_sel + NET_LOBBIES - 1) % NET_LOBBIES;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    if(input_menu_down()) {
        browse_sel = (browse_sel + 1) % NET_LOBBIES;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    if((pad.pressed & (BTN_A | BTN_START)) && net_state() == NET_BROWSE) {
        if(net_lobbies()[browse_sel].count >= NET_MAX_PLAYERS) audio_sfx(RL_SFX_MENU_BACK, 200);
        else {
            audio_sfx(RL_SFX_MENU_SELECT, 220);
            to_truck_select();
        }
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(RL_SFX_MENU_BACK, 200);
        online_quit();
    }
}

void online_update(float dt) {
    screen_t += dt;
    fly_t += dt;
    camera_flyover(fly_t);
    switch(screen) {
        case OS_NAME: name_update(); break;
        case OS_CONNECT:
            if(net_state() == NET_BROWSE) {
                set_screen(OS_BROWSER);
                audio_sfx(RL_SFX_CHIME, 200);
            }
            else if(pad.pressed & BTN_B) {
                audio_sfx(RL_SFX_MENU_BACK, 200);
                online_quit();
            }
            break;
        case OS_BROWSER: browser_update(); break;
        case OS_ERROR:
            if(pad.pressed & BTN_A) {
                /* Try again, with the same name. */
                audio_sfx(RL_SFX_MENU_SELECT, 200);
                net_begin(name);
                set_screen(OS_CONNECT);
            }
            else if(pad.pressed & BTN_B) {
                audio_sfx(RL_SFX_MENU_BACK, 200);
                online_quit();
            }
            break;
    }
}

#define PALE ui_col(UI_DIM)
#define TEXT ui_col(UI_TEXT)
#define GOLD ui_col(UI_ACCENT)

static void draw_toast(void) {
    if(toast_t <= 0) return;
    float a = fminf((TOAST_S - toast_t) / 0.2f, fminf(toast_t / 0.4f, 1.0f));
    float w = text_width(hud_font(), toast, 0.5f) + 36;
    float y = game.state == ST_DRIVE ? 40 : 6;   /* under the FPS counter */
    ui_pill(320 - w * 0.5f, y, 320 + w * 0.5f, y + 28, color_alpha(0xff343c44, a), color_alpha(0xff182028, a));
    hud_text_c(320, y + 14, 0.5f, ALIGN_CENTER, color_alpha(GOLD, a), toast);
}

static void draw_name(void) {
    ui_heading(320, 40, "ONLINE");
    ui_panel(100, 84, 540, 420);
    hud_text_c(320, 108, 0.62f, ALIGN_CENTER, PALE, "ENTER YOUR NAME");
    ui_well(180, 128, 460, 168);
    char shown[NET_NAME_LEN + 2];
    snprintf(shown, sizeof(shown), "%s%s", name, ((int)(screen_t * 3) & 1) && strlen(name) < NET_NAME_LEN - 1 ? "_" : "");
    hud_text_c(320, 148, 0.95f, ALIGN_CENTER, 0xffffffff, shown);
    for(int r = 0; r < 5; r++) {
        int n = key_count(r);
        for(int c = 0; c < n; c++) {
            bool sel = r == key_row && c == key_col;
            float w = r < 4 ? 36 : 110, x = r < 4 ? 140 + c * 36 : 205 + c * 120, y = 186 + r * 40;
            ui_key(x, y, x + w - 4, y + 34, sel);
            char label[8];
            if(r < 4) snprintf(label, sizeof(label), "%c", key_rows[r][c] == ' ' ? '_' : key_rows[r][c]);
            else snprintf(label, sizeof(label), "%s", c ? "DONE" : "DEL");
            hud_text_c(x + (w - 4) * 0.5f, y + 17, 0.62f, ALIGN_CENTER, ui_col(sel ? UI_ITEM_SEL : UI_ITEM), label);
        }
    }
    ui_hint("A: TYPE   B: DELETE   START: DONE");
}

static void draw_connect(void) {
    ui_heading(320, 40, "ONLINE");
    ui_panel(110, 170, 530, 330);
    static const char *dots[] = {"", ".", "..", "..."};
    char buf[48];
    snprintf(buf, sizeof(buf), "CONNECTING%s", dots[(int)(screen_t * 3) & 3]);
    hud_text_c(320, 206, 0.95f, ALIGN_CENTER, GOLD, buf);
    const char *what = net_state() == NET_HELLO ? "SAYING HELLO TO THE SERVER" : net_message();
    hud_text_c(320, 258, 0.55f, ALIGN_CENTER, TEXT, what);
    if(netlink_is_modem()) hud_text_c(320, 290, 0.45f, ALIGN_CENTER, PALE, "DIALING CAN TAKE UP TO A MINUTE");
    ui_hint("B: CANCEL");
}

static void draw_browser(void) {
    ui_heading(320, 40, "ONLINE LOBBIES");
    ui_panel(30, 72, 610, 432);
    hud_text_c(596, 86, 0.45f, ALIGN_RIGHT, PALE, netlink_status());
    if(net_update_available()) hud_text_c(320, 86, 0.45f, ALIGN_CENTER, GOLD, "NEW VERSION OUT!");
    const NetLobbyInfo *ls = net_lobbies();
    for(int i = 0; i < NET_LOBBIES; i++) {
        const NetLobbyInfo *e = &ls[i];
        /* Each area's lobbies under its name. */
        int group = i / (NET_LOBBIES / 2);
        float y = 114 + i * 29 + group * 18, cy = y + 12;
        if(i % (NET_LOBBIES / 2) == 0)
            hud_text_c(44, y - 9, 0.42f, ALIGN_LEFT, GOLD, rl_levels[online_lobby_area(i)]->name);
        bool sel = i == browse_sel;
        ui_row(38, y - 1, 602, y + 25, sel);
        uint32_t col = sel ? ui_col(UI_ITEM_SEL) : TEXT;
        char buf[24];
        snprintf(buf, sizeof(buf), "LOBBY %d", i + 1);
        hud_text_c(52, cy, 0.56f, ALIGN_LEFT, col, buf);
        snprintf(buf, sizeof(buf), "%d/%d", e->count, NET_MAX_PLAYERS);
        hud_text_c(196, cy, 0.56f, ALIGN_RIGHT, e->count >= NET_MAX_PLAYERS ? ui_col(UI_WARN) : col, buf);
        if(!e->count) {
            hud_text_c(222, cy, 0.48f, ALIGN_LEFT, sel ? col : PALE, "EMPTY");
            continue;
        }
        tod_format(e->hour, buf, sizeof(buf));
        hud_text_c(222, cy, 0.48f, ALIGN_LEFT, sel ? col : PALE, buf);
        /* Who is there, a trophy by each who has been to the top. */
        float x = 286;
        for(int k = 0; k < e->count; k++) {
            float w = text_width(hud_font(), e->names[k], 0.46f);
            if(x + w > 596) break;
            hud_text_c(x, cy, 0.46f, ALIGN_LEFT, col, e->names[k]);
            x += w;
            if(e->flags[k] & NPF_SUMMIT) {
                hud_trophy(x + TROPHY_GAP + 0.6f * 14, cy, 14, 1.0f);
                x += TROPHY_GAP + 1.2f * 14;
            }
            x += 14;
        }
    }
    if(net_message()[0]) hud_text_c(320, 452, 0.5f, ALIGN_CENTER, ui_col(UI_WARN), net_message());
    else ui_hint("A: JOIN   B: BACK");
}

static void draw_error(void) {
    ui_heading(320, 40, "ONLINE");
    ui_panel(70, 170, 570, 320);
    hud_text_c(320, 214, 0.7f, ALIGN_CENTER, ui_col(UI_WARN), error_line1);
    hud_text_c(320, 262, 0.5f, ALIGN_CENTER, TEXT, error_line2);
    ui_hint("A: TRY AGAIN   B: BACK");
}

void online_draw(void) {
    switch(screen) {
        case OS_NAME: draw_name(); break;
        case OS_CONNECT: draw_connect(); break;
        case OS_BROWSER: draw_browser(); break;
        case OS_ERROR: draw_error(); break;
    }
    draw_toast();
}

static uint32_t ping_col(int ms) {
    return ms < 120 ? 0xff90f0b0 : ms < 250 ? 0xffffd070 : 0xffff9050;
}

void online_draw_hud(void) {
    if(game.photo) return;
    draw_toast();
    if(game.hud_hidden || net_state() != NET_LOBBY) return;
    const NetLobby *l = net_lobby();
    /* Name tags over the others' trucks, fading with distance. */
    for(int i = 0; i < l->count; i++) {
        const NetPlayer *p = &l->players[i];
        if(p->slot == l->you) continue;
        const Truck *t = &trucks[truck_of(p->slot)];
        if(t->absent) continue;
        float dist = vlen(vsub(t->pos, r_cam.pos));
        float a = clampf((220.0f - dist) / 60.0f, 0, 1);
        float sx, sy, iw;
        if(a <= 0 || !r_project(vmad(t->pos, v3(0, 1, 0), 2.3f), &sx, &sy, &iw)) continue;
        if(sx < -40 || sx > 680 || sy < -20 || sy > 500) continue;
        float sc = clampf(0.62f - dist * 0.002f, 0.38f, 0.62f);
        float w = text_width(hud_font(), p->name, sc);
        hud_text_c(sx, sy, sc, ALIGN_CENTER, color_alpha(0xfff8f0dc, a), p->name);
        if(p->flags & NPF_SUMMIT) hud_trophy(sx + w * 0.5f + TROPHY_GAP + 0.6f * 30 * sc, sy, 30 * sc, a);
    }
    /* The roster, on the right under the corner map (clear of the
       messages across the middle, and of the large map). */
    float x0 = 420, x1 = 622, y0 = game.map_mode == 0 ? 152 : 20, h = 18 + l->count * 20;
    hud_panel(x0, y0, x1, y0 + h, 0x90182028);
    char buf[24];
    snprintf(buf, sizeof(buf), "LOBBY %d", l->id + 1);
    hud_text_c(x0 + 10, y0 + 10, 0.42f, ALIGN_LEFT, 0xffb8b0a0, buf);
    for(int i = 0; i < l->count; i++) {
        const NetPlayer *p = &l->players[i];
        float cy = y0 + 27 + i * 20;
        bool me = p->slot == l->you;
        /* A swatch in the colour of their arrow on the map. */
        hud_panel(x0 + 9, cy - 4, x0 + 17, cy + 4, hud_map_color(truck_of(p->slot)));
        hud_text_c(x0 + 23, cy, 0.5f, ALIGN_LEFT, me ? 0xffffd070 : 0xfff4ecd8, p->name);
        if(p->flags & NPF_SUMMIT) hud_trophy(x0 + 23 + text_width(hud_font(), p->name, 0.5f) + TROPHY_GAP + 0.6f * 15, cy, 15, 1.0f);
        if(!me) {
            snprintf(buf, sizeof(buf), "%d MS", p->ping);
            hud_text_c(x1 - 10, cy, 0.4f, ALIGN_RIGHT, ping_col(p->ping), buf);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Autotest (-DRL_AT_ONLINE): go online, join a lobby, let the CPU drive,
   jump to the summit for the trophy, and check the others' trucks came
   through. Point it at a local server with -DRL_NET_TEST_HOST=<Mac IP>
   (server/README.md has the recipes). */

#ifdef RL_AT_ONLINE
/* Each screenshot once, when its moment comes. */
static void at_shot(int i, bool when, const char *label) {
    if(!when || (at.shots & (1u << i))) return;
    at.shots |= 1u << i;
    qa_shot("%s", label);
}

void online_autotest_boot(void) {
    online_enter();
    snprintf(name, sizeof(name), "%s", AT_STR(RL_AT_NAME));
    browse_sel = RL_AT_LOBBY - 1;
    printf("RL ONLINE: autotest as %s, lobby %d, server %s:%d\n", name, RL_AT_LOBBY, RL_NET_HOST, RL_NET_PORT);
}

void online_autotest(float dt) {
    if(at.reported) return;
    /* The name screen for a moment, then on as if Start was pressed. */
    if(game.state == ST_ONLINE && screen == OS_NAME) {
        at_shot(0, screen_t > 0.8f, "online-name");
        if(screen_t > 1.6f) {
            net_begin(name);
            set_screen(OS_CONNECT);
        }
        return;
    }
    at_shot(1, game.state == ST_ONLINE && screen == OS_CONNECT && screen_t > 0.3f, "online-connect");
    if(game.state == ST_ONLINE && screen == OS_BROWSER && net_state() == NET_BROWSE) {
        at_shot(2, screen_t > 1.0f, "online-browser");
        if(screen_t > 1.8f) to_truck_select();
        return;
    }
    if(game.state == ST_SELECT && !joining) {
        at_shot(3, game.time > 0.6f, "online-select");
        /* Long enough on the truck select that a server ignoring the
           console there would lose it (the connection times out at 6 s). */
        if(game.time > RL_AT_SELECT_WAIT) online_join();
        return;
    }
    if(game.state == ST_ONLINE && screen == OS_ERROR) {
        printf("RL AUTOTEST: FAIL online: %s %s\n", error_line1, error_line2);
        at.reported = true;
        return;
    }
    if(game.state == ST_SELECT && !joining && game.time > 0.5f) {
        online_join();
        return;
    }
    if(game.state != ST_DRIVE) return;
    at.drive_t += dt;
    /* Pictures: the others' trucks from the start line, the menu over the
       running mountain, then the trophies at the top. */
    at_shot(4, at.drive_t > 1.2f, "online-start");
    at_shot(5, at.drive_t > 6.0f, "online-drive");
    if(!at.menu_open && at.drive_t > 15.0f && at.drive_t < 16.0f) {
        game.paused = at.menu_open = true;
        game.pause_sel = 0;
    }
    at_shot(6, at.drive_t > 15.7f, "online-menu");
    if(at.menu_open && at.drive_t > 16.5f) game.paused = false;
    at_shot(7, at.drive_t > RL_AT_SUMMIT_AFTER + 5.0f, "online-summit");
    const NetLobby *l = net_lobby();
    for(int i = 0; i < l->count; i++) {
        int s = l->players[i].slot;
        if(s == l->you) continue;
        const Truck *t = &trucks[truck_of(s)];
        if(t->absent) continue;
        float track = vlen(vsub(t->wh[WFR].centre, t->wh[WFL].centre)), base = vlen(vsub(t->wh[WFL].centre, t->wh[WRL].centre));
        if(t->spec.radius <= 0 || track < 1.0f || base < 1.5f) {
            if(!at.unbuilt)
                printf("RL ONLINE: slot %d truck has no build (radius %.2f, track %.2f, wheelbase %.2f m)\n", s,
                       (double)t->spec.radius, (double)track, (double)base);
            at.unbuilt = true;
        }
        if(at.seen_any[s]) at.seen_m[s] += vlen(vsub(t->pos, at.seen_at[s]));
        at.seen_at[s] = t->pos;
        at.seen_any[s] = true;
    }
    if(!at.jumped && at.drive_t > RL_AT_SUMMIT_AFTER) {
        at.jumped = true;
        truck_recover_at(&trucks[0], v3(LEVEL_SUMMIT_X - 6, LEVEL_SUMMIT_Y, LEVEL_SUMMIT_Z - 6));
        camera_reset();
        printf("RL ONLINE: jumping to the summit\n");
    }
    int trophies = 0;
    bool mine = false;
    for(int i = 0; i < l->count; i++)
        if(l->players[i].flags & NPF_SUMMIT) {
            trophies++;
            if(l->players[i].slot == l->you) mine = true;
        }
    float moved = 0;
    int remotes = 0;
    for(int s = 0; s < NET_MAX_PLAYERS; s++)
        if(at.seen_any[s]) {
            remotes++;
            moved = fmaxf(moved, at.seen_m[s]);
        }
    if(at.drive_t - at.last_log >= 10.0f) {
        at.last_log = at.drive_t;
        char who[80] = "";
        for(int i = 0; i < l->count; i++) {
            char e[20];
            snprintf(e, sizeof(e), "%s%s%s", i ? ", " : "", l->players[i].name, (l->players[i].flags & NPF_SUMMIT) ? "*" : "");
            strncat(who, e, sizeof(who) - strlen(who) - 1);
        }
        printf("RL ONLINE: t=%.0f lobby %d at %.2f h (tod %.2f): %s; %d remote trucks seen, furthest moved %.0f m, "
               "interp %d ms, motion %d steps %d pops worst %.2f m\n",
               (double)at.drive_t, l->id + 1, (double)l->hour, (double)tod.hour, who, remotes, (double)moved,
               net_interp_ms(), at.steps, at.pops, (double)at.worst);
    }
    if(at.drive_t >= RL_AT_ONLINE_SECONDS) {
        at.reported = true;
        const char *fail = NULL;
        if(at.unbuilt) fail = "a remote truck was drawn without its build";
        else if(!mine) fail = "the server never showed our trophy";
        else if(trophies < RL_AT_EXPECT_TROPHIES) fail = "fewer trophies in the lobby than expected";
#ifndef RL_AT_ONLINE_ALONE
        else if(remotes == 0 || moved < 10.0f) fail = "no remote truck seen driving";
#endif
        if(fail)
            printf("RL AUTOTEST: FAIL online: %s (%d trophies, %d remotes, moved %.0f m)\n", fail, trophies, remotes,
                   (double)moved);
        else
            printf("RL AUTOTEST: PASS online: %d players, %d trophies, %d remotes seen moving %.0f m, joins %d leaves %d, "
                   "motion %d steps %d pops worst %.2f m\n",
                   l->count, trophies, remotes, (double)moved, at.came, at.went, at.steps, at.pops, (double)at.worst);
    }
}
#endif
