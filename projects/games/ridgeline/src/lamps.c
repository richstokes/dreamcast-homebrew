/* Ridgeline: the trucks' headlights on the trees, rocks and props
   (lamps.h).

   The scenery's colours are baked (world.c) and relit only by the clock,
   so a beam is added on to the vertices it reaches just before they are
   drawn and taken off again straight after: no extra geometry, and the
   clock's relighting never sees it. Each near tree, boulder, log and prop
   is a range of pool vertices with a bounding sphere (a tree a few: its
   trunk and each tier), bucketed by 24 m cell, so a frame tests the cells
   along a beam, the spheres in those, and lights only what is in it.

   The beam is a cone with an elliptical section, wider than it is tall,
   pitched down as the beams on the ground are (truckdraw.c), soft to its
   edge and fading out by its range. It shows only in the dark.

   Cost (console, 2026-11-09): up to ~0.35 ms a frame, 0.5-0.6 ms with
   four lit trucks together in the forest (README, Performance). If that
   needs to come down, LAMPS 1 (the player's truck only) is the first
   thing to try. The grass tufts and the other trucks take the same beams
   (lamps_light): a tuft as the ground under it, a truck on its side
   toward the lamp.

   The campfire and the lookout's lanterns (glow.h) light what stands
   round them the same way. They never move, so each vertex in their
   reach has its share worked out once, when the world is built; a frame only scales that by the flicker
   and adds it on. Near trees and rocks swap for far versions that keep
   their colours (world.c), so their firelight fades out before the swap. */
#include "lamps.h"
#include "glow.h"
#include "world.h"
#include "tod.h"
#include "truck.h"
#include "terrain.h"
#include "engine/prof.h"
#include <sh4zam/shz_sh4zam.h>

#define LCELL 24.0f
#define LCELLS 32
#define OBJ_MAX 6400        /* a near tree is a few: its trunk and each tier */
#define LAMPS 3             /* trucks lit at once: the player's, then the nearest */
#define SAVE_MAX 8192       /* vertices relit in a frame, at most */
#define LAMP_NEAR 70.0f     /* other trucks further from the camera light nothing */
#define OBJ_VERTS 1024      /* an object's vertices lit, at most */
#define DRAW_NEAR 90.0f     /* the near trees' and rocks' range (world.c) */

typedef struct {
    float x, y, z, r;
    uint32_t first_n;       /* first pool vertex (low 20 bits), count (high 12) */
} LObj;
_Static_assert(sizeof(LObj) == 20, "LObj packing");
static LObj obj[OBJ_MAX];
static int nobj;
static uint16_t order[OBJ_MAX], cell_first[LCELLS * LCELLS + 1];

typedef struct {
    V3 o, f, r, u;          /* where it shines from, along, across, up (all unit) */
    float th, tv, range;    /* the spread across and up (tangents), metres */
    float ih2, iv2, irange, k;
    float fhx, fhz;         /* along, flat (unit) */
    int truck;              /* whose */
} Lamp;
/* This frame's beams (lamps_begin), for the grass and the trucks too. */
static Lamp lamps[LAMPS];
static int nl;

static struct {
    uint32_t k, c;
} saved[SAVE_MAX];
static int nsaved;
static uint16_t cand[OBJ_MAX];
static uint8_t cand_mask[OBJ_MAX];
uint64_t lamps_us;   /* time spent lighting, for the autotest's split */

/* The campfire's objects and their vertices: which pool vertex, and the
   colour full firelight adds to it. */
#define GLOW_LIT_FAR 260.0f  /* the warm lights' light on the scenery is gone by here */
#define FIRE_OBJS 192
#define FIRE_VERTS 4096
#define FIRE_FADE 14.0f     /* metres short of the swap the firelight starts to go */
typedef struct {
    V3 lod;                 /* where its chunk measures the camera's distance from */
    float far;              /* ...and how far that draws it (0: always) */
    uint16_t obj, first, n; /* the object; its run of fire_v */
    uint8_t group;          /* whose light (glow.h) */
} FObj;
static FObj fobj[FIRE_OBJS];
static struct {
    uint32_t k;
    uint8_t r, g, b, pad;
} fire_v[FIRE_VERTS];
static int nfobj, nfire_v;
static V3 lod_p;
static float lod_far;

void lamps_clear(void) {
    nobj = nfobj = nfire_v = 0;
    lod_far = 0;
}

void lamps_lod(V3 p, float far) {
    lod_p = p;
    lod_far = far;
}

void lamps_add(uint32_t first, uint32_t end) {
    if(nobj >= OBJ_MAX || end <= first) return;
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for(uint32_t k = first; k < end; k++) {
        const RV *v = &scenery.pool[k];
        lo = v3(fminf(lo.x, v->x), fminf(lo.y, v->y), fminf(lo.z, v->z));
        hi = v3(fmaxf(hi.x, v->x), fmaxf(hi.y, v->y), fmaxf(hi.z, v->z));
    }
    LObj *o = &obj[nobj++];
    o->x = (lo.x + hi.x) * 0.5f;
    o->y = (lo.y + hi.y) * 0.5f;
    o->z = (lo.z + hi.z) * 0.5f;
    o->r = vlen(vsub(hi, lo)) * 0.5f;
    uint32_t n = end - first;
    if(n > 4095 || first >= (1u << 20)) {
        nobj--;
        return;
    }
    o->first_n = first | n << 20;
    /* Once for each group of warm lights that reaches it. */
    for(int g = 0; g < GLOW_GROUPS; g++)
        for(int i = 0; i < glow_n; i++) {
            if(glows[i].group != g) continue;
            V3 d = vsub(v3(o->x, o->y, o->z), glows[i].p);
            float reach = glows[i].reach + o->r;
            if(vdot(d, d) >= reach * reach) continue;
            if(nfobj < FIRE_OBJS) fobj[nfobj++] = (FObj){lod_p, lod_far, (uint16_t)(nobj - 1), 0, 0, (uint8_t)g};
            break;
        }
}

/* Each fire object's vertices and what the fire adds to them: by how
   near they are and how they face it, in the fire's orange on the
   vertex's own colour. */
static void fire_index(void) {
    if(!world_lrec) return;
    const LightRec *rec = world_lrec - world_lrec_first;
    for(int i = 0; i < nfobj; i++) {
        FObj *fo = &fobj[i];
        const LObj *o = &obj[fo->obj];
        uint32_t first = o->first_n & 0xfffff, n = o->first_n >> 20;
        fo->first = (uint16_t)nfire_v;
        for(uint32_t k = first; k < first + n && nfire_v < FIRE_VERTS; k++) {
            const RV *v = &scenery.pool[k];
            LightRec lr = rec[k];
            float nx = lr.nx * (1.0f / 127.0f), nz = lr.nz * (1.0f / 127.0f);
            float ny = sqrtf(fmaxf(1.0f - nx * nx - nz * nz, 0.0f));
            if(lr.alb & 0x40) ny = -ny;
            float w = 0;
            for(int gi = 0; gi < glow_n; gi++) {
                const Glow *gl = &glows[gi];
                if(gl->group != fo->group) continue;
                V3 d = vsub(gl->p, v3(v->x, v->y, v->z));
                float d2 = vdot(d, d), fall = glow_falloff(gl, d2) * 1.7f;
                if(fall < 0.01f) continue;
                /* A colour used as is has no normal: the tent's inside. */
                float lam = lr.alb & 0x80 ? 0.6f : 0.3f + 0.7f * (nx * d.x + ny * d.y + nz * d.z) / sqrtf(fmaxf(d2, 1e-4f));
                if(lam > 0) w += fall * lam;
            }
            if(w < 0.01f) continue;
            uint32_t alb = world_alb[lr.alb & 0x3f];
            int r = (int)((((alb >> 16) & 255) * 0.9f + 36.0f) * w);
            int g = (int)((((alb >> 8) & 255) * 0.9f + 30.0f) * w * 0.55f);
            int b = (int)(((alb & 255) * 0.9f + 20.0f) * w * 0.2f);
            if(r < 3) continue;
            fire_v[nfire_v].k = k;
            fire_v[nfire_v].r = (uint8_t)(r > 255 ? 255 : r);
            fire_v[nfire_v].g = (uint8_t)(g > 255 ? 255 : g);
            fire_v[nfire_v].b = (uint8_t)(b > 255 ? 255 : b);
            nfire_v++;
        }
        fo->n = (uint16_t)(nfire_v - fo->first);
    }
}

/* The fire's light on to its vertices, saving what they were. */
static void fire_light(void) {
    if(!nfire_v) return;
    float level[GLOW_GROUPS];
    bool any = false;
    for(int g = 0; g < GLOW_GROUPS; g++) {
        level[g] = glow_level[g] >= 0.03f && r_sphere_visible(glow_c[g], glow_r[g]) ? glow_level[g] : 0;
        if(level[g] > 0) {
            /* From across the valley a fire is its glow (glow.c holds
               that a few pixels wide); what it lights round it is too
               small to see, and a camp of them is thousands of
               vertices. */
            float d = vlen(vsub(glow_c[g], r_cam.pos));
            level[g] *= clampf((GLOW_LIT_FAR - d) * (1.0f / 40.0f), 0, 1);
        }
        any |= level[g] > 0;
    }
    if(!any) return;
    RV *pool = scenery.pool;
    for(int i = 0; i < nfobj; i++) {
        const FObj *fo = &fobj[i];
        float k = level[fo->group];
        if(k <= 0) continue;
        if(fo->far > 0) {
            float d = vlen(vsub(fo->lod, r_cam.pos));
            if(d >= fo->far) continue;
            k *= fminf((fo->far - d) * (1.0f / FIRE_FADE), 1.0f);
        }
        int s = (int)(k * 256.0f);
        if(s < 4 || nsaved + fo->n > SAVE_MAX) continue;
        for(int j = fo->first; j < fo->first + fo->n; j++) {
            RV *pv = &pool[fire_v[j].k];
            uint32_t c = pv->c;
            saved[nsaved].k = fire_v[j].k;
            saved[nsaved].c = c;
            nsaved++;
            uint32_t r = ((c >> 16) & 255) + ((fire_v[j].r * s) >> 8);
            uint32_t g = ((c >> 8) & 255) + ((fire_v[j].g * s) >> 8);
            uint32_t b = (c & 255) + ((fire_v[j].b * s) >> 8);
            pv->c = 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
}

static int cell_of(float v) {
    int c = (int)((v - LEVEL_ORG) / LCELL);
    return c < 0 ? 0 : c >= LCELLS ? LCELLS - 1 : c;
}

void lamps_index(void) {
    static uint16_t cnt[LCELLS * LCELLS];
    memset(cnt, 0, sizeof(cnt));
    for(int i = 0; i < nobj; i++) cnt[cell_of(obj[i].z) * LCELLS + cell_of(obj[i].x)]++;
    cell_first[0] = 0;
    for(int c = 0; c < LCELLS * LCELLS; c++) cell_first[c + 1] = cell_first[c] + cnt[c];
    memset(cnt, 0, sizeof(cnt));
    for(int i = 0; i < nobj; i++) {
        int c = cell_of(obj[i].z) * LCELLS + cell_of(obj[i].x);
        order[cell_first[c] + cnt[c]++] = (uint16_t)i;
    }
    fire_index();
    printf("Ridgeline lamps: %d objects, %d by the fire and lanterns (%d vertices)\n", nobj, nfobj, nfire_v);
}

/* Could anything of object o be in lamp l's beam? */
static bool obj_in(const LObj *o, const Lamp *l) {
    V3 d = vsub(v3(o->x, o->y, o->z), l->o);
    float a = vdot(d, l->f);
    if(a + o->r < 0.3f || a - o->r > l->range) return false;
    float am = a + o->r;
    if(fabsf(vdot(d, l->r)) - o->r > am * l->th) return false;
    float h = vdot(d, l->u);
    return h - o->r < am * l->tv && h + o->r > -am * l->tv * 2.0f;
}

/* Light object o's vertices with the lamps in mask, saving what they
   were. Lamp by lamp, so each one's numbers stay in registers across the
   vertices; most vertices are out of the beam and cost only the test. */
static void obj_light(const LObj *o, const Lamp *lamps, int mask) {
    static float sum[OBJ_VERTS];
    RV *pool = scenery.pool;
    const LightRec *rec = world_lrec - world_lrec_first;
    uint32_t first = o->first_n & 0xfffff;
    int n = (int)(o->first_n >> 20);
    if(n > OBJ_VERTS) n = OBJ_VERTS;
    const RV *v = pool + first;
    bool any = false;
    for(int i = 0; i < n; i++) sum[i] = 0;
    for(int li = 0; li < LAMPS; li++) {
        if(!(mask & (1 << li))) continue;
        const Lamp *l = &lamps[li];
        const float ox = l->o.x, oy = l->o.y, oz = l->o.z, fx = l->f.x, fy = l->f.y, fz = l->f.z;
        const float rx = l->r.x, ry = l->r.y, rz = l->r.z, ux = l->u.x, uy = l->u.y, uz = l->u.z;
        const float range = l->range, ih2 = l->ih2, iv2 = l->iv2, ir = l->irange, k = l->k;
        for(int i = 0; i < n; i++) {
            float dx = v[i].x - ox, dy = v[i].y - oy, dz = v[i].z - oz;
            float a = dx * fx + dy * fy + dz * fz;
            if(a < 0.3f || a > range) continue;
            float s = dx * rx + dy * ry + dz * rz;
            float h = dx * ux + dy * uy + dz * uz;
            /* Below the axis the beam spreads on down to the ground. */
            float e = s * s * ih2 + h * h * (h > 0 ? iv2 : iv2 * 0.25f);
            float a2 = a * a;
            if(e >= a2) continue;
            LightRec lr = rec[first + i];
            float nx = lr.nx * (1.0f / 127.0f), nz = lr.nz * (1.0f / 127.0f);
            float ny2 = fmaxf(1.0f - nx * nx - nz * nz, 1e-4f);
            float ny = ny2 * shz_inv_sqrtf_fsrra(ny2);
            if(lr.alb & 0x40) ny = -ny;
            /* Facing the lamp, wrapped a little round the sides (fsrra, not
               a divide: a is at least 0.3 m, so nothing here is zero). */
            float ndl = -(nx * dx + ny * dy + nz * dz) * shz_inv_sqrtf_fsrra(dx * dx + dy * dy + dz * dz);
            float lam = 0.25f + 0.75f * ndl;
            if(lam <= 0) continue;
            float ia = shz_inv_sqrtf_fsrra(a2), x = a * ir;
            sum[i] += (1.0f - e * ia * ia) * (1.0f - x * x) * lam * k;
            any = true;
        }
    }
    if(!any) return;
    for(int i = 0; i < n; i++) {
        float t = sum[i];
        if(t < 0.02f) continue;
        LightRec lr = rec[first + i];
        if(lr.alb & 0x80) continue;   /* a colour used as is: the tent's dark inside */
        if(nsaved >= SAVE_MAX) return;
        RV *pv = &pool[first + i];
        uint32_t c = pv->c;
        saved[nsaved].k = first + i;
        saved[nsaved].c = c;
        nsaved++;
        /* The lamps' warm white on the vertex's own colour, lifted a
           little so dark green firs still show it. */
        uint32_t alb = world_alb[lr.alb & 0x3f];
        int r = (int)((c >> 16) & 255) + (int)((((alb >> 16) & 255) * 0.8f + 40.0f) * t);
        int g = (int)((c >> 8) & 255) + (int)((((alb >> 8) & 255) * 0.8f + 38.0f) * t);
        int b = (int)(c & 255) + (int)(((alb & 255) * 0.8f + 30.0f) * t * 0.85f);
        pv->c = 0xff000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 |
                (uint32_t)(b > 255 ? 255 : b);
    }
}

void lamps_begin(bool on_screen) {
    nsaved = 0;
    nl = 0;
    if(!world_lrec) return;
    PROF_BEGIN(pl);
    uint64_t t0 = timer_us_gettime64();
    fire_light();
    /* By day a headlight is lost in the sun. */
    float dark = clampf(pal->dark * 1.1f + 0.4f * world_mist(), 0, 1);
    if(!on_screen || dark < 0.05f) {
        PROF_LAP(PS_LAMPS, pl);
        lamps_us += timer_us_gettime64() - t0;
        return;
    }
    float ldist[LAMPS];
    for(int ti = 0; ti < game.ntrucks; ti++) {
        const Truck *t = &trucks[ti];
        if(t->absent || !t->lights) continue;
        /* The player's truck always; others near the camera, nearest first. */
        float d = ti == 0 ? 0 : vlen(vsub(t->pos, r_cam.pos));
        if(d > LAMP_NEAR) continue;
        int slot = nl < LAMPS ? nl++ : -1;
        if(slot < 0) {
            /* Full: replace the furthest, if this is nearer. */
            int far = 0;
            for(int i = 1; i < LAMPS; i++)
                if(ldist[i] > ldist[far]) far = i;
            if(ldist[far] <= d) continue;
            slot = far;
        }
        bool full = t->lights == LIGHTS_FULL;
        V3 R = t->ax[0], U = t->ax[1], F = t->ax[2];
        /* The nose, as the beams on the ground start from (truckdraw.c). */
        V3 nose = vmad(t->pos, F, t->spec.body_hi[2]);
        nose.y += 0.4f;
        float pitch = (full ? 2.5f : 6.0f) * ENG_PI / 180.0f, cp = cosf(pitch), sp = sinf(pitch);
        Lamp *l = &lamps[slot];
        l->o = vsub(nose, vscale(F, 0.6f));   /* a little back, so the bumper's sides are in it */
        l->f = vsub(vscale(F, cp), vscale(U, sp));
        l->r = R;
        l->u = vadd(vscale(U, cp), vscale(F, sp));
        l->th = full ? 0.42f : 0.55f;
        l->tv = full ? 0.34f : 0.28f;
        l->range = full ? 48.0f : 26.0f;
        l->ih2 = 1.0f / (l->th * l->th);
        l->iv2 = 1.0f / (l->tv * l->tv);
        l->irange = 1.0f / l->range;
        l->k = dark * (full ? 1.3f : 1.05f);
        float fl = sqrtf(l->f.x * l->f.x + l->f.z * l->f.z);
        if(fl < 0.1f) fl = 0.1f;
        l->fhx = l->f.x / fl;
        l->fhz = l->f.z / fl;
        l->truck = ti;
        ldist[slot] = d;
    }
    /* The objects any beam might reach, each once, with the lamps on it. */
    int nc = 0;
    for(int li = 0; li < nl; li++) {
        const Lamp *l = &lamps[li];
        V3 e = vmad(l->o, l->f, l->range);
        float w = l->range * l->th + 9.0f;   /* the beam's spread and the biggest object */
        int c0 = cell_of(fminf(l->o.x, e.x) - w), c1 = cell_of(fmaxf(l->o.x, e.x) + w);
        int r0 = cell_of(fminf(l->o.z, e.z) - w), r1 = cell_of(fmaxf(l->o.z, e.z) + w);
        /* Across and along the beam, flat: a cell's circle (and the
           biggest prop) against the beam's wedge. */
        V3 fh = v3(l->fhx, 0, l->fhz);
        const float cr = LCELL * 0.71f + 9.0f;
        for(int cj = r0; cj <= r1; cj++)
            for(int ci = c0; ci <= c1; ci++) {
                int c = cj * LCELLS + ci;
                if(cell_first[c + 1] == cell_first[c]) continue;
                float dx = LEVEL_ORG + (ci + 0.5f) * LCELL - l->o.x, dz = LEVEL_ORG + (cj + 0.5f) * LCELL - l->o.z;
                float a = dx * fh.x + dz * fh.z;
                if(a + cr < 0 || a - cr > l->range) continue;
                if(fabsf(dx * fh.z - dz * fh.x) - cr > (a + cr) * l->th) continue;
                for(int k = cell_first[c]; k < cell_first[c + 1]; k++) {
                    int i = order[k];
                    if(!obj_in(&obj[i], l)) continue;
                    if(!cand_mask[i]) cand[nc++] = (uint16_t)i;
                    cand_mask[i] |= (uint8_t)(1 << li);
                }
            }
    }
    /* Only what is drawn near: on screen, and closer than the near
       trees and rocks are drawn (world.c) - past that the far versions
       stand in, which keep their colours. */
    for(int k = 0; k < nc; k++) {
        int i = cand[k];
        const LObj *o = &obj[i];
        V3 c = v3(o->x, o->y, o->z);
        float fd = DRAW_NEAR + o->r;
        V3 d = vsub(c, r_cam.pos);
        if(vdot(d, d) < fd * fd && r_sphere_visible(c, o->r)) obj_light(o, lamps, cand_mask[i]);
        cand_mask[i] = 0;
    }
    PROF_LAP(PS_LAMPS, pl);
    lamps_us += timer_us_gettime64() - t0;
}

int lamps_ground_mask(float x, float z, float r) {
    int mask = 0;
    for(int li = 0; li < nl; li++) {
        const Lamp *l = &lamps[li];
        float dx = x - l->o.x, dz = z - l->o.z;
        float a = dx * l->fhx + dz * l->fhz;
        if(a + r < 0 || a - r > l->range) continue;
        if(fabsf(dx * l->fhz - dz * l->fhx) - r > (a + r) * l->th) continue;
        mask |= 1 << li;
    }
    return mask;
}

float lamps_light(V3 p, int mask, int skip, V3 *from) {
    float sum = 0;
    V3 dir = v3(0, 0, 0);
    for(int li = 0; li < nl; li++) {
        const Lamp *l = &lamps[li];
        if(!(mask & (1 << li)) || l->truck == skip) continue;
        float dx = p.x - l->o.x, dy = p.y - l->o.y, dz = p.z - l->o.z;
        float a = dx * l->f.x + dy * l->f.y + dz * l->f.z;
        if(a < 0.3f || a > l->range) continue;
        float s = dx * l->r.x + dy * l->r.y + dz * l->r.z;
        float h = dx * l->u.x + dy * l->u.y + dz * l->u.z;
        float e = s * s * l->ih2 + h * h * (h > 0 ? l->iv2 : l->iv2 * 0.25f);
        float a2 = a * a;
        if(e >= a2) continue;
        float ia = shz_inv_sqrtf_fsrra(a2), x = a * l->irange;
        float k = (1.0f - e * ia * ia) * (1.0f - x * x) * l->k;
        sum += k;
        if(from) dir = vmad(dir, v3(-dx, -dy, -dz), k * shz_inv_sqrtf_fsrra(dx * dx + dy * dy + dz * dz));
    }
    if(from) *from = sum > 0 ? vnorm(dir) : v3(0, 1, 0);
    return sum;
}

int lamps_beams(void) {
    return nl;
}

void lamps_beam(int i, V3 *o, V3 *f, V3 *r, V3 *u, float *th, float *tv, float *range) {
    const Lamp *l = &lamps[i];
    *o = l->o;
    *f = l->f;
    *r = l->r;
    *u = l->u;
    *th = l->th;
    *tv = l->tv;
    *range = l->range;
}

void lamps_end(void) {
    /* Newest first: a vertex in the firelight and a beam was saved twice. */
    RV *pool = scenery.pool;
    for(int i = nsaved - 1; i >= 0; i--) pool[saved[i].k].c = saved[i].c;
    nsaved = 0;
}
