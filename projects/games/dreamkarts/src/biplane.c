/* Spiral Canyon's final-lap biplane.

   As a human reaches the long way home on the final lap, a red biplane
   comes in low from behind, passes over the kart and takes station ~30 m
   ahead of it, draws one slow S across the road (out past each edge),
   then pulls away ahead, climbing, and fades into the distance: about
   eleven seconds in all.

   It flies a guide line, not the road: the road's centreline over the
   next few hundred metres, smoothed so the chicane becomes a gentle
   drift, built when the show starts. Along it the plane is paced to the
   kart by a spring (the kart's speed, slides and bumps don't reach it),
   and the S is a function of time alone, so the heading and bank change
   smoothly. The mesh comes from tools/build_biplane.py (Blender), lit
   once per race for the track's sun; the propeller is its own mesh, spun
   about the hub. ~830 triangles while it flies (in the translucent list,
   back faces culled, while it fades), and a drone loop (audio_plane)
   tuned for range and doppler. */
#include "biplane.h"
#include "audio.h"
#include "biplane_mesh.h"
#include "kart.h"
#include "render.h"
#include "track.h"
#include "world.h"

static float smoothstep01(float x) {
    x = clampf(x, 0, 1);
    return x * x * (3.0f - 2.0f * x);
}

/* The show, in seconds from the trigger. */
#define ENTRY_T    1.5f      /* in from behind, over the kart, to its station */
#define S_T0       1.2f      /* the S across the road */
#define S_LEN      6.5f
#define EXIT_T     7.7f      /* then away ahead, climbing */
#define FADE_T     2.2f      /* fading out over the show's last seconds */
#define SHOW_LEN   12.0f
#define START_BACK 16.0f     /* metres behind the kart at the trigger */
#define AHEAD      30.0f     /* its station ahead of the kart */
#define S_OVER     2.5f      /* how far past each road edge the S reaches */
#define CRUISE_H   7.5f      /* over the road */
#define PASS_H     6.0f      /* coming over the kart */
#define PACE_W     2.5f      /* the pacing spring, rad/s */
#define PROP_SPIN  47.0f     /* rad/s: strobes into a spin at 60 Hz */
#define MAX_SHOWS  2

/* The guide line: GUIDE_N points GUIDE_DS apart from the show's start. */
#define GUIDE_N    96
#define GUIDE_DS   8.0f
#define GUIDE_R    4         /* box filter radius in points, three passes */

#define BODY_N (int)(sizeof(dk_plane_body_verts) / sizeof(dk_plane_body_verts[0]))
#define PROP_N (int)(sizeof(dk_plane_prop_verts) / sizeof(dk_plane_prop_verts[0]))
#define BODY_I (int)(sizeof(dk_plane_body_index) / sizeof(dk_plane_body_index[0]))
#define PROP_I (int)(sizeof(dk_plane_prop_index) / sizeof(dk_plane_prop_index[0]))

static RV body[BODY_N] __attribute__((aligned(32)));
static RV prop[PROP_N] __attribute__((aligned(32)));
static const pvr_poly_hdr_t *h_plane, *h_plane_tr;
static uint8_t drawn_alpha = 255;    /* the alpha body[] and prop[] carry now */

typedef struct {
    bool active;
    float t;
    int kart;          /* the kart it flies ahead of */
    float g0;          /* lap distance of the guide's first point */
    V3 guide[GUIDE_N]; /* the smoothed centreline, road height in y */
    float s, v;        /* unwrapped lap distance along the guide, and its rate */
    float ks, ks_last; /* the kart's lap distance, unwrapped */
    float half;        /* the road's half width at the start */
    V3 pos;            /* the model origin */
    float yaw, pitch, roll;
    float range, closing;   /* from player 1's camera, and how fast that shrinks */
#ifdef DK_AUTOTEST
    float min_clear, near_kart, over_kart, max_turn, max_turn_t;
    int rock_hits, seen_frames;
#endif
} Show;

static Show shows[MAX_SHOWS];
static bool staged;
static bool seen[MAX_PLAYERS];
static float prev_s[MAX_PLAYERS];
static float newest_t = -1;

/* Where the show starts: the long way home, heading west with the low sun
   behind the karts, so the plane is lit on the side the camera sees. */
static float trigger_s(void) {
    return track_seg_s(8) + 2.0f;
}

/* Lit once per race for the heading it mostly flies: a high fill so the
   red reads whichever way it banks, the sun on what faces it, warmed like
   the karts. */
static void light(RV *out, const dk_plane_vert_t *in, int n, V3 sun) {
    for(int i = 0; i < n; i++) {
        V3 nn = v3(in[i].nx / 127.0f, in[i].ny / 127.0f, in[i].nz / 127.0f);
        float l = 0.70f + 0.38f * fmaxf(0.0f, vdot(nn, sun)) + 0.12f * fmaxf(0.0f, nn.y);
        int r = (int)(in[i].r * l * world_light[0] / 256.0f);
        int g = (int)(in[i].g * l * world_light[1] / 256.0f);
        int b = (int)(in[i].b * l * world_light[2] / 256.0f);
        out[i] = (RV){in[i].x * 0.001f, in[i].y * 0.001f, in[i].z * 0.001f, 0, 0,
                      argb(255, r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b)};
    }
}

void biplane_init(void) {
    h_plane = r_header(TEX_NONE, HF_OP);
    h_plane_tr = r_header(TEX_NONE, HF_TR);
}

void biplane_reset(void) {
    staged = track.id == TRACK_CANYON;
    memset(shows, 0, sizeof(shows));
    memset(seen, 0, sizeof(seen));
    for(int p = 0; p < MAX_PLAYERS; p++) prev_s[p] = -1;
    newest_t = -1;
    if(!staged) return;
    /* The sun in plane space, for a plane heading along the stretch. */
    V3 f = track_tangent(trigger_s() + 60.0f);
    float yaw = atan2f(f.x, f.z), cs = cosf(yaw), sn = sinf(yaw);
    V3 w = world_sun_dir;
    V3 sun = vnorm(v3(w.x * cs - w.z * sn, w.y, w.x * sn + w.z * cs));
    light(body, dk_plane_body_verts, BODY_N, sun);
    light(prop, dk_plane_prop_verts, PROP_N, sun);
    drawn_alpha = 255;
}

/* The plane over the kart at show time t: how far ahead of it along the
   guide, how far across, and how high over the road. The S runs on an
   eased clock so it sets off and settles without a jerk. */
static void flight(const Show *sh, float t, float *ahead, float *d, float *h) {
    float in = smoothstep01(t / ENTRY_T);
    float out = t > EXIT_T ? t - EXIT_T : 0.0f;
    *ahead = lerpf(-START_BACK, AHEAD, in) + 11.0f * out * out;
    *h = lerpf(PASS_H, CRUISE_H, in) + 2.2f * out * out;
    float u = smoothstep01((t - S_T0) / S_LEN);
    *d = (sh->half + S_OVER) * sinf(u * DK_TAU);
}

/* The guide at unwrapped lap distance s: point (road height in y) and
   unit direction. */
static V3 guide_at(const Show *sh, float s, V3 *dir) {
    float f = clampf((s - sh->g0) / GUIDE_DS, 0, GUIDE_N - 1.001f);
    int i = (int)f;
    float a = f - (float)i;
    V3 p = vadd(vscale(sh->guide[i], 1.0f - a), vscale(sh->guide[i + 1], a));
    /* The direction blends the tangents at the two points, so the heading
       turns smoothly rather than in a step at each point. */
    V3 t0 = vsub(sh->guide[i + 1], sh->guide[i > 0 ? i - 1 : 0]);
    V3 t1 = vsub(sh->guide[i + 2 < GUIDE_N ? i + 2 : GUIDE_N - 1], sh->guide[i]);
    V3 dv = vadd(vscale(vnorm(t0), 1.0f - a), vscale(vnorm(t1), a));
    *dir = vnorm(v3(dv.x, 0, dv.z));
    return p;
}

static void build_guide(Show *sh, float s0) {
    V3 raw[GUIDE_N + 2 * GUIDE_R], tmp[GUIDE_N + 2 * GUIDE_R];
    int n = GUIDE_N + 2 * GUIDE_R;
    for(int i = 0; i < n; i++) {
        float s = track_wrap(s0 + (float)(i - GUIDE_R) * GUIDE_DS);
        raw[i] = track_point(s, 0);
        raw[i].y = track_road_y(s, 0);
    }
    for(int pass = 0; pass < 3; pass++) {
        for(int i = 0; i < n; i++) {
            V3 sum = v3(0, 0, 0);
            int c = 0;
            for(int k = -GUIDE_R; k <= GUIDE_R; k++)
                if(i + k >= 0 && i + k < n) {
                    sum = vadd(sum, raw[i + k]);
                    c++;
                }
            tmp[i] = vscale(sum, 1.0f / (float)c);
        }
        memcpy(raw, tmp, sizeof(raw));
    }
    memcpy(sh->guide, raw + GUIDE_R, sizeof(sh->guide));
    sh->g0 = s0;
}

/* Place the plane for show time t. The heading is the guide's plus the
   way the S carries it across; it banks into the turn that makes. */
static void place(Show *sh, float dt) {
    const Kart *k = &karts[sh->kart];
    sh->ks += track_delta(sh->ks_last, k->tp.s);
    sh->ks_last = k->tp.s;
    float ahead, d, h, ahead0, d0, h0;
    flight(sh, sh->t, &ahead, &d, &h);
    flight(sh, sh->t - dt, &ahead0, &d0, &h0);
    /* Pace the kart: a spring towards its spot ahead of the kart, moving
       at the kart's speed (plus how fast that spot runs ahead). */
    float vt = fmaxf(k->speed, 8.0f) + (ahead - ahead0) / dt;
    float a = PACE_W * PACE_W * (sh->ks + ahead - sh->s) + 2.0f * PACE_W * (vt - sh->v);
    sh->v = fmaxf(sh->v + a * dt, 5.0f);
    sh->s += sh->v * dt;
    V3 dir;
    V3 g = guide_at(sh, sh->s, &dir);
    V3 right = v3(dir.z, 0, -dir.x);
    float side = (d - d0) / dt;
    float yaw = atan2f(dir.x, dir.z) + atan2f(side, sh->v);
    float turn = yaw - sh->yaw;
    if(turn > DK_PI) turn -= DK_TAU;
    if(turn < -DK_PI) turn += DK_TAU;
    /* Right turn: the yaw rises, the right wing goes down (negative roll). */
    float bank = clampf(atanf(-sh->v * turn / dt / 9.8f), -0.8f, 0.8f);
    sh->roll += (bank - sh->roll) * fminf(1.0f, dt * 3.0f);
    V3 pos = vmad(g, right, d);
    pos.y = g.y + h;
    sh->pitch = atan2f(pos.y - sh->pos.y, fmaxf(sh->v * dt, 0.01f));
#ifdef DK_AUTOTEST
    if(sh->t > 0.1f && fabsf(turn) / dt > sh->max_turn) {
        sh->max_turn = fabsf(turn) / dt;
        sh->max_turn_t = sh->t;
    }
#endif
    sh->yaw = yaw;
    sh->pos = pos;
}

static void start_show(const Kart *k) {
    for(int i = 0; i < MAX_SHOWS; i++)
        if(!shows[i].active) {
            Show *sh = &shows[i];
            memset(sh, 0, sizeof(*sh));
            sh->active = true;
            sh->kart = (int)(k - karts);
            sh->ks = sh->ks_last = k->tp.s;
            sh->half = track_half_at(&k->tp);
            float ahead, d, h;
            flight(sh, 0, &ahead, &d, &h);
            build_guide(sh, k->tp.s + ahead - 2.0f * GUIDE_DS);
            sh->s = sh->ks + ahead;
            sh->v = fmaxf(k->speed, 8.0f);
            V3 dir;
            V3 g = guide_at(sh, sh->s, &dir);
            sh->yaw = atan2f(dir.x, dir.z);
            sh->pos = v3(g.x, g.y + h, g.z);
            place(sh, DK_DT);
            sh->range = 1e9f;
#ifdef DK_AUTOTEST
            sh->min_clear = 1e9f;
            sh->near_kart = 1e9f;
#endif
            return;
        }
}

void biplane_update(float dt) {
    newest_t = -1;
    if(!staged) return;
    /* Each human reaching the long way home on the final lap; a second
       human arriving within a couple of seconds shares the first show. */
    float trigger = trigger_s();
    for(int p = 0; p < game.num_players; p++) {
        const Kart *k = &karts[game.pl[p].kart];
        float s = k->tp.s;
        if(!seen[p] && k->lap == game.laps && game.laps > 1 && prev_s[p] >= 0 &&
           track_delta(prev_s[p], trigger) > 0 && track_delta(trigger, s) >= 0 && track_delta(prev_s[p], s) < 20.0f) {
            seen[p] = true;
            bool recent = false;
            for(int i = 0; i < MAX_SHOWS; i++)
                if(shows[i].active && shows[i].t < 2.0f) recent = true;
            if(!recent) start_show(k);
        }
        prev_s[p] = s;
    }
    for(int i = 0; i < MAX_SHOWS; i++) {
        Show *sh = &shows[i];
        if(!sh->active) continue;
        sh->t += dt;
        if(sh->t > SHOW_LEN) {
            sh->active = false;
#ifdef DK_AUTOTEST
            printf("DK PLANE show: %.1f m over the kart (%.1f m to one side), %.1f m min ground clearance, "
                   "%d frames near rock, in view %.1f s, sharpest turn %.0f deg/s at %.1f s, %.0f m away at the end\n",
                   (double)sh->over_kart, (double)sh->near_kart, (double)sh->min_clear, sh->rock_hits,
                   (double)(sh->seen_frames * DK_DT), (double)(sh->max_turn * 180.0f / DK_PI), (double)sh->max_turn_t, (double)sh->range);
#endif
            continue;
        }
        if(newest_t < 0 || sh->t < newest_t) newest_t = sh->t;
        place(sh, dt);
        /* For the drone: range to player 1's camera and how fast it closes. */
        float r = vlen(vsub(sh->pos, game.pl[0].cam.pos));
        if(sh->range < 1e8f) sh->closing += ((sh->range - r) / dt - sh->closing) * fminf(1.0f, dt * 8.0f);
        sh->range = r;
#ifdef DK_AUTOTEST
        /* The wheels hang 1.6 m below the origin, the wings reach 4 m. */
        float clear = sh->pos.y - 1.6f - world_ground_y(sh->pos.x, sh->pos.z);
        if(clear < sh->min_clear) sh->min_clear = clear;
        if(world_in_rock(sh->pos, 4.0f) && !sh->rock_hits++)
            printf("DK PLANE near rock at %.2f s, s %.0f\n", (double)sh->t, (double)track_wrap(sh->s));
        const Kart *k = &karts[sh->kart];
        float dx = sh->pos.x - k->pos.x, dz = sh->pos.z - k->pos.z;
        float near = sqrtf(dx * dx + dz * dz);
        if(near < sh->near_kart) {
            sh->near_kart = near;
            sh->over_kart = sh->pos.y - k->pos.y;
        }
        if(r_sphere_visible(sh->pos, 3.0f)) sh->seen_frames++;
#endif
    }
}

void biplane_audio(bool on) {
    const Show *sh = NULL;
    for(int i = 0; i < MAX_SHOWS && on && staged; i++)
        if(shows[i].active && (!sh || shows[i].t < sh->t)) sh = &shows[i];
    if(!sh) {
        audio_plane(0, 128, 1.0f);
        return;
    }
    const Camera *c = &game.pl[0].cam;
    V3 rel = vsub(sh->pos, c->pos);
    float fade = smoothstep01(sh->t / 0.4f) * (1.0f - smoothstep01((sh->t - (SHOW_LEN - FADE_T)) / FADE_T));
    int vol = (int)(235.0f * clampf(16.0f / (sh->range + 6.0f), 0, 1) * fade);
    int pan = (int)clampf(128.0f + vdot(vnorm(rel), c->right) * 90.0f, 0, 255);
    /* Doppler, overdone a little so it reads. */
    audio_plane(vol, pan, 340.0f / fmaxf(340.0f - 1.6f * sh->closing, 200.0f));
}

/* How opaque the plane is at show time t: fully until the fade. */
static float show_alpha(const Show *sh) {
    return 1.0f - smoothstep01((sh->t - (SHOW_LEN - FADE_T)) / FADE_T);
}

static void set_alpha(uint8_t a) {
    if(a == drawn_alpha) return;
    for(int i = 0; i < BODY_N; i++) body[i].c = (body[i].c & 0x00ffffffu) | ((uint32_t)a << 24);
    for(int i = 0; i < PROP_N; i++) prop[i].c = (prop[i].c & 0x00ffffffu) | ((uint32_t)a << 24);
    drawn_alpha = a;
}

static void draw_show(const Show *sh, const pvr_poly_hdr_t *h, bool cull) {
    if(!r_sphere_visible(sh->pos, 5.0f)) return;
    float m[16], rz[16], mp[16];
    m4_translate_rotate(m, sh->pos, sh->yaw, sh->pitch, sh->roll);
    r_load_model(m);
    r_hdr(h);
    r_mesh(body, BODY_N, dk_plane_body_index, BODY_I, cull);
    m4_rot_z(rz, sh->t * PROP_SPIN);
    rz[14] = DK_PLANE_HUB_Z_MM * 0.001f;
    m4_mul(mp, m, rz);
    r_load_model(mp);
    r_mesh(prop, PROP_N, dk_plane_prop_index, PROP_I, cull);
}

void biplane_draw(void) {
    if(!staged) return;
    for(int i = 0; i < MAX_SHOWS; i++)
        if(shows[i].active && show_alpha(&shows[i]) >= 1.0f) {
            set_alpha(255);
            draw_show(&shows[i], h_plane, false);
        }
}

/* Fading: in the translucent list with its back faces culled, so the far
   side of the fuselage and wings doesn't show through. */
void biplane_draw_tr(void) {
    if(!staged) return;
    for(int i = 0; i < MAX_SHOWS; i++) {
        float a = shows[i].active ? show_alpha(&shows[i]) : 1.0f;
        if(a >= 1.0f || a <= 0.0f) continue;
        set_alpha((uint8_t)(a * 255.0f));
        draw_show(&shows[i], h_plane_tr, true);
    }
}

float biplane_show_time(void) {
    return newest_t;
}
