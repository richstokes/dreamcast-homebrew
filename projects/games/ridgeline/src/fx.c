/* Ridgeline effects: what the tyres throw up (dust, gravel, mud, snow,
   spray), the roostertail a spinning tyre flings back, and the tracks the
   rear wheels leave in soft ground. Soft painted puffs are few and
   short-lived: on the console the PVR's fill rate, not the CPU, sets the
   budget. Clods are small and cheap, so a roostertail can be dense. */
#include "fx.h"
#include "world.h"
#include "truck.h"
#include "terrain.h"

#define MAX_PARTS 300

enum { P_PUFF, P_CLOD };

typedef struct {
    V3 p, v;
    float life, max, size, grow, spin, rot;
    uint32_t col;
    float drag, gravity;
    uint8_t kind, bounce;
} Part;

static Part parts[MAX_PARTS];
static int part_n;
static const pvr_poly_hdr_t *h_puff, *h_clod, *h_tread;
static float emit_acc[MAX_TRUCKS][4], rooster_acc[MAX_TRUCKS][4];

/* ---- Tracks ----
   Each truck's rear wheels lay a strip of tread prints on soft ground:
   a ring of points per wheel, the player's longest. A run breaks where the
   ground turns hard, the wheel leaves it, or the truck jumps (a recover). */
#define TRK_PLAYER 448
#define TRK_CPU 128
#define TRK_STEP 0.5f          /* metres between points, at least */
#define TRK_LONG 1.6f          /* ...and at most, on the straight */
#define TRK_LIFE 150.0f        /* seconds before a print has gone */
#define TRK_DRAW 35.0f         /* metres from the camera */

typedef struct {
    V3 l, r;
    float v, born;
    uint32_t col;
    uint16_t run;
} TrkPt;

typedef struct {
    TrkPt *pt;
    int cap, head, count;
    bool live;
    V3 last, dir;
    uint32_t col;
    float v;
    uint16_t run;
} TrkSlot;

static TrkPt trk_pool[TRK_PLAYER * 2 + TRK_CPU * 2 * (MAX_TRUCKS - 1)];
static TrkSlot trk[MAX_TRUCKS][2];
static float fx_clock;

/* What a tyre prints in each surface (none on rock): colour and how dark. */
static uint32_t track_col(int surf) {
    switch(surf) {
        case SURF_MUD: return 0xe0201006;
        case SURF_DIRT: return 0xa04a3420;
        case SURF_GRASS: return 0x904a5226;
        case SURF_FOREST: return 0xa0302414;
        case SURF_GRAVEL: return 0x7058544a;
        case SURF_SCREE: return 0x705c564c;
        case SURF_SNOW: return 0xb0707c94;
        default: return 0;
    }
}

/* What a spinning tyre flings back: two colours of clod, how many a second
   at full spin, how fast for the spin, how big, whether they bounce (stones)
   and a haze to go with them (dust, mist, powder). */
typedef struct {
    uint32_t col[2];
    float rate, speed, size;
    uint8_t bounce;
    uint32_t haze;
} Rooster;

static const Rooster rooster[SURF_COUNT] = {
    [SURF_GRASS] = {{0xff58582c, 0xff6c5434}, 26, 0.50f, 0.09f, 0, 0},
    [SURF_FOREST] = {{0xff4c3e2c, 0xff5c4a32}, 26, 0.50f, 0.09f, 0, 0},
    [SURF_DIRT] = {{0xff785a3a, 0xff8a6844}, 32, 0.55f, 0.08f, 0, 0},
    [SURF_GRAVEL] = {{0xff8c8880, 0xffa29a8e}, 30, 0.65f, 0.055f, 1, 0},
    [SURF_MUD] = {{0xff3c2a1c, 0xff523e28}, 60, 0.60f, 0.11f, 0, 0x806a5440},
    [SURF_SCREE] = {{0xff88827a, 0xff9c948a}, 30, 0.60f, 0.07f, 1, 0},
    [SURF_SNOW] = {{0xffe6ecf2, 0xfff6f8fa}, 45, 0.50f, 0.08f, 0, 0x90f0f4f8},
};

void fx_init(void) {
    h_puff = r_header(TEX_PUFF, HF_TR);
    h_clod = r_header(TEX_CLOD, HF_TR);
    h_tread = r_header(TEX_TREAD, HF_TR);
    TrkPt *p = trk_pool;
    for(int ti = 0; ti < MAX_TRUCKS; ti++)
        for(int s = 0; s < 2; s++) {
            int cap = ti == 0 ? TRK_PLAYER : TRK_CPU;
            trk[ti][s] = (TrkSlot){.pt = p, .cap = cap};
            p += cap;
        }
    fx_clear();
}

void fx_clear(void) {
    part_n = 0;
    for(int ti = 0; ti < MAX_TRUCKS; ti++)
        for(int s = 0; s < 2; s++) {
            trk[ti][s].count = trk[ti][s].head = 0;
            trk[ti][s].live = false;
        }
}

static Part *spawn(void) {
    if(part_n < MAX_PARTS) return &parts[part_n++];
    /* Full: replace the oldest-looking one. */
    int best = 0;
    for(int i = 1; i < MAX_PARTS; i++)
        if(parts[i].life / parts[i].max > parts[best].life / parts[best].max) best = i;
    return &parts[best];
}

/* Soft puffs are what costs on the console: big translucent quads the PVR
   has to sort and blend. A spinning tyre flinging haze at the chase camera
   once took the render past 30 ms, so they have a hard cap. */
#define MAX_PUFFS 36
static int puff_n;

static void puff(V3 p, V3 v, float max, float size, float grow, uint32_t col, float drag, float gravity) {
    if(puff_n >= MAX_PUFFS) return;
    puff_n++;
    Part *q = spawn();
    *q = (Part){.p = p, .v = v, .max = max, .size = size, .grow = grow, .col = col, .drag = drag, .gravity = gravity,
                .kind = P_PUFF};
}

void fx_splash(V3 p, float strength) {
    int n = 4 + (int)(strength * 6);
    for(int i = 0; i < n; i++)
        puff(p, v3(frand_s() * 2.5f * strength, 2.0f + 3.0f * frand() * strength, frand_s() * 2.5f * strength),
             0.6f + 0.4f * frand(), 0.35f, 1.2f, 0xd8e8f4f8, 0.6f, 9.0f);
}

static void track_add(TrkSlot *s, V3 p, V3 dir, float half, uint32_t col) {
    V3 side = v3(dir.z * half, 0, -dir.x * half);
    TrkPt *q = &s->pt[s->head];
    q->l = vsub(p, side);
    q->r = vadd(p, side);
    q->l.y = terrain_height(q->l.x, q->l.z) + 0.035f;
    q->r.y = terrain_height(q->r.x, q->r.z) + 0.035f;
    q->v = s->v;
    q->born = fx_clock;
    q->col = col;
    q->run = s->run;
    s->head = (s->head + 1) % s->cap;
    if(s->count < s->cap) s->count++;
    s->last = p;
    s->dir = dir;
    s->col = col;
}

static void tracks_update(int ti, const Truck *t) {
    for(int k = 0; k < 2; k++) {
        const Wheel *w = &t->wh[2 + k];
        TrkSlot *s = &trk[ti][k];
        uint32_t col = w->contact && w->water < 0.08f ? track_col(w->surf) : 0;
        /* CPU trucks print only where it shows most: mud and snow. On the
           trails they follow each other and their prints pile up unseen. */
        if(ti != 0 && w->surf != SURF_MUD && w->surf != SURF_SNOW) col = 0;
        if(!col) {
            s->live = false;
            continue;
        }
        /* Dug in: a spinning tyre leaves a darker print. */
        float dig = clampf((w->slip - 1.5f) / 5.0f, 0, 1);
        col = color_alpha(col, fminf(1.0f, (float)(col >> 24) / 255.0f * (1.0f + 0.5f * dig)));
        V3 p = w->cp, d = vsub(p, s->last);
        d.y = 0;
        float dist = vlen(d);
        float half = t->spec.width * 0.55f;
        if(!s->live || dist > 2.5f) {
            V3 f = t->ax[2];
            f.y = 0;
            s->run++;
            s->v = 0;
            s->live = true;
            track_add(s, p, vnorm(f), half, col);
        }
        else if(dist >= TRK_STEP) {
            /* A point where the track bends, the ground under it bends or
               the print changes; on the straight, every TRK_LONG. */
            V3 dn = vscale(d, 1.0f / dist);
            float hm = terrain_height((p.x + s->last.x) * 0.5f, (p.z + s->last.z) * 0.5f);
            float he = fabsf(hm - (terrain_height(p.x, p.z) + terrain_height(s->last.x, s->last.z)) * 0.5f);
            int da = (int)(col >> 24) - (int)(s->col >> 24);
            bool recol = (col & 0xffffff) != (s->col & 0xffffff) || da > 24 || da < -24;
            if(dist >= TRK_LONG || vdot(dn, s->dir) < 0.9986f || he > 0.02f || recol) {
                s->v += dist;
                track_add(s, p, dn, half, col);
            }
        }
    }
}

static void rooster_update(int ti, const Truck *t, int i, float dcam, float dt) {
    const Wheel *w = &t->wh[i];
    float r = t->spec.radius, rim = w->omega * r;
    float ground = vdot(t->vel, t->ax[2]);
    float spin = rim - ground;
    /* Only a tyre turning faster than the ground moves under it throws
       anything back: a locked, sliding one doesn't. */
    if(fabsf(rim) < 1.0f || spin * rim <= 0) {
        rooster_acc[ti][i] = 0;
        return;
    }
    float k = clampf((fabsf(spin) - 1.5f) / 5.0f, 0, 1);
    if(k <= 0) return;
    bool wet = w->water > 0.08f;
    const Rooster *rs = &rooster[w->surf];
    if(!wet && rs->rate <= 0) return;
    float rate = (wet ? 50.0f : rs->rate) * k;
    if(dcam > 30.0f) rate *= 0.5f;
    rooster_acc[ti][i] += rate * dt;
    V3 back = vscale(t->ax[2], rim > 0 ? -1.0f : 1.0f), up = t->ax[1];
    float out = (i & 1) ? 1.0f : -1.0f;
    while(rooster_acc[ti][i] >= 1.0f) {
        rooster_acc[ti][i] -= 1.0f;
        float thr = fabsf(spin) * (wet ? 0.7f : rs->speed) * (0.7f + 0.5f * frand());
        float el = 0.45f + 0.65f * frand();     /* 25 to 63 degrees up */
        V3 v = vadd(t->vel, vadd(vscale(back, thr * cosf(el)), vscale(up, thr * sinf(el))));
        v = vmad(v, t->ax[0], thr * (frand_s() * 0.18f + out * 0.08f));
        V3 p = vadd(w->cp, vadd(vscale(back, r * 0.7f), vscale(up, 0.15f)));
        p = vmad(p, t->ax[0], frand_s() * t->spec.width * 0.4f);
        if(wet) {
            puff(p, v, 0.8f, 0.16f, 1.6f, 0xb8e0ecf4, 0.5f, 9.0f);
            continue;
        }
        Part *q = spawn();
        *q = (Part){.p = p, .v = v, .max = 1.2f, .size = rs->size * (0.7f + 0.6f * frand()), .spin = frand_s() * 9.0f,
                    .col = rs->col[frand() < 0.5f], .drag = 0.12f, .gravity = 9.8f, .kind = P_CLOD, .bounce = rs->bounce};
        if(rs->haze && frand() < 0.1f)
            puff(p, vscale(v, 0.5f), 0.7f + 0.4f * frand(), 0.28f, 1.3f, rs->haze, 1.6f, 1.0f);
    }
}

void fx_update(float dt) {
    fx_clock += dt;
    /* Wheels: what each one throws, by surface, slip and speed. */
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        float dcam = vlen(vsub(t->pos, r_cam.pos));
        if(dcam > 90.0f) continue;
        tracks_update(ti, t);
        for(int i = 0; i < 4; i++) {
            Wheel *w = &t->wh[i];
            if(!w->contact) continue;
            rooster_update(ti, t, i, dcam, dt);
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
                V3 back = vscale(t->ax[2], -signf(w->omega) * (0.3f + 0.3f * frand()));
                V3 p = vadd(w->cp, v3(frand_s() * 0.2f, 0.1f, frand_s() * 0.2f));
                if(wet)
                    puff(p, vadd(vscale(back, 3.0f + v * 0.25f), v3(frand_s(), 1.5f + 2.0f * frand(), frand_s())), 0.7f,
                         0.3f, 1.4f, 0xc0e4f0f4, 0.8f, 8.0f);
                else if(w->surf == SURF_MUD) {
                    /* Rolling through mud: lumps off the tread. */
                    Part *q = spawn();
                    *q = (Part){.p = p,
                                .v = vadd(vscale(back, 2.0f + w->slip * 0.8f),
                                          v3(frand_s() * 0.8f, 1.6f + 2.4f * frand(), frand_s() * 0.8f)),
                                .max = 0.9f, .size = 0.09f + 0.05f * frand(), .spin = frand_s() * 6.0f,
                                .col = rooster[SURF_MUD].col[frand() < 0.5f], .drag = 0.3f, .gravity = 9.0f, .kind = P_CLOD};
                }
                else
                    /* Dust: rises, spreads and hangs. */
                    puff(p, vadd(vscale(back, 1.0f + v * 0.12f), v3(frand_s() * 0.6f, 0.5f + 0.8f * frand(), frand_s() * 0.6f)),
                         1.6f + 1.2f * frand(), 0.45f, 1.5f, color_alpha(sp->dust, 0.42f), 1.4f, -0.15f);
            }
        }
        /* Entering water: a splash. */
        static float last_water[MAX_TRUCKS];
        if(t->water > 0.15f && last_water[ti] <= 0.15f && fabsf(t->speed) > 2.0f)
            fx_splash(vadd(t->pos, vscale(t->ax[2], 2.0f)), fminf(fabsf(t->speed) / 8.0f, 1.5f));
        last_water[ti] = t->water;
    }
    puff_n = 0;
    for(int i = 0; i < part_n;) {
        Part *q = &parts[i];
        q->life += dt;
        if(q->life >= q->max) {
            parts[i] = parts[--part_n];
            continue;
        }
        q->rot += q->spin * dt;
        q->v.y -= q->gravity * dt;
        q->v = vscale(q->v, 1.0f - fminf(q->drag * dt, 0.5f));
        q->p = vmad(q->p, q->v, dt);
        if(q->gravity > 0) {
            float g = terrain_height(q->p.x, q->p.z);
            if(q->p.y < g) {
                q->p.y = g;
                if(q->kind == P_CLOD && q->bounce && q->v.y < -1.5f) {
                    /* Stones skip once or twice. */
                    q->v = v3(q->v.x * 0.5f, -q->v.y * 0.3f, q->v.z * 0.5f);
                }
                else {
                    q->v = v3(q->v.x * 0.3f, 0, q->v.z * 0.3f);
                    q->spin = 0;
                    /* A clod that has landed lies there a moment, then goes. */
                    if(q->kind == P_CLOD && q->max - q->life > 0.4f) q->max = q->life + 0.4f;
                }
            }
        }
        puff_n += q->kind == P_PUFF;
        i++;
    }
}

/* Tracks go out in short strips, each with a bounding sphere, so the
   stretches behind or beside the camera are dropped whole rather than
   transformed vertex by vertex. */
#define TRK_CHUNK 16

typedef struct {
    RV v[TRK_CHUNK * 2];
    int n;
    V3 lo, hi;
} TrkBuf;

static void chunk_flush(TrkBuf *b) {
    if(b->n >= 4) {
        V3 c = vscale(vadd(b->lo, b->hi), 0.5f);
        r_strip_bounded(b->v, b->n, c, vlen(vsub(b->hi, b->lo)) * 0.5f + 0.1f);
    }
    b->n = 0;
}

static void chunk_add(TrkBuf *b, const RV *a) {
    V3 p = v3(a->x, a->y, a->z);
    if(b->n == 0) b->lo = b->hi = p;
    else {
        b->lo = v3(fminf(b->lo.x, p.x), fminf(b->lo.y, p.y), fminf(b->lo.z, p.z));
        b->hi = v3(fmaxf(b->hi.x, p.x), fmaxf(b->hi.y, p.y), fmaxf(b->hi.z, p.z));
    }
    b->v[b->n++] = *a;
}

static void tracks_draw(void) {
    static TrkBuf b;
    V3 cam = r_cam.pos;
    r_hdr(h_tread);
    for(int ti = 0; ti < MAX_TRUCKS; ti++)
        for(int k = 0; k < 2; k++) {
            const TrkSlot *s = &trk[ti][k];
            int run = -1;
            b.n = 0;
            int idx = s->head - s->count;
            if(idx < 0) idx += s->cap;
            for(int j = 0; j < s->count; j++, idx = idx + 1 == s->cap ? 0 : idx + 1) {
                const TrkPt *q = &s->pt[idx];
                float age = fx_clock - q->born;
                float dx = q->l.x - cam.x, dz = q->l.z - cam.z;
                float dist2 = dx * dx + dz * dz;
                bool gone = age >= TRK_LIFE || dist2 >= TRK_DRAW * TRK_DRAW;
                if(gone || q->run != run) {
                    chunk_flush(&b);
                    run = -1;
                    if(gone) continue;
                    run = q->run;
                }
                else if(b.n >= TRK_CHUNK * 2) {
                    /* A full strip: the next starts from its last pair. */
                    RV l = b.v[b.n - 2], r = b.v[b.n - 1];
                    chunk_flush(&b);
                    chunk_add(&b, &l);
                    chunk_add(&b, &r);
                }
                /* Fading out with age, and toward the draw distance (by its
                   square: no root per point). */
                float a = (float)(q->col >> 24) * (1.0f / 255.0f) * fminf((TRK_LIFE - age) * (1.0f / 40.0f), 1.0f) *
                          fminf((TRK_DRAW * TRK_DRAW - dist2) * (1.0f / (TRK_DRAW * TRK_DRAW * 0.45f)), 1.0f);
                uint32_t c = color_alpha(q->col, a);
                float v = q->v * (1.0f / 1.1f);
                chunk_add(&b, &(RV){q->l.x, q->l.y, q->l.z, 0, v, c});
                chunk_add(&b, &(RV){q->r.x, q->r.y, q->r.z, 1, v, c});
            }
            chunk_flush(&b);
        }
}

void fx_draw(void) {
    tracks_draw();
    /* What the tyres throw is lit by the day too: dim it at night. */
    float dim = 1.0f - 0.65f * pal->dark;
    r_hdr(h_puff);
    for(int i = 0; i < part_n; i++) {
        const Part *q = &parts[i];
        if(q->kind != P_PUFF) continue;
        float k = q->life / q->max;
        float a = (float)(q->col >> 24) / 255.0f * (1.0f - k) * fminf(1.0f, q->life * 10.0f);
        /* Close to the camera one puff covers half the screen: fade it. */
        V3 dc = vsub(q->p, r_cam.pos);
        float d2 = vdot(dc, dc);
        if(d2 < 25.0f) a *= clampf((sqrtf(d2) - 2.0f) * (1.0f / 3.0f), 0, 1);
        if(a < 0.02f) continue;
        float s = q->size * (1.0f + q->grow * k);
        r_billboard(q->p, s, s, q->life * 0.7f, 0, 0, 1, 1, color_alpha(color_scale(q->col, dim), a));
    }
    r_hdr(h_clod);
    for(int i = 0; i < part_n; i++) {
        const Part *q = &parts[i];
        if(q->kind != P_CLOD) continue;
        float a = clampf((q->max - q->life) / 0.3f, 0, 1);
        r_billboard(q->p, q->size, q->size, q->rot, 0, 0, 1, 1, color_alpha(color_scale(q->col, dim), a));
    }
}
