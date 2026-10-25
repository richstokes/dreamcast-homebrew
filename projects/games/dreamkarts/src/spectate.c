/* DreamKarts spectating: a TV director for an online race this console is
   only watching.

   Trackside cameras are placed from the track itself when a race is set
   up: one on the outside of every bend (two on long ones), one at the end
   of every long straight looking back down it, and one high over the
   start line. Each covers the stretch of road up to its handover point,
   so a TV shot cuts from camera to camera as the kart it follows comes
   past, zooming to keep the kart a steady size.

   In AUTO the director changes shot every few seconds: trackside TV, the
   chase camera, a helicopter circling behind, a camera on the nose
   looking back at the pack, or a blimp high overhead. It mostly follows
   the leader, often the closest fight for a place, and goes to the start
   line camera for every finish. The watcher can pick the camera (X or the
   triggers) and the kart (left/right), and hide the overlay (Y) for a
   clean picture. */
#include "spectate.h"
#include "hud.h"
#include "kart.h"
#include "net.h"
#include "render.h"
#include "track.h"
#include "world.h"
#include <stdlib.h>

#define MAX_TV 24
#define GAP_STEP 4.0f       /* metres per entry in the leader's time table */
#define GAP_MAX 4096
#define LINE_HAND 10.0f    /* the start line camera hands over just past it */
#define ROCK_SEG 8.0f      /* metres of road per bit of TvCam.hidden */

typedef struct {
    V3 pos;
    float s, hand;          /* lap distance it stands at, and hands over at */
    uint32_t hidden;        /* pieces of its stretch behind scenery (sight_view) */
} TvCam;

static bool on;
static TvCam tv[MAX_TV];
static int ntv;
static int mode_pick = CAM_AUTO, target_pick = -1;
static int shot_mode = CAM_TV, shot_target, tv_cur = -1, tv_next = -1;
static float shot_t, shot_len, tv_next_t;
static bool cut;            /* the next camera frame jumps instead of easing */
static bool hud_hidden;
static float hint_t, toast_t, banner_t, spec_t;
static char toast[40], banner[40];
static bool was_finished[MAX_KARTS];
static float lead_t[GAP_MAX];   /* race time the leader reached each GAP_STEP */
static int lead_n;
static float map_cx, map_cy, map_sc;
static V3 look_s;
static float fov_s, heli_a;
static const pvr_poly_hdr_t *h_flat;

static const char *const mode_names[CAM_MODES] = {"AUTO", "TV", "CHASE", "HELICOPTER", "FRONT", "BLIMP"};

void spectate_set(bool o) {
    on = o;
    if(!o) target_pick = -1;
}

bool spectate_on(void) {
    return on;
}

int spectate_target(void) {
    return shot_target;
}

int spectate_shot_mode(void) {
    return shot_mode;
}

int spectate_tv_count(void) {
    return ntv;
}

static float rangef(float lo, float hi) {
    return lo + (hi - lo) * frand();
}

/* Positive distance along the lap from a forward to b, in [0, length). */
static float ahead(float a, float b) {
    return track_wrap(b - a);
}

static const TrackSample *sample_at(float s) {
    int i = (int)(track_wrap(s) / track.ds) % TRACK_SAMPLES;
    return &track.smp[i];
}

/* ------------------------------------------------------------------ */
/* Trackside cameras                                                   */

static void add_cam(float s, float side, float extra, float h, float hand) {
    if(ntv >= MAX_TV) return;
    /* Not beside a gate's posts: step back from it. */
    for(int g = 0; g < world_gates; g++)
        if(fabsf(track_delta(world_gate_s[g], s)) < 14.0f) s = world_gate_s[g] - 14.0f;
    s = track_wrap(s);
    const TrackSample *t = sample_at(s);
    float wall = side > 0 ? t->wall_out : t->wall_in;
    if(wall > 24.0f) wall = t->half + t->curb + 6.0f;   /* an open edge in the sky */
    float d = side * (wall + extra);
    V3 p = track_point(s, d);
    float road = track_road_y(s, clampf(d, -t->half, t->half));
    p.y = fmaxf(road, world_ground_y(p.x, p.z)) + h;
    tv[ntv++] = (TvCam){p, s, track_wrap(hand), 0};
}

/* How much of the road a camera covers (len metres up to from + len) it
   can see past solid scenery (world_solid: Spiral Canyon's rock,
   Tsudoken's ramp and props), nothing if it stands in it, and which
   ROCK_SEG metre pieces of it are hidden (a bit each). */
static float sight_view(V3 p, float from, float len, uint32_t *hidden) {
    *hidden = 0xffffffffu;
    if(world_solid(p, 1.5f)) return 0;
    *hidden = 0;
    int n = 0, seen = 0;
    for(float u = ROCK_SEG * 0.5f; u < len && n < 32; u += ROCK_SEG, n++) {
        float s = track_wrap(from + u);
        V3 q = track_point(s, 0);
        q.y = track_road_y(s, 0) + 1.0f;
        if(world_sight_clear(p, q)) seen++;
        else *hidden |= 1u << n;
    }
    return n ? (float)seen / n : 1.0f;
}

/* A camera the scenery hides the road from moves to wherever nearby sees
   the most of it: up, in to the road's edge (or a tunnel's ceiling),
   across to the other side, or along the road. What none can see (a
   tunnel's depths) the director films from the chase camera instead. */
static void clear_view(TvCam *c, float from, float len) {
    if(track.id != TRACK_CANYON && track.id != TRACK_TSUDOKEN) return;
    float best = sight_view(c->pos, from, len, &c->hidden);
    if(best > 0.999f) return;
    V3 o = track_point(c->s, 0), r = vsub(track_point(c->s, 1), o);
    float d0 = (c->pos.x - o.x) * r.x + (c->pos.z - o.z) * r.z;
    float rise0 = c->pos.y - track_road_y(c->s, clampf(d0, -sample_at(c->s)->half, sample_at(c->s)->half));
    static const float shift[] = {0, -12, 12}, rise[] = {0, 4, 9, 15};
    for(int a = 0; a < 3 && best < 0.999f; a++) {
        float s = track_wrap(c->s + shift[a]);
        const TrackSample *t = sample_at(s);
        float out[3] = {fabsf(d0), t->half + t->curb + 2.0f, t->half - 1.0f};
        for(int side = 0; side < 2 && best < 0.999f; side++)
            for(int w = 0; w < 3 && best < 0.999f; w++)
                for(int h = 0; h < 4 && best < 0.999f; h++) {
                    float d = (side ? -1.0f : 1.0f) * (d0 < 0 ? -out[w] : out[w]);
                    V3 p = track_point(s, d);
                    p.y = track_road_y(s, clampf(d, -t->half, t->half)) + (w == 2 ? 3.5f : rise0) + rise[h];
                    uint32_t hid;
                    float v = sight_view(p, from, len, &hid);
                    if(v > best + 0.01f) {
                        best = v;
                        c->pos = p;
                        c->s = s;
                        c->hidden = hid;
                    }
                }
    }
#ifdef DK_AUTOTEST
    printf("DK SPECTATE: camera moved clear of the scenery to s %.0f (%.0f, %.0f, %.0f), sees %.0f%%\n", (double)c->s,
           (double)c->pos.x, (double)c->pos.y, (double)c->pos.z, (double)(best * 100));
#endif
}

/* True where scenery hides lap distance s from camera i. */
static bool tv_hidden(int i, float s) {
    int j = (int)(ahead(tv[(i + ntv - 1) % ntv].hand, s) / ROCK_SEG);
    return j < 32 && (tv[i].hidden >> j & 1);
}

static int by_hand(const void *a, const void *b) {
    float x = ((const TvCam *)a)->hand, y = ((const TvCam *)b)->hand;
    return x < y ? -1 : x > y;
}

static void place_cameras(void) {
    /* Once per track: the scenery checks take ~0.4 s. */
    static int placed_for = -1;
    if(ntv > 0 && placed_for == track.id) return;
    placed_for = track.id;
    ntv = 0;
    const float L = track.length, step = 2.0f, bend = 1.0f / 110.0f;
    /* Start the scan on a straight so no bend wraps past it. */
    float s0 = 0;
    for(float s = 0; s < L; s += step)
        if(fabsf(track_curvature(s)) < bend * 0.5f) {
            s0 = s;
            break;
        }
    float b[MAX_TV], e[MAX_TV], sign[MAX_TV];
    int nb = 0;
    bool in = false;
    for(float u = 0; u <= L; u += step) {
        float s = s0 + u, c = track_curvature(s), sg = c < 0 ? 1.0f : -1.0f;  /* outside of a left is the right */
        bool now_in = fabsf(c) >= bend && u < L;
        /* A bend ends where it straightens or reverses (esses are two). */
        if(in && (!now_in || sg != sign[nb])) {
            e[nb] = s;
            if(e[nb] - b[nb] > 8.0f) nb++;
            in = false;
        }
        if(now_in && !in && nb < MAX_TV) {
            b[nb] = s;
            sign[nb] = sg;
            in = true;
        }
    }
    for(int i = 0; i < nb; i++) {
        float len = e[i] - b[i];
        if(len < 150.0f) {
            add_cam(b[i] + len * 0.5f, sign[i], 7.0f, 5.0f, e[i] + 12.0f);
        }
        else {
            add_cam(b[i] + len * 0.3f, sign[i], 7.0f, 5.5f, b[i] + len * 0.55f);
            add_cam(b[i] + len * 0.78f, sign[i], 7.0f, 5.0f, e[i] + 12.0f);
        }
        /* The straight after this bend, filmed from its far end. */
        float next_b = i + 1 < nb ? b[i + 1] : b[0] + L, gap = next_b - e[i];
        float next_side = i + 1 < nb ? sign[i + 1] : sign[0];
        if(gap > 90.0f) add_cam(next_b - 4.0f, next_side, 5.0f, 3.5f, next_b + 6.0f);
        if(gap > 260.0f) add_cam(e[i] + gap * 0.5f, -next_side, 5.0f, 4.0f, e[i] + gap * 0.5f + 6.0f);
    }
    /* Beside the run to the flag, watching the karts go under the gantry. */
    add_cam(-22.0f, -1.0f, 6.0f, 6.0f, LINE_HAND);
    qsort(tv, ntv, sizeof(tv[0]), by_hand);
    /* Drop cameras that would only hold a kart for a moment. */
    for(int i = 0; i < ntv && ntv > 3;) {
        int p = (i + ntv - 1) % ntv;
        if(ahead(tv[p].hand, tv[i].hand) < 28.0f && fabsf(tv[i].hand - LINE_HAND) > 0.01f) {
            memmove(&tv[i], &tv[i + 1], sizeof(tv[0]) * (ntv - i - 1));
            ntv--;
        }
        else {
            i++;
        }
    }
    /* Above the highest road each one films (ramps, climbs), so a slope
       never fills the picture. */
    for(int i = 0; i < ntv; i++) {
        float from = tv[(i + ntv - 1) % ntv].hand, len = ahead(from, tv[i].hand), top = -1e9f;
        for(float u = 0; u <= len; u += 4.0f) top = fmaxf(top, track_road_y(track_wrap(from + u), 0));
        tv[i].pos.y = fmaxf(tv[i].pos.y, top + 3.0f);
        clear_view(&tv[i], from, len);
    }
#ifdef DK_AUTOTEST
    printf("DK SPECTATE: %s: %d trackside cameras\n", track.name, ntv);
#endif
}

/* The camera covering lap distance s: the first one handing over ahead. */
static int tv_for(float s) {
    int best = 0;
    float bd = 1e9f;
    for(int i = 0; i < ntv; i++) {
        float d = ahead(s, tv[i].hand);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* The director                                                        */

static int placed(int place) {   /* kart index in place (1-based) */
    if(place < 1) place = 1;
    if(place > kart_count) place = kart_count;
    return game.finish_order[place - 1];
}

/* The kart worth watching in a battle: the chaser in the closest fight. */
static int battle(void) {
    int best = -1;
    float bg = 14.0f;
    for(int p = 1; p < kart_count; p++) {
        const Kart *a = &karts[placed(p)], *b = &karts[placed(p + 1)];
        if(a->finished || b->finished) continue;
        float g = a->progress - b->progress;
        if(g < bg) {
            bg = g;
            best = placed(p + 1);
        }
    }
    return best;
}

/* The highest placed kart still racing (the winner once all are home). */
static int front_runner(void) {
    for(int p = 1; p <= kart_count; p++)
        if(!karts[placed(p)].finished) return placed(p);
    return placed(1);
}

static void set_target(int k) {
    if(k < 0 || k >= kart_count) k = 0;
    if(k != shot_target) cut = true;
    shot_target = k;
    game.player = game.pl[0].kart = k;
}

static void new_shot(void) {
    static const int weights[CAM_MODES] = {0, 40, 18, 17, 13, 12};
    int mode;
    do {
        int r = (int)(dk_rand() % 100), acc = 0;
        for(mode = CAM_TV; mode < CAM_MODES - 1; mode++) {
            acc += weights[mode];
            if(r < acc) break;
        }
    } while(mode == shot_mode && mode != CAM_TV);
    int pick = (int)(dk_rand() % 100), b = battle();
    int k = front_runner();
    if(pick >= 55 && pick < 90 && b >= 0) k = b;
    else if(pick >= 90) k = placed(1 + (int)(dk_rand() % kart_count));
    static const float lo[CAM_MODES] = {0, 10, 6, 7, 5, 5}, hi[CAM_MODES] = {0, 16, 9, 10, 7, 7};
    shot_mode = mode;
    shot_len = rangef(lo[mode], hi[mode]);
    shot_t = 0;
    cut = true;
    set_target(k);
}

void spectate_begin(void) {
    if(!h_flat) h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    place_cameras();
    memset(was_finished, 0, sizeof(was_finished));
    for(int i = 0; i < GAP_MAX; i++) lead_t[i] = -1;
    lead_n = 0;
    shot_mode = CAM_TV;
    shot_t = 0;
    shot_len = rangef(7.0f, 9.0f);   /* the start, from the side of the road */
    tv_cur = tv_next = -1;
    hint_t = 7.0f;
    banner_t = toast_t = 0;
    cut = true;
    heli_a = 0;
    set_target(placed(1));
    /* The map fits the top-right corner. */
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        V3 p = track.smp[i].p;
        x0 = fminf(x0, p.x);
        x1 = fmaxf(x1, p.x);
        z0 = fminf(z0, p.z);
        z1 = fmaxf(z1, p.z);
    }
    map_sc = fminf(150.0f / (x1 - x0 + 24.0f), 120.0f / (z1 - z0 + 24.0f));
    map_cx = 548.0f - (x0 + x1) * 0.5f * map_sc;
    map_cy = 92.0f + (z0 + z1) * 0.5f * map_sc;
}

static void show_toast(const char *text) {
    snprintf(toast, sizeof(toast), "%s", text);
    toast_t = 1.6f;
    hint_t = fmaxf(hint_t, 4.0f);
}

#ifdef DK_AT_SPECTATE
static bool saw_grid;
static uint64_t cost_upd, cost_draw, cost_upd_max, cost_draw_max;   /* us */
static int cost_frames, cost_steps;

bool spectate_saw_grid(void) {
    return saw_grid;
}

/* Spectate autotest: step through every camera with X during the race,
   with a screenshot of each, and log the director's shots. */
static void autotest(float dt) {
    static float t, next = 8.0f;
    static int last_mode = -1, presses;
    static bool s_grid, s_results;
    t += dt;
    if(game.state == ST_COUNTDOWN && game.countdown > 1.5f && !s_grid) {
        s_grid = saw_grid = true;
        printf("DK SHOT watch-grid\n");
    }
    if(game.state == ST_RESULTS && game.time > 1.5f && !s_results) {
        s_results = true;
        printf("DK SHOT watch-results\n");
    }
    if(game.state == ST_RACE && game.race_time > next && presses < CAM_MODES) {
        next = game.race_time + 7.0f;
        pad.pressed |= BTN_X;
        presses++;
    }
    if(shot_mode != last_mode && game.state == ST_RACE) {
        last_mode = shot_mode;
        printf("DK SPECTATE: %s on %s (tv %d)\n", mode_names[shot_mode], karts[shot_target].name, tv_cur);
    }
    static float shot_at[CAM_MODES];
    if(game.state == ST_RACE && shot_t > 2.5f && shot_at[shot_mode] == 0 && game.race_time > 6.0f) {
        shot_at[shot_mode] = game.race_time;
        printf("DK SHOT watch-%s\n", mode_names[shot_mode]);
    }
}
#endif

static void input(void) {
    int dir = (pad.pressed & (BTN_X | BTN_R)) ? 1 : (pad.pressed & BTN_L) ? -1 : 0;
    if(dir) {
        mode_pick = (mode_pick + dir + CAM_MODES) % CAM_MODES;
        char buf[40];
        snprintf(buf, sizeof(buf), "CAMERA: %s", mode_names[mode_pick]);
        show_toast(buf);
        shot_t = shot_len;   /* change now */
    }
    int kdir = input_menu_right() ? 1 : input_menu_left() ? -1 : 0;
    if(kdir) {
        /* Step through the field by place: AUTO, 1st, 2nd ... last, AUTO. */
        int place = target_pick < 0 ? 0 : karts[target_pick].place;
        place += kdir;
        if(place < 0) place = kart_count;
        target_pick = place == 0 || place > kart_count ? -1 : placed(place);
        char buf[40];
        if(target_pick < 0) snprintf(buf, sizeof(buf), "FOLLOWING: AUTO");
        else snprintf(buf, sizeof(buf), "FOLLOWING: %s", karts[target_pick].name ? karts[target_pick].name : "?");
        show_toast(buf);
        if(target_pick >= 0) set_target(target_pick);
        else shot_t = shot_len;
    }
    if(pad.pressed & BTN_Y) {
        hud_hidden = !hud_hidden;
        show_toast(hud_hidden ? "OVERLAY OFF" : "OVERLAY ON");
    }
}

static void update(float dt);

void spectate_update(float dt) {
    if(!on || kart_count == 0) return;
#ifdef DK_AT_SPECTATE
    autotest(dt);   /* its serial logging stays out of the timing */
    uint64_t t0 = timer_us_gettime64();
    update(dt);
    uint64_t us = timer_us_gettime64() - t0;
    cost_upd += us;
    if(us > cost_upd_max) cost_upd_max = us;
    cost_steps++;
#else
    update(dt);
#endif
}

static void update(float dt) {
    spec_t += dt;
    if(hint_t > 0) hint_t -= dt;
    if(toast_t > 0) toast_t -= dt;
    if(banner_t > 0) banner_t -= dt;
    input();
    bool racing = game.state == ST_RACE;
    /* The leader's clock at every few metres, for the gaps. */
    if(racing) {
        float lead = karts[placed(1)].progress;
        while(lead_n < GAP_MAX && lead_n * GAP_STEP <= lead) lead_t[lead_n++] = game.race_time;
    }
    /* Finishes: a banner for the winner, a word for the rest. */
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->finished && !was_finished[i] && game.state != ST_COUNTDOWN) {
            char buf[40];
            if(k->place == 1) {
                snprintf(banner, sizeof(banner), "%s WINS!", k->name ? k->name : "?");
                banner_t = 4.0f;
            }
            else {
                snprintf(buf, sizeof(buf), "%s %d%s", k->name ? k->name : "?", k->place, hud_ordinal(k->place));
                show_toast(buf);
            }
        }
        was_finished[i] = k->finished;
    }
    if(game.state == ST_RESULTS) {
        set_target(placed(1));
        return;
    }
    shot_t += dt;
    if(target_pick >= 0) set_target(target_pick);
    /* A finish is always seen from the line. */
    int fr = front_runner();
    const Kart *f = &karts[fr];
    float to_line = (float)(game.laps + 1) * track.length - f->progress;
    bool finale = racing && !f->finished && f->lap >= game.laps && to_line < 130.0f;
    static float finale_hold;
    if(finale) finale_hold = 2.5f;
    else if(finale_hold > 0) finale_hold -= dt;
    if(mode_pick == CAM_AUTO && target_pick < 0 && (finale || finale_hold > 0)) {
        if(finale && (shot_mode != CAM_TV || shot_target != fr)) {
            shot_mode = CAM_TV;
            cut = true;
            set_target(fr);
        }
        shot_t = 0;
        shot_len = 3.0f;
        return;
    }
    if(game.state == ST_COUNTDOWN) return;
    if(shot_t >= shot_len) {
        if(mode_pick == CAM_AUTO) {
            new_shot();
        }
        else {
            shot_mode = mode_pick;
            shot_t = 0;
            shot_len = rangef(8.0f, 12.0f);
            cut = true;
        }
        if(target_pick >= 0) set_target(target_pick);
        else if(mode_pick != CAM_AUTO) set_target((dk_rand() % 3) ? front_runner() : (battle() >= 0 ? battle() : front_runner()));
    }
    /* Down a tunnel no trackside camera sees the kart: chase it through. */
    float ts = karts[shot_target].tp.s;
    if(shot_mode == CAM_TV && ntv > 0 && tv_hidden(tv_for(ts), ts)) {
        shot_mode = CAM_CHASE;
        shot_t = 0;
        shot_len = mode_pick == CAM_AUTO ? rangef(6.0f, 9.0f) : 3.0f;
        cut = true;
    }
}

/* ------------------------------------------------------------------ */
/* Cameras                                                             */

static void basis(Camera *c, V3 pos, V3 look, float fov) {
    c->pos = pos;
    c->fwd = vnorm(vsub(look, pos));
    c->right = vnorm(vcross(v3(0, 1, 0), c->fwd));
    c->up = vcross(c->fwd, c->right);
    c->fov = fov;
    c->shake = 0;
}

/* Ease toward a shot, or jump to it on a cut. */
static void frame(Camera *c, V3 pos, V3 look, float fov, float pos_rate, float look_rate, float dt) {
    static V3 pos_s;
    if(cut) {
        pos_s = pos;
        look_s = look;
        fov_s = fov;
    }
    else {
        pos_s = vlerp(pos_s, pos, smooth_k(pos_rate, dt));
        look_s = vlerp(look_s, look, smooth_k(look_rate, dt));
        fov_s += (fov - fov_s) * smooth_k(3.0f, dt);
    }
    basis(c, pos_s, look_s, fov_s);
}

/* The point to keep in frame: the kart, or it and a rival close by. */
static V3 subject(const Kart *k, float *width) {
    V3 p = vadd(k->pos, v3(0, 0.7f, 0));
    *width = 11.0f;
    for(int i = 0; i < kart_count; i++) {
        const Kart *o = &karts[i];
        if(o == k || abs(o->place - k->place) != 1) continue;
        float sep = vlen(vsub(o->pos, k->pos));
        if(sep < 14.0f) {
            p = vlerp(p, vadd(o->pos, v3(0, 0.7f, 0)), 0.5f);
            *width = fmaxf(*width, sep * 1.5f + 8.0f);
            break;
        }
    }
    return p;
}

static void tv_shot(Camera *cam, const Kart *k, float dt) {
    int want = tv_for(k->tp.s);
    /* A kart wobbling over a handover point must not flick between two. */
    if(want != tv_cur) {
        if(tv_cur < 0 || cut) {
            tv_cur = want;
        }
        else if(want == tv_next) {
            tv_next_t += dt;
            if(tv_next_t > 0.25f) {
                tv_cur = want;
                cut = true;
            }
        }
        else {
            tv_next = want;
            tv_next_t = 0;
        }
    }
    float width;
    V3 look = vadd(subject(k, &width), vscale(k->vel, 0.12f));
    V3 pos = tv[tv_cur].pos;
    float dist = vlen(vsub(look, pos));
    float fov = clampf(2.0f * atanf(width * 0.5f / fmaxf(dist, 1.0f)), 0.16f, 1.25f);
    frame(cam, pos, look, fov, 1, 7.0f, dt);
}

static void heli_shot(Camera *cam, const Kart *k, float dt) {
    heli_a += dt * 0.16f;
    float a = k->heading + DK_PI + sinf(heli_a) * 0.9f;
    V3 pos = vadd(k->pos, v3(sinf(a) * 24.0f, 15.0f, cosf(a) * 24.0f));
    V3 look = vmad(k->pos, heading_fwd(k->heading), 5.0f);
    frame(cam, pos, look, 0.95f, 1.6f, 4.0f, dt);
}

static void front_shot(Camera *cam, const Kart *k, float dt) {
    V3 f = heading_fwd(k->heading);
    V3 pos = vadd(vmad(k->pos, f, 8.5f), v3(0, k->gnd - k->pos.y + 2.3f, 0));
    pos.y = fmaxf(pos.y, k->pos.y + 1.2f);
    V3 look = vadd(vmad(k->pos, f, -7.0f), v3(0, 0.6f, 0));
    frame(cam, pos, look, 1.2f, 6.0f, 6.0f, dt);
}

static void blimp_shot(Camera *cam, const Kart *k, float dt) {
    V3 f = heading_fwd(k->heading);
    V3 pos = vadd(vmad(k->pos, f, -36.0f), v3(0, 58.0f, 0));
    V3 look = vmad(k->pos, f, 18.0f);
    frame(cam, pos, look, 1.0f, 0.9f, 2.0f, dt);
}

/* Countdown: a crane drifting down over the grid. */
static void grid_shot(Camera *cam, float dt) {
    float e = clampf(game.countdown / 3.0f, 0, 1);
    e = e * e * (3 - 2 * e);
    V3 from = vadd(track_point(22.0f, 10.0f), v3(0, 13.0f, 0));
    V3 to = vadd(track_point(2.0f, 9.0f), v3(0, 3.2f, 0));
    V3 look = vadd(track_point(-16.0f, 0), v3(0, 0.5f, 0));
    from.y += track_height(22.0f);
    to.y += track_height(2.0f);
    look.y += track_height(-16.0f);
    frame(cam, vlerp(from, to, e), look, 1.05f, 3.0f, 3.0f, dt);
}

void game_chase_camera(Player *pl, float dt);

void spectate_camera(Camera *cam, float dt) {
    const Kart *k = &karts[shot_target];
    if(game.state == ST_COUNTDOWN && mode_pick == CAM_AUTO) {
        grid_shot(cam, dt);
        cut = false;
        return;
    }
    switch(shot_mode) {
        case CAM_CHASE: {
            Player *pl = &game.pl[0];
            if(cut) {
                pl->cam_yaw = k->heading;
                pl->cam_height = k->gnd + 2.25f;
                pl->cam_dist = 5.3f;
                pl->fov = 1.3f;
            }
            game_chase_camera(pl, dt);
            if(cam != &pl->cam) *cam = pl->cam;
            look_s = vadd(cam->pos, cam->fwd);
            break;
        }
        case CAM_HELI: heli_shot(cam, k, dt); break;
        case CAM_FRONT: front_shot(cam, k, dt); break;
        case CAM_BLIMP: blimp_shot(cam, k, dt); break;
        default: tv_shot(cam, k, dt); break;
    }
    cut = false;
}

void spectate_set_camera(Camera *cam, V3 pos, V3 look, float fov) {
    basis(cam, pos, look, fov);
}

float spectate_tv_preview(Camera *cam, int i) {
    if(ntv == 0) place_cameras();
    if(i < 0 || i >= ntv) return 0;
    /* Look at the middle of the stretch this camera covers. */
    int p = (i + ntv - 1) % ntv;
    float mid = tv[p].hand + ahead(tv[p].hand, tv[i].hand) * 0.5f;
    V3 look = track_point(mid, 0);
    look.y = track_road_y(track_wrap(mid), 0) + 0.7f;
    float dist = vlen(vsub(look, tv[i].pos));
    basis(cam, tv[i].pos, look, clampf(2.0f * atanf(10.0f / fmaxf(dist, 1)), 0.2f, 1.25f));
    return track_wrap(mid);
}

/* ------------------------------------------------------------------ */
/* Overlay                                                             */

static void swatch(float x, float y, uint32_t col) {
    r_hdr(h_flat);
    r_quad2d(x - 1, y - 1, x + 13, y + 13, 0, 0, 1, 1, 0xff0c1430, 16.4f);
    r_quad2d(x, y, x + 12, y + 12, 0, 0, 1, 1, col | 0xff000000, 16.5f);
}

static void gap_text(char *buf, int n, const Kart *k, const Kart *first) {
    buf[0] = '\0';
    if(k == first) {
        if(k->finished) hud_format_time(buf, k->finish_time);
        return;
    }
    if(k->finished && first->finished) {
        snprintf(buf, n, "+%.1f", (double)fmaxf(k->finish_time - first->finish_time, 0));
        return;
    }
    float behind = first->progress - k->progress;
    if(behind >= track.length) {
        int laps = (int)(behind / track.length);
        snprintf(buf, n, "+%d LAP%s", laps, laps > 1 ? "S" : "");
        return;
    }
    int i = (int)(k->progress / GAP_STEP);
    if(i >= 0 && i < lead_n && lead_t[i] >= 0) {
        snprintf(buf, n, "+%.1f", (double)fmaxf(game.race_time - lead_t[i], 0));
    }
}

static void standings(void) {
    const Kart *first = &karts[placed(1)];
    float x0 = 14, y0 = 14, row = 23, x1 = 222;
    hud_panel(x0, y0, x1, y0 + 34 + row * kart_count, argb(170, 20, 60, 160), argb(170, 10, 30, 90), 15);
    char buf[24];
    int lap = first->lap < 1 ? 1 : first->lap > game.laps ? game.laps : first->lap;
    snprintf(buf, sizeof(buf), "LAP %d/%d", lap, game.laps);
    if(game.state == ST_COUNTDOWN) snprintf(buf, sizeof(buf), "%d LAPS", game.laps);
    hud_text(x0 + 10, y0 + 4, 0.52f, ALIGN_LEFT, argb(255, 255, 220, 80), buf);
    /* A blinking LIVE light. */
    bool lit = ((int)(spec_t * 1.5f) & 1) == 0;
    r_hdr(h_flat);
    r_quad2d(x1 - 58, y0 + 11, x1 - 48, y0 + 21, 0, 0, 1, 1, lit ? 0xffff3030 : 0xff802020, 16.5f);
    hud_text(x1 - 10, y0 + 5, 0.45f, ALIGN_RIGHT, 0xffffffff, "LIVE");
    for(int p = 1; p <= kart_count; p++) {
        const Kart *k = &karts[placed(p)];
        float y = y0 + 32 + (p - 1) * row;
        if(placed(p) == shot_target) {
            r_hdr(h_flat);
            r_quad2d(x0 + 4, y - 1, x1 - 4, y + row - 2, 0, 0, 1, 1, argb(90, 255, 255, 255), 15.5f);
        }
        snprintf(buf, sizeof(buf), "%d", p);
        hud_text(x0 + 18, y, 0.46f, ALIGN_CENTER, hud_place_color(p), buf);
        swatch(x0 + 32, y + 4, k->ui_color);
        hud_text(x0 + 52, y, 0.46f, ALIGN_LEFT, k->finished ? argb(255, 190, 255, 190) : 0xffffffff,
                 k->name ? k->name : "?");
        gap_text(buf, sizeof(buf), k, first);
        hud_text(x1 - 10, y + 2, 0.4f, ALIGN_RIGHT, argb(255, 255, 226, 120), buf);
    }
}

static void lower_third(void) {
    const Kart *k = &karts[shot_target];
    char buf[48];
    float x0 = 24, y0 = 392, x1 = 400;
    hud_panel(x0, y0, x1, y0 + 56, argb(190, 20, 60, 160), argb(190, 10, 30, 90), 15);
    r_hdr(h_flat);
    r_quad2d(x0, y0, x0 + 8, y0 + 56, 0, 0, 1, 1, k->ui_color | 0xff000000, 15.6f);
    snprintf(buf, sizeof(buf), "%d%s", k->place, hud_ordinal(k->place));
    hud_text(x0 + 20, y0 + 4, 0.85f, ALIGN_LEFT, hud_place_color(k->place), buf);
    hud_text(x0 + 92, y0 + 6, 0.8f, ALIGN_LEFT, 0xffffffff, k->name ? k->name : "?");
    const char *cam = mode_names[shot_mode];
    if(game.state == ST_COUNTDOWN && mode_pick == CAM_AUTO) cam = "GRID";
    if(shot_mode == CAM_TV && tv_cur >= 0 && !(game.state == ST_COUNTDOWN && mode_pick == CAM_AUTO))
        snprintf(buf, sizeof(buf), "TV CAM %d", tv_cur + 1);
    else
        snprintf(buf, sizeof(buf), "%s CAM", cam);
    char tail[32];
    snprintf(tail, sizeof(tail), "%.0f KM/H", (double)(fabsf(k->speed) * 3.6f));
    hud_text(x1 - 12, y0 + 6, 0.42f, ALIGN_RIGHT, argb(220, 200, 225, 255), buf);
    if(game.state != ST_COUNTDOWN) hud_text(x1 - 12, y0 + 28, 0.42f, ALIGN_RIGHT, argb(255, 255, 226, 120), tail);
    const NetLobby *l = net_lobby();
    if(l && l->id >= 0) {
        snprintf(buf, sizeof(buf), "LOBBY %d  -  %d WATCHING", l->id + 1, l->watchers);
        hud_text(x0 + 20, y0 + 34, 0.36f, ALIGN_LEFT, argb(220, 200, 225, 255), buf);
    }
}

static void name_tags(void) {
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(i == shot_target && (shot_mode == CAM_CHASE || shot_mode == CAM_FRONT)) continue;
        float d = vlen(vsub(k->pos, r_cam.pos));
        if(d > 140.0f * 1.3f / fmaxf(r_cam.fov, 0.16f)) continue;
        float sx, sy, iw;
        if(!r_project(vadd(k->pos, v3(0, 2.0f, 0)), &sx, &sy, &iw)) continue;
        if(sx < 10 || sx > 630 || sy < 30 || sy > 470) continue;
        hud_text(sx, sy - 18, 0.36f, ALIGN_CENTER, color_lerp(k->ui_color | 0xff000000, 0xffffffff, 0.35f),
                 k->name ? k->name : "?");
    }
}

static void draw(void);

void spectate_draw(void) {
    if(!on) return;
#ifdef DK_AT_SPECTATE
    uint64_t t0 = timer_us_gettime64();
    draw();
    uint64_t us = timer_us_gettime64() - t0;
    cost_draw += us;
    if(us > cost_draw_max) cost_draw_max = us;
    if(++cost_frames == 600) {
        printf("DK SPECTATE: overlay %.3f ms/frame (max %.3f), director %.3f ms/step (max %.3f)\n",
               (double)cost_draw / cost_frames / 1000.0, (double)cost_draw_max / 1000.0,
               (double)cost_upd / (cost_steps ? cost_steps : 1) / 1000.0, (double)cost_upd_max / 1000.0);
        cost_draw = cost_upd = cost_draw_max = cost_upd_max = 0;
        cost_frames = cost_steps = 0;
    }
#else
    draw();
#endif
}

static void draw(void) {
    if(!hud_hidden) {
        name_tags();
        standings();
        hud_panel(468, 18, 628, 166, argb(120, 10, 30, 90), argb(120, 10, 30, 90), 15);
        hud_draw_track_map(map_cx, map_cy, map_sc, 18.0f, true);
        lower_third();
    }
    if(banner_t > 0) {
        float a = clampf(banner_t / 0.5f, 0, 1), s = 1.3f + 0.05f * sinf(spec_t * 7);
        hud_text(320, 180, s, ALIGN_CENTER, color_alpha(argb(255, 255, 220, 60), a), banner);
    }
    if(toast_t > 0)
        hud_text(320, 66, 0.6f, ALIGN_CENTER, color_alpha(0xffffffff, clampf(toast_t / 0.3f, 0, 1)), toast);
    if(hint_t > 0 && !hud_hidden)
        hud_text(320, 456, 0.4f, ALIGN_CENTER, color_alpha(argb(255, 220, 230, 255), clampf(hint_t, 0, 1)),
                 "X: CAMERA   LEFT/RIGHT: KART   Y: OVERLAY   START: LEAVE");
}
