/* Particle effects. Sparks are screen-aligned additive streaks along their
   velocity; puffs are alpha-blended billboards; flames are drawn directly
   from each boosting kart's exhausts every frame. */
#include "fx.h"
#include "models.h"
#include "render.h"
#include "texture_assets.h"
#include "world.h"

enum { P_SPARK, P_GLOW, P_SMOKE, P_DUST, P_STAR, P_CONFETTI };

typedef struct {
    V3 p, v;
    float life, max_life;
    float size, grow;
    float gravity, drag;
    uint32_t c0, c1;
    uint8_t type;
} Particle;

#define MAX_PARTICLES 700
static Particle parts[MAX_PARTICLES];
static int part_n;
static const pvr_poly_hdr_t *h_add, *h_smoke, *h_star, *h_flame, *h_spark_glow;
static float spawn_acc[MAX_KARTS][3];

uint32_t fx_stage_color(int stage) {
    switch(stage) {
        case 1: return argb(255, 70, 170, 255);
        case 2: return argb(255, 255, 150, 30);
        case 3: return argb(255, 210, 90, 255);
        default: return argb(255, 255, 236, 170);
    }
}

void fx_init(void) {
    h_add = r_header(TEX_GLOW, HF_TR | HF_ADD);
    h_spark_glow = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_smoke = r_header(TEX_SMOKE, HF_TR);
    h_star = r_header(TEX_STAR, HF_TR | HF_ADD);
    h_flame = r_header(TEX_FLAME, HF_TR | HF_ADD | HF_CLAMP);
}

void fx_clear(void) {
    part_n = 0;
    memset(spawn_acc, 0, sizeof(spawn_acc));
}

static Particle *spawn(int type, V3 p, V3 v, float life, float size, uint32_t c0, uint32_t c1) {
    if(part_n >= MAX_PARTICLES) {
        /* Recycle the oldest-looking slot. */
        int i = (int)(dk_rand() % MAX_PARTICLES);
        parts[i] = parts[--part_n];
    }
    Particle *q = &parts[part_n++];
    q->p = p;
    q->v = v;
    q->life = q->max_life = life;
    q->size = size;
    q->grow = 0;
    q->gravity = 0;
    q->drag = 0;
    q->c0 = c0;
    q->c1 = c1;
    q->type = (uint8_t)type;
    return q;
}

void fx_update(float dt) {
    for(int i = 0; i < part_n;) {
        Particle *q = &parts[i];
        q->life -= dt;
        if(q->life <= 0) {
            parts[i] = parts[--part_n];
            continue;
        }
        q->v.y -= q->gravity * dt;
        if(q->drag > 0) {
            float k = 1.0f - smooth_k(q->drag, dt);
            q->v = vscale(q->v, k);
        }
        q->p = vmad(q->p, q->v, dt);
        if(q->p.y < 0.02f && q->type == P_SPARK) {
            q->p.y = 0.02f;
            q->v.y = -q->v.y * 0.35f;
        }
        if(q->type == P_CONFETTI && q->p.y < 0.02f) {
            q->p.y = 0.02f;
            q->v = v3(0, 0, 0);
        }
        q->size += q->grow * dt;
        i++;
    }
}

void fx_confetti(V3 p, int count) {
    static const uint32_t cols[] = {0xffff4040, 0xffffd030, 0xff40c0ff, 0xff60e060, 0xffff70d0, 0xffffffff};
    for(int i = 0; i < count; i++) {
        V3 v = v3(frand_s() * 7.0f, 6.0f + frand() * 8.0f, frand_s() * 7.0f);
        Particle *q = spawn(P_CONFETTI, vadd(p, v3(frand_s() * 4, frand() * 2, frand_s() * 4)), v,
                            2.5f + frand() * 1.5f, 0.16f, cols[i % 6], cols[i % 6]);
        q->gravity = 7.0f;
        q->drag = 1.6f;
    }
}

static void burst_stars(V3 p, int n, uint32_t col, float speed) {
    for(int i = 0; i < n; i++) {
        V3 v = v3(frand_s() * speed, frand() * speed * 0.8f + 1.0f, frand_s() * speed);
        Particle *q = spawn(P_STAR, p, v, 0.35f + frand() * 0.25f, 0.35f + frand() * 0.2f, col, color_alpha(col, 0));
        q->drag = 4.0f;
        q->gravity = 3.0f;
    }
}

void fx_splash(V3 p, float strength) {
    int puffs = (int)(9 * strength), drops = (int)(12 * strength);
    for(int i = 0; i < puffs; i++) {
        V3 v = v3(frand_s() * 1.6f, (2.0f + frand() * 3.0f) * strength, frand_s() * 1.6f);
        Particle *q = spawn(P_SMOKE, vadd(p, v3(frand_s() * 0.6f, 0.1f, frand_s() * 0.6f)), v,
                            0.7f + frand() * 0.4f, 0.55f, argb(210, 245, 252, 255), argb(0, 220, 240, 255));
        q->gravity = 6.0f;
        q->grow = 1.3f;
        q->drag = 1.0f;
    }
    for(int i = 0; i < drops; i++) {
        V3 v = v3(frand_s() * 3.0f, (4.0f + frand() * 4.0f) * strength, frand_s() * 3.0f);
        Particle *q = spawn(P_SPARK, vadd(p, v3(0, 0.1f, 0)), v, 0.5f + frand() * 0.3f, 0.05f,
                            0xffffffff, argb(0, 200, 235, 255));
        q->gravity = 12.0f;
    }
}

void fx_kart(const Kart *k, float dt) {
    int idx = (int)(k - karts);
    V3 fwd = heading_fwd(k->heading + k->vis_yaw), right = heading_right(k->heading + k->vis_yaw);
    V3 wl = models_wheel_world(k, 2), wr = models_wheel_world(k, 3);
    float spd = fabsf(k->speed);
    /* Only karts near a human's camera are worth effects. */
    float near2 = 1e30f;
    for(int p = 0; p < game.num_players; p++) {
        V3 d = vsub(k->pos, karts[game.pl[p].kart].pos);
        near2 = fminf(near2, vdot(d, d));
    }
    if(near2 > 85.0f * 85.0f) return;
    /* Two views double the fill: halve particle rates in a split screen. */
    float split = game.num_players > 1 ? 0.5f : 1.0f;
    dt *= split;

    /* Drift sparks from both rear tyres. */
    if(k->drift == DRIFT_ACTIVE && k->grounded) {
        uint32_t c = fx_stage_color(k->stage);
        float rate = k->stage == 0 ? 40.0f : 120.0f + 30.0f * k->stage;
        spawn_acc[idx][0] += rate * dt;
        while(spawn_acc[idx][0] >= 1.0f) {
            spawn_acc[idx][0] -= 1.0f;
            V3 base = (dk_rand() & 1) ? wl : wr;
            V3 v = vadd(vscale(k->vel, 0.8f),
                        vadd(vscale(fwd, -(3.0f + frand() * 5.0f)),
                             vadd(v3(0, 1.5f + frand() * 3.5f, 0), vscale(right, frand_s() * 2.5f))));
            float life = 0.16f + frand() * 0.16f;
            Particle *q = spawn(P_SPARK, vadd(base, v3(0, 0.05f, 0)), v, life,
                                k->stage == 0 ? 0.035f : 0.06f, color_lerp(0xffffffff, c, k->stage == 0 ? 0.4f : 0.8f), color_alpha(c, 0));
            q->gravity = 14.0f;
        }
    }
    /* Tyre smoke while sliding on tarmac, sand spray off-road. */
    if(k->grounded && spd > 4.0f) {
        bool sand = k->surface == SURF_SAND;
        bool close = near2 < 40.0f * 40.0f;
        float rate = !close ? 0.0f : sand ? 22.0f : (k->drift == DRIFT_ACTIVE ? 10.0f : 0.0f);
        spawn_acc[idx][1] += rate * dt;
        while(spawn_acc[idx][1] >= 1.0f) {
            spawn_acc[idx][1] -= 1.0f;
            V3 base = (dk_rand() & 1) ? wl : wr;
            V3 v = vadd(vscale(k->vel, 0.25f), v3(frand_s() * 1.5f, 0.8f + frand() * 1.5f, frand_s() * 1.5f));
            uint32_t c = sand ? argb(150, 236, 214, 160) : argb(90, 240, 240, 240);
            Particle *q = spawn(sand ? P_DUST : P_SMOKE, vadd(base, v3(0, 0.2f, 0)), v, 0.5f + frand() * 0.4f,
                                sand ? 0.45f : 0.35f, c, color_alpha(c, 0));
            q->grow = sand ? 1.8f : 1.4f;
            q->drag = 2.0f;
        }
    }
    if(k->burnout_t > 0) {
        spawn_acc[idx][2] += 50.0f * dt;
        while(spawn_acc[idx][2] >= 1.0f) {
            spawn_acc[idx][2] -= 1.0f;
            V3 base = (dk_rand() & 1) ? wl : wr;
            Particle *q = spawn(P_SMOKE, vadd(base, v3(0, 0.2f, 0)),
                                v3(frand_s() * 2, 1.0f + frand() * 1.5f, frand_s() * 2), 0.9f, 0.5f,
                                argb(170, 250, 250, 250), argb(0, 250, 250, 250));
            q->grow = 2.0f;
            q->drag = 1.5f;
        }
    }

    uint32_t ev = k->events;
    if(ev & (EV_CHARGE1 | EV_CHARGE2 | EV_CHARGE3)) {
        uint32_t c = fx_stage_color(k->stage);
        burst_stars(wl, 5, c, 4.0f);
        burst_stars(wr, 5, c, 4.0f);
    }
    if(ev & (EV_BOOST | EV_DASH | EV_ROCKET)) {
        for(int e = 0; e < 2; e++) {
            V3 ex = models_exhaust_world(k, e);
            for(int i = 0; i < 6; i++) {
                V3 v = vadd(vscale(k->vel, 0.5f), vadd(vscale(fwd, -4.0f - frand() * 3.0f), v3(frand_s(), frand() * 1.5f, frand_s())));
                Particle *q = spawn(P_GLOW, ex, v, 0.3f, 0.35f, argb(255, 255, 220, 120), argb(0, 255, 80, 20));
                q->drag = 3.0f;
            }
        }
    }
    if(ev & EV_LAND) {
        for(int i = 0; i < 10; i++) {
            float a = (float)i / 10.0f * DK_TAU;
            V3 v = v3(sinf(a) * 3.0f, 0.6f, cosf(a) * 3.0f);
            uint32_t c = k->surface == SURF_SAND ? argb(150, 236, 214, 160) : argb(110, 245, 245, 245);
            Particle *q = spawn(P_SMOKE, vadd(k->pos, v3(0, 0.15f, 0)), vadd(v, vscale(k->vel, 0.6f)), 0.45f, 0.35f, c,
                                color_alpha(c, 0));
            q->grow = 1.6f;
            q->drag = 3.0f;
        }
    }
    if(ev & EV_WALL) {
        V3 side = k->tp.d > 0 ? right : vscale(right, -1);
        V3 hit = vadd(vmad(k->pos, side, 0.9f), v3(0, 0.6f, 0));
        burst_stars(hit, 7, argb(255, 255, 230, 90), 5.0f);
    }
    if(ev & EV_KART_BUMP) burst_stars(vadd(k->pos, v3(0, 0.8f, 0)), 4, 0xffffffff, 4.0f);
    if(ev & EV_CHARGE_JUMP) burst_stars(vadd(k->pos, v3(0, 0.3f, 0)), 8, 0xffffffff, 3.5f);
}

static void draw_flames(void) {
    r_hdr(h_flame);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        bool burn = k->boost_t > 0;
        if(!burn) continue;
        if(!r_sphere_visible(k->pos, 3.0f)) continue;
        V3 back = vscale(heading_fwd(k->heading + k->vis_yaw), -1.0f);
        float strength = clampf(k->boost_t / fmaxf(k->boost_len, 0.1f) * 1.4f, 0.3f, 1.0f);
        for(int e = 0; e < 2; e++) {
            V3 ex = models_exhaust_world(k, e);
            float len = (0.9f + 0.35f * frand()) * strength + 0.3f;
            V3 tail = vadd(vmad(ex, back, len), v3(0, 0.08f, 0));
            r_streak_ex(ex, tail, 0.20f, 0.06f, 0xffffffff, argb(255, 255, 120, 40), 0.02f, 0.98f);
        }
    }
    r_hdr(h_spark_glow);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->boost_t > 0) {
            for(int e = 0; e < 2; e++) {
                V3 ex = models_exhaust_world(k, e);
                r_billboard(ex, 0.45f, 0.45f, 0, 0, 0, 1, 1, argb(220, 255, 190, 90));
            }
        }
        /* Charge glow at the rear tyres while drifting. */
        if(k->drift == DRIFT_ACTIVE && k->grounded && r_sphere_visible(k->pos, 2.5f)) {
            uint32_t c = fx_stage_color(k->stage);
            float s = (k->stage == 0 ? 0.25f : 0.42f + 0.08f * k->stage) * (0.85f + 0.3f * frand());
            for(int w = 2; w < 4; w++) {
                V3 p = vadd(models_wheel_world(k, w), v3(0, 0.12f, 0));
                r_billboard(p, s, s, 0, 0, 0, 1, 1, color_alpha(c, 0.9f));
                r_billboard(p, s * 0.45f, s * 0.45f, 0, 0, 0, 1, 1, 0xd0ffffff);
            }
        }
    }
}

void fx_draw(void) {
    r_load_vp();
    /* Alpha puffs first, far to near (translucency is presorted). Puffs
       right in front of the lens would cover the screen: skip them, and cap
       their size, since translucent fill is the console's bottleneck. */
    static uint16_t order[MAX_PARTICLES];
    static float depth[MAX_PARTICLES];
    int n = 0;
    for(int i = 0; i < part_n; i++) {
        const Particle *q = &parts[i];
        if(q->type != P_SMOKE && q->type != P_DUST) continue;
        float z = r_view_depth(q->p);
        if(z < 3.5f) continue;
        int j = n++;
        while(j > 0 && depth[j - 1] < z) {
            order[j] = order[j - 1];
            depth[j] = depth[j - 1];
            j--;
        }
        order[j] = (uint16_t)i;
        depth[j] = z;
    }
    r_hdr(h_smoke);
    for(int k = 0; k < n; k++) {
        const Particle *q = &parts[order[k]];
        float t = 1.0f - q->life / q->max_life;
        float size = fminf(q->size, 1.4f);
        r_billboard(q->p, size, size, (float)order[k] * 0.7f, 0, 0, 1, 1, color_lerp(q->c0, q->c1, t));
    }
    draw_flames();
    r_hdr(h_spark_glow);
    for(int i = 0; i < part_n; i++) {
        const Particle *q = &parts[i];
        float t = 1.0f - q->life / q->max_life;
        uint32_t c = color_lerp(q->c0, q->c1, t);
        if(q->type == P_SPARK) {
            V3 tail = vmad(q->p, q->v, -0.035f);
            r_streak(q->p, tail, q->size, c, color_alpha(c, 0));
        }
        else if(q->type == P_GLOW) {
            r_billboard(q->p, q->size, q->size, 0, 0, 0, 1, 1, c);
        }
        else if(q->type == P_CONFETTI) {
            float w = q->size * (0.4f + 0.6f * fabsf(sinf(q->life * 9.0f + i)));
            r_billboard(q->p, w, q->size, q->life * 5.0f, 0.3f, 0.3f, 0.7f, 0.7f, c);
        }
    }
    r_hdr(h_star);
    for(int i = 0; i < part_n; i++) {
        const Particle *q = &parts[i];
        if(q->type != P_STAR) continue;
        float t = 1.0f - q->life / q->max_life;
        r_billboard(q->p, q->size, q->size, t * 2.0f, 0, 0, 1, 1, color_lerp(q->c0, q->c1, t));
    }
}
