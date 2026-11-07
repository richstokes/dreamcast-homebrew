/* Ridgeline effects: what the tyres throw up (dust, gravel, mud, snow,
   spray) and the exhaust's breath. Soft painted puffs, few and short-lived:
   on the console the PVR's fill rate, not the CPU, sets the budget. */
#include "fx.h"
#include "truck.h"
#include "terrain.h"

#define MAX_PARTS 140

typedef struct {
    V3 p, v;
    float life, max, size, grow;
    uint32_t col;
    float drag, gravity;
} Part;

static Part parts[MAX_PARTS];
static int part_n;
static const pvr_poly_hdr_t *h_puff;
static float emit_acc[MAX_TRUCKS][4];

void fx_init(void) {
    h_puff = r_header(TEX_PUFF, HF_TR);
    part_n = 0;
}

void fx_clear(void) {
    part_n = 0;
}

static Part *spawn(void) {
    if(part_n < MAX_PARTS) return &parts[part_n++];
    /* Full: replace the oldest-looking one. */
    int best = 0;
    for(int i = 1; i < MAX_PARTS; i++)
        if(parts[i].life / parts[i].max > parts[best].life / parts[best].max) best = i;
    return &parts[best];
}

void fx_splash(V3 p, float strength) {
    int n = 4 + (int)(strength * 6);
    for(int i = 0; i < n; i++) {
        Part *q = spawn();
        q->p = p;
        q->v = v3(frand_s() * 2.5f * strength, 2.0f + 3.0f * frand() * strength, frand_s() * 2.5f * strength);
        q->life = 0;
        q->max = 0.6f + 0.4f * frand();
        q->size = 0.35f;
        q->grow = 1.2f;
        q->col = 0xd8e8f4f8;
        q->drag = 0.6f;
        q->gravity = 9.0f;
    }
}

void fx_update(float dt) {
    /* Wheels: what each one throws, by surface, slip and speed. */
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        float dcam = vlen(vsub(t->pos, r_cam.pos));
        if(dcam > 90.0f) continue;
        for(int i = 0; i < 4; i++) {
            Wheel *w = &t->wh[i];
            if(!w->contact) continue;
            float v = fabsf(w->omega * t->spec.radius);
            float rate = w->slip * 9.0f + v * 0.9f;
            const SurfProps *sp = &surf_props[w->surf];
            bool wet = w->water > 0.08f;
            if(w->surf == SURF_ROCK && !wet) rate = w->slip * 2.0f;
            if(w->surf == SURF_WETROCK && !wet) rate *= 0.3f;
            if(dcam > 45.0f) rate *= 0.4f;
            emit_acc[ti][i] += rate * dt;
            while(emit_acc[ti][i] >= 1.0f) {
                emit_acc[ti][i] -= 1.0f;
                Part *q = spawn();
                V3 back = vscale(t->ax[2], -signf(w->omega) * (0.3f + 0.3f * frand()));
                q->p = vadd(w->cp, v3(frand_s() * 0.2f, 0.1f, frand_s() * 0.2f));
                q->life = 0;
                if(wet) {
                    q->v = vadd(vscale(back, 3.0f + v * 0.25f), v3(frand_s(), 1.5f + 2.0f * frand(), frand_s()));
                    q->max = 0.7f;
                    q->size = 0.3f;
                    q->grow = 1.4f;
                    q->col = 0xc0e4f0f4;
                    q->drag = 0.8f;
                    q->gravity = 8.0f;
                }
                else if(w->surf == SURF_MUD) {
                    q->v = vadd(vscale(back, 2.0f + w->slip * 0.8f), v3(frand_s() * 0.8f, 1.6f + 2.4f * frand(), frand_s() * 0.8f));
                    q->max = 0.9f;
                    q->size = 0.16f;
                    q->grow = 0.2f;
                    q->col = 0xff5a4230;
                    q->drag = 0.3f;
                    q->gravity = 9.0f;
                }
                else {
                    /* Dust: rises, spreads and hangs. */
                    q->v = vadd(vscale(back, 1.0f + v * 0.12f), v3(frand_s() * 0.6f, 0.5f + 0.8f * frand(), frand_s() * 0.6f));
                    q->max = 1.6f + 1.2f * frand();
                    q->size = 0.45f;
                    q->grow = 1.5f;
                    q->col = color_alpha(sp->dust, 0.42f);
                    q->drag = 1.4f;
                    q->gravity = -0.15f;
                }
            }
        }
        /* Entering water: a splash. */
        static float last_water[MAX_TRUCKS];
        if(t->water > 0.15f && last_water[ti] <= 0.15f && fabsf(t->speed) > 2.0f)
            fx_splash(vadd(t->pos, vscale(t->ax[2], 2.0f)), fminf(fabsf(t->speed) / 8.0f, 1.5f));
        last_water[ti] = t->water;
    }
    for(int i = 0; i < part_n;) {
        Part *q = &parts[i];
        q->life += dt;
        if(q->life >= q->max) {
            parts[i] = parts[--part_n];
            continue;
        }
        q->v.y -= q->gravity * dt;
        q->v = vscale(q->v, 1.0f - fminf(q->drag * dt, 0.5f));
        q->p = vmad(q->p, q->v, dt);
        float g = terrain_height(q->p.x, q->p.z);
        if(q->p.y < g && q->gravity > 0) {
            q->p.y = g;
            q->v = v3(q->v.x * 0.3f, 0, q->v.z * 0.3f);
        }
        i++;
    }
}

void fx_draw(void) {
    r_hdr(h_puff);
    for(int i = 0; i < part_n; i++) {
        const Part *q = &parts[i];
        float k = q->life / q->max;
        float a = (float)(q->col >> 24) / 255.0f * (1.0f - k) * fminf(1.0f, q->life * 10.0f);
        float s = q->size * (1.0f + q->grow * k);
        r_billboard(q->p, s, s, q->life * 0.7f, 0, 0, 1, 1, color_alpha(q->col, a));
    }
}
