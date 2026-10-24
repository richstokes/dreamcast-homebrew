/* Spiral Canyon's final-lap biplane.

   As a human reaches the long way home on the final lap, a red biplane
   comes in low from behind, passes over the kart and takes station ~30 m
   ahead of it, criss-crossing the road in banked S-turns through the
   chicane and down the next straight, then pulls away ahead and climbs
   out: about six seconds in view. The flight is paced to the kart along
   the lap (a crash doesn't strand it), its heading the road's plus its
   drift across it. The mesh comes from tools/build_biplane.py (Blender),
   lit once per race for the track's sun; the propeller is its own mesh,
   spun about the hub. ~830 triangles while it flies, and a drone loop
   (audio_plane) tuned for range and doppler. */
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
#define WEAVE_T0   1.0f      /* criss-crossing the road from here */
#define EXIT_T     6.0f      /* then away ahead, climbing */
#define SHOW_LEN   8.0f
#define START_BACK 16.0f     /* metres behind the kart at the trigger */
#define AHEAD      30.0f     /* its station ahead of the kart */
#define WEAVE_D    6.0f      /* metres either side of the road's centre */
#define WEAVE_P    4.5f      /* seconds for a there-and-back */
#define CRUISE_H   7.5f      /* over the road, weaving */
#define PASS_H     6.0f      /* coming over the kart */
#define PROP_SPIN  47.0f     /* rad/s: strobes into a spin at 60 Hz */
#define MAX_SHOWS  2

#define BODY_N (int)(sizeof(dk_plane_body_verts) / sizeof(dk_plane_body_verts[0]))
#define PROP_N (int)(sizeof(dk_plane_prop_verts) / sizeof(dk_plane_prop_verts[0]))
#define BODY_I (int)(sizeof(dk_plane_body_index) / sizeof(dk_plane_body_index[0]))
#define PROP_I (int)(sizeof(dk_plane_prop_index) / sizeof(dk_plane_prop_index[0]))

static RV body[BODY_N] __attribute__((aligned(32)));
static RV prop[PROP_N] __attribute__((aligned(32)));
static const pvr_poly_hdr_t *h_plane;

typedef struct {
    bool active;
    float t;
    int kart;          /* the kart it flies ahead of */
    float s, d, y;     /* unwrapped lap distance, lateral offset, height over the road */
    float ks;          /* the kart's lap distance, unwrapped, and where it was */
    float ks_last;
    V3 pos;            /* the model origin */
    float yaw, pitch, roll;
    float range, closing;   /* from player 1's camera, and how fast that shrinks */
#ifdef DK_AUTOTEST
    float min_clear, near_kart, over_kart;
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
}

/* The flight about the kart at show time t: how far ahead of it along the
   lap, how far across the road, and how high over it. */
static void flight(float t, float *ahead, float *d, float *h) {
    float in = smoothstep01(t / ENTRY_T);
    float out = t > EXIT_T ? t - EXIT_T : 0.0f;
    *ahead = lerpf(-START_BACK, AHEAD, in) + 9.0f * out * out;
    *h = lerpf(PASS_H, CRUISE_H, in) + 5.0f * out * out;
    float w = t > WEAVE_T0 ? t - WEAVE_T0 : 0.0f;
    float amp = WEAVE_D * smoothstep01(w / 1.5f) * (1.0f - smoothstep01(out / 1.2f));
    *d = amp * sinf(w * DK_TAU / WEAVE_P);
}

/* Place the plane for show time t; the heading follows the road plus the
   way it is moving across it, and it banks into each turn. */
static void place(Show *sh, float dt) {
    const Kart *k = &karts[sh->kart];
    sh->ks += track_delta(sh->ks_last, k->tp.s);
    sh->ks_last = k->tp.s;
    float ahead, d, h;
    flight(sh->t, &ahead, &d, &h);
    float s = sh->ks + ahead;
    float ds = fmaxf(s - sh->s, 0.05f), dd = d - sh->d;
    float y = track_road_y(track_wrap(s), d) + h;
    V3 tg = track_tangent(track_wrap(s));
    float yaw = atan2f(tg.x, tg.z) + atan2f(dd, ds);
    float turn = yaw - sh->yaw;
    if(turn > DK_PI) turn -= DK_TAU;
    if(turn < -DK_PI) turn += DK_TAU;
    float v = ds / fmaxf(dt, 1e-3f);
    /* Right turn: the yaw rises, the right wing goes down (negative roll). */
    float bank = clampf(atanf(-v * turn / fmaxf(dt, 1e-3f) / 9.8f) * 0.8f, -0.9f, 0.9f);
    sh->roll += (bank - sh->roll) * fminf(1.0f, dt * 2.5f);
    sh->pitch = atan2f(y - sh->y, sqrtf(ds * ds + dd * dd));
    sh->yaw = yaw;
    sh->s = s;
    sh->d = d;
    sh->y = y;
    sh->pos = track_point(track_wrap(s), d);
    sh->pos.y = y;
}

static void start_show(const Kart *k) {
    for(int i = 0; i < MAX_SHOWS; i++)
        if(!shows[i].active) {
            Show *sh = &shows[i];
            memset(sh, 0, sizeof(*sh));
            sh->active = true;
            sh->kart = (int)(k - karts);
            sh->ks = sh->ks_last = k->tp.s;
            float ahead, d, h;
            flight(0, &ahead, &d, &h);
            sh->s = sh->ks + ahead - 0.5f;
            sh->d = d;
            sh->y = track_road_y(track_wrap(sh->s), d) + h;
            V3 tg = track_tangent(k->tp.s);
            sh->yaw = atan2f(tg.x, tg.z);
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
                   "%d frames near rock, in view %.1f s\n", (double)sh->over_kart, (double)sh->near_kart,
                   (double)sh->min_clear, sh->rock_hits, (double)(sh->seen_frames * DK_DT));
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
            printf("DK PLANE near rock at %.2f s, s %.0f d %.1f\n", (double)sh->t, (double)track_wrap(sh->s),
                   (double)sh->d);
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
    float fade = smoothstep01(sh->t / 0.4f) * (1.0f - smoothstep01((sh->t - (SHOW_LEN - 1.2f)) / 1.2f));
    int vol = (int)(235.0f * clampf(16.0f / (sh->range + 6.0f), 0, 1) * fade);
    int pan = (int)clampf(128.0f + vdot(vnorm(rel), c->right) * 90.0f, 0, 255);
    /* Doppler, overdone a little so it reads. */
    audio_plane(vol, pan, 340.0f / fmaxf(340.0f - 1.6f * sh->closing, 200.0f));
}

void biplane_draw(void) {
    if(!staged) return;
    for(int i = 0; i < MAX_SHOWS; i++) {
        const Show *sh = &shows[i];
        if(!sh->active || !r_sphere_visible(sh->pos, 5.0f)) continue;
        float m[16], rz[16], mp[16];
        m4_translate_rotate(m, sh->pos, sh->yaw, sh->pitch, sh->roll);
        r_load_model(m);
        r_hdr(h_plane);
        r_mesh(body, BODY_N, dk_plane_body_index, BODY_I, false);
        m4_rot_z(rz, sh->t * PROP_SPIN);
        rz[14] = DK_PLANE_HUB_Z_MM * 0.001f;
        m4_mul(mp, m, rz);
        r_load_model(mp);
        r_mesh(prop, PROP_N, dk_plane_prop_index, PROP_I, false);
    }
}

float biplane_show_time(void) {
    return newest_t;
}
