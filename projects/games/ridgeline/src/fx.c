/* Ridgeline effects: what the tyres throw up (dust, gravel, mud, snow,
   spray), the roostertail a spinning tyre flings back, and the tracks the
   rear wheels leave in soft ground. Soft painted puffs are few and
   short-lived: on the console the PVR's fill rate, not the CPU, sets the
   budget. Clods are small and cheap, so a roostertail can be dense. */
#include "fx.h"
#include "world.h"
#include "truck.h"
#include "terrain.h"
#include "dunes.h"

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
   ground turns hard, the wheel leaves it, or the truck jumps (a recover).
   A point is only laid where the print has something to say (below), up
   to TRK_LONG apart, so the last stretch of a run is drawn to the tyre
   itself: without it the print trails the truck by up to that far, and
   catches up in jumps.

   The dunes keep their prints: sand holds a track until the wind has
   smoothed it, and from a crest the line a truck took up the last dune
   is half of what there is to look at. There the rings are several
   times longer (in the room the textures were linked into, main.c's
   spent_alloc), every truck prints in sand, a print shows from three
   times as far and lasts minutes. What wears one away is the wind, so
   its age runs on the wind's clock (dunes_wear): it softens a notch in
   each gust and stands in a lull, and sooner on ground open to the wind
   than in the lee, so a line over a dune goes first off its windward
   side and its crest. As it wears it spreads.

   Bare rock takes no print, but a tyre that spins or slides on it leaves
   its rubber: the player's rear tyres lay a black streak on dry rock and
   slickrock for as long as they slip, darker the harder they do. It is
   the same strip with the tread's texture drawn out along it (its v
   hardly moves), so the lugs read as streaks and not as prints. */
#define RUBBER_ON 1.2f         /* m/s of slip at the contact that starts a streak */
#define RUBBER_OFF 0.6f        /* ...and under which it ends */
#define RUBBER_COL 0x00140f0d
#define RUBBER_V 0.04f         /* how much of a print's length of texture a metre of streak uses */
#define TRK_PLAYER 448
#define TRK_CPU 128
#define TRK_PLAYER_DUNES 1536
#define TRK_CPU_DUNES 384
#define TRK_STEP 0.5f          /* metres between points, at least */
#define TRK_LONG 1.6f          /* ...and at most, on the straight */
#define TRK_LIFE 150.0f        /* seconds before a print has gone */
#define TRK_DRAW 35.0f         /* metres from the camera */
#define TRK_LIFE_DUNES 400.0f  /* ...of the wind's clock, on ground in the lee */
#define TRK_DRAW_DUNES 105.0f    /* the player's; the others' as anywhere, or a dune four trucks
                                    have climbed is a thousand points in view from its top */
#define TRK_BLOCK 16           /* points to a block of a ring, culled as one (the rings are whole blocks) */
#define TRK_COARSE 30.0f       /* past this a block of the dunes' prints is drawn from every other one */
#define TRK_COARSER 70.0f      /* ...and past this from every fourth */

typedef struct {
    V3 l, r;
    float v, born;
    uint32_t col;
    uint16_t run;
    uint16_t wear;             /* how fast it ages, 256ths: 256 but in the dunes' wind */
} TrkPt;
_Static_assert(sizeof(TrkPt) == 40, "TrkPt packing");

/* A sphere round a block's points, once all of them are laid (r < 0
   until then): most of a long ring is behind the camera or out of range
   and is passed over a block at a time. */
typedef struct {
    V3 c;
    float r;
} TrkBlk;

#define TRK_SUPER 8             /* blocks to a stretch, culled as one before its blocks are looked at:
                                  a full ring is hundreds of blocks, nearly all of them far behind */

typedef struct {
    TrkPt *pt;
    TrkBlk *blk, *sup;
    int cap, head, count;
    bool live;
    V3 last, dir;
    uint32_t col;
    float v;
    uint16_t run;
    /* Under the tyre now, past the last point laid. */
    bool tip_on;
    V3 tip_l, tip_r;
    float tip_v;
    uint32_t tip_col;
    uint16_t wear;
    bool rubber;               /* the run being laid is rubber on rock, not a print */
} TrkSlot;

static TrkPt trk_pool[TRK_PLAYER * 2 + TRK_CPU * 2 * (MAX_TRUCKS - 1)];
static TrkPt *trk_pool_dunes;
static TrkBlk trk_blk[(TRK_PLAYER_DUNES * 2 + TRK_CPU_DUNES * 2 * (MAX_TRUCKS - 1)) / TRK_BLOCK];
static TrkBlk trk_sup[(TRK_PLAYER_DUNES * 2 + TRK_CPU_DUNES * 2 * (MAX_TRUCKS - 1)) / (TRK_BLOCK * TRK_SUPER) + 2 * MAX_TRUCKS];
_Static_assert(TRK_PLAYER % TRK_BLOCK == 0 && TRK_CPU % TRK_BLOCK == 0 && TRK_PLAYER_DUNES % TRK_BLOCK == 0 &&
               TRK_CPU_DUNES % TRK_BLOCK == 0, "the rings are whole blocks");
static TrkSlot trk[MAX_TRUCKS][2];
static float fx_clock, trk_clock;
static float trk_life = TRK_LIFE, trk_draw = TRK_DRAW, trk_fade = 1.0f / 40.0f;
static bool trk_sand;          /* the dunes' rules */

/* What a tyre prints in each surface (none on rock): colour and how dark. */
static uint32_t track_col(int surf) {
    if(lvl_area == AREA_DUNES) {
        /* Grey sand: a tyre turns up the damp, darker sand under the
           crust, deepest where it is softest. */
        switch(surf) {
            case SURF_MUD: return 0xe0241c14;
            case SURF_DEEPSAND: return 0xc0544a3c;
            case SURF_SAND: return 0xb0544a3c;
            case SURF_HARDPACK: return 0x80483e32;
            case SURF_GRASS: return 0x90584c38;
            case SURF_DIRT: return 0x90584c38;
            case SURF_GRAVEL: return 0x70544c44;
            case SURF_SCREE: return 0x70544c44;
            default: return 0;
        }
    }
    if(look->dust) {
        /* The desert: red ground prints darker red; none on bare rock. */
        switch(surf) {
            case SURF_MUD: return 0xe0301408;
            case SURF_DIRT: return 0x90582c1a;
            case SURF_GRASS: return 0x90582c1a;
            case SURF_SAND: return 0xb0683a22;
            case SURF_GRAVEL: return 0x70583a2c;
            case SURF_SCREE: return 0x70583a2c;
            default: return 0;
        }
    }
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
    /* Sand flies in sheets with a cloud of it hanging behind. */
    [SURF_SAND] = {{0xffc8845a, 0xffd89a6c}, 55, 0.55f, 0.06f, 0, 0x70d8a074},
};

/* The dunes' sand is grey: packed, a few clods; loose, sheets of it;
   deep, a wall of it and a cloud that hangs. */
static const Rooster rooster_dunes[5] = {
    {{0xff8a7e68, 0xff9c907a}, 30, 0.55f, 0.07f, 0, 0},              /* packed sand, brush country, dirt */
    {{0xffa89c86, 0xffb8ac94}, 55, 0.55f, 0.06f, 0, 0x70c8bca4},     /* sand */
    {{0xffb4a892, 0xffc4b8a0}, 75, 0.62f, 0.065f, 0, 0x90d0c4ac},    /* deep sand */
    {{0xff8c8880, 0xff9a948a}, 30, 0.62f, 0.06f, 1, 0},              /* gravel, talus */
    {{0xff3a3024, 0xff4e4232}, 60, 0.60f, 0.11f, 0, 0x80645848},     /* mud */
};

/* The desert's dirt, scrub and talus are all red (the table above is the
   mountain's). */
static const Rooster rooster_red[4] = {
    {{0xff9a5c3c, 0xffb06e48}, 30, 0.55f, 0.08f, 0, 0},      /* dirt, scrub */
    {{0xff9c6a54, 0xffb07c62}, 30, 0.62f, 0.06f, 1, 0},      /* gravel, talus */
};

static const Rooster *rooster_of(int surf) {
    if(lvl_area == AREA_DUNES) {
        switch(surf) {
            case SURF_SAND: return &rooster_dunes[1];
            case SURF_DEEPSAND: return &rooster_dunes[2];
            case SURF_GRAVEL:
            case SURF_SCREE: return &rooster_dunes[3];
            case SURF_MUD: return &rooster_dunes[4];
            case SURF_ROCK:
            case SURF_WETROCK: return &rooster[surf];
            default: return &rooster_dunes[0];
        }
    }
    if(look->dust) {
        if(surf == SURF_DIRT || surf == SURF_GRASS || surf == SURF_FOREST) return &rooster_red[0];
        if(surf == SURF_GRAVEL || surf == SURF_SCREE) return &rooster_red[1];
    }
    return &rooster[surf];
}

static uint32_t dust_of(int surf) {
    if(look->dust && surf != SURF_MUD && surf != SURF_SNOW) return look->dust;
    return surf_props[surf].dust;
}

void fx_init(void) {
    h_puff = r_header(TEX_PUFF, HF_TR);
    h_clod = r_header(TEX_CLOD, HF_TR);
    h_tread = r_header(TEX_TREAD, HF_TR);
    fx_clear();
}

/* Also when the area changes: the dunes have their own rings and rules. */
void fx_clear(void) {
    part_n = 0;
    trk_sand = lvl_area == AREA_DUNES;
    if(trk_sand && !trk_pool_dunes)
        trk_pool_dunes = spent_alloc(sizeof(TrkPt) * (TRK_PLAYER_DUNES * 2 + TRK_CPU_DUNES * 2 * (MAX_TRUCKS - 1)));
    if(!trk_pool_dunes) trk_sand = false;
    trk_life = trk_sand ? TRK_LIFE_DUNES : TRK_LIFE;
    trk_draw = trk_sand ? TRK_DRAW_DUNES : TRK_DRAW;
    /* A print in sand softens all through its second half; elsewhere it
       goes in its last forty seconds. */
    trk_fade = trk_sand ? 1.0f / (0.55f * TRK_LIFE_DUNES) : 1.0f / 40.0f;
    TrkPt *p = trk_sand ? trk_pool_dunes : trk_pool;
    TrkBlk *bl = trk_blk, *su = trk_sup;
    for(int ti = 0; ti < MAX_TRUCKS; ti++)
        for(int s = 0; s < 2; s++) {
            int cap = trk_sand ? (ti == 0 ? TRK_PLAYER_DUNES : TRK_CPU_DUNES) : (ti == 0 ? TRK_PLAYER : TRK_CPU);
            int nb = cap / TRK_BLOCK, ns = (nb + TRK_SUPER - 1) / TRK_SUPER;
            trk[ti][s] = (TrkSlot){.pt = p, .blk = bl, .sup = su, .cap = cap};
            for(int k = 0; k < nb; k++) bl[k].r = -1.0f;
            for(int k = 0; k < ns; k++) su[k].r = -1.0f;
            p += cap;
            bl += nb;
            su += ns;
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
    s->wear = 256;
    if(trk_sand) {
        /* Open to the wind (the ground leans into it) a print wears up
           to twice as fast as in the lee. */
        V3 n;
        terrain_height_n(p.x, p.z, &n);
        s->wear = (uint16_t)(256.0f * (1.0f + 1.1f * clampf(0.3f - 3.0f * (n.x * WIND_X + n.z * WIND_Z), 0, 1)));
    }
    q->v = s->v;
    q->born = trk_clock;
    q->col = col;
    q->run = s->run;
    q->wear = s->wear;
    /* Its block is being laid again, or is now whole. */
    int at = s->head % TRK_BLOCK;
    TrkBlk *bk = &s->blk[s->head / TRK_BLOCK];
    int bi = s->head / TRK_BLOCK, nb = s->cap / TRK_BLOCK;
    if(at == 0) bk->r = s->sup[bi / TRK_SUPER].r = -1.0f;
    else if(at == TRK_BLOCK - 1) {
        const TrkPt *first = q - (TRK_BLOCK - 1);
        V3 lo = first->l, hi = first->l;
        for(int k = 0; k < TRK_BLOCK; k++)
            for(int e = 0; e < 2; e++) {
                V3 v = e ? first[k].r : first[k].l;
                lo = v3(fminf(lo.x, v.x), fminf(lo.y, v.y), fminf(lo.z, v.z));
                hi = v3(fmaxf(hi.x, v.x), fmaxf(hi.y, v.y), fmaxf(hi.z, v.z));
            }
        bk->c = vscale(vadd(lo, hi), 0.5f);
        bk->r = vlen(vsub(hi, lo)) * 0.5f + 0.5f;
        /* ...and with it, perhaps, its stretch. */
        if(bi % TRK_SUPER == TRK_SUPER - 1 || bi == nb - 1) {
            const TrkBlk *b0 = &s->blk[bi - bi % TRK_SUPER];
            int n = bi % TRK_SUPER + 1;
            bool whole = true;
            V3 slo = b0->c, shi = b0->c;
            float rmax = 0;
            for(int k = 0; k < n; k++) {
                whole &= b0[k].r >= 0;
                slo = v3(fminf(slo.x, b0[k].c.x), fminf(slo.y, b0[k].c.y), fminf(slo.z, b0[k].c.z));
                shi = v3(fmaxf(shi.x, b0[k].c.x), fmaxf(shi.y, b0[k].c.y), fmaxf(shi.z, b0[k].c.z));
                rmax = fmaxf(rmax, b0[k].r);
            }
            TrkBlk *sp = &s->sup[bi / TRK_SUPER];
            sp->c = vscale(vadd(slo, shi), 0.5f);
            sp->r = whole ? vlen(vsub(shi, slo)) * 0.5f + rmax : -1.0f;
        }
    }
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
        /* CPU trucks print only where it shows most: mud and snow, and
           the dunes' sand. On the trails they follow each other and
           their prints pile up unseen. */
        if(ti != 0 && w->surf != SURF_MUD && w->surf != SURF_SNOW &&
           !(trk_sand && (w->surf == SURF_SAND || w->surf == SURF_DEEPSAND)))
            col = 0;
        bool rubber = false;
        if(!col && ti == 0 && w->contact && w->water < 0.08f && (w->surf == SURF_ROCK || w->surf == SURF_SLICKROCK) &&
           w->slip > (s->live && s->rubber ? RUBBER_OFF : RUBBER_ON)) {
            rubber = true;
            col = RUBBER_COL | (uint32_t)(255.0f * clampf(0.3f + (w->slip - RUBBER_OFF) * 0.14f, 0.3f, 0.85f)) << 24;
        }
        if(!col) {
            s->live = s->tip_on = false;
            continue;
        }
        /* From a print to a streak or back is a new run. */
        if(rubber != s->rubber) s->live = false;
        s->rubber = rubber;
        if(!rubber) {
            /* Dug in: a spinning tyre leaves a darker print. */
            float dig = clampf((w->slip - 1.5f) / 5.0f, 0, 1);
            col = color_alpha(col, fminf(1.0f, (float)(col >> 24) / 255.0f * (1.0f + 0.5f * dig)));
        }
        float vk = rubber ? RUBBER_V : 1.0f;
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
                s->v += dist * vk;
                track_add(s, p, dn, half, col);
            }
        }
        /* From the last point to the tyre. */
        d = vsub(p, s->last);
        d.y = 0;
        dist = vlen(d);
        s->tip_on = dist > 0.05f;
        if(s->tip_on) {
            V3 side = vscale(v3(d.z, 0, -d.x), half / dist);
            s->tip_l = vsub(p, side);
            s->tip_r = vadd(p, side);
            s->tip_l.y = terrain_height(s->tip_l.x, s->tip_l.z) + 0.035f;
            s->tip_r.y = terrain_height(s->tip_r.x, s->tip_r.z) + 0.035f;
            s->tip_v = s->v + dist * vk;
            s->tip_col = col;
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
    const Rooster *rs = rooster_of(w->surf);
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
    trk_clock += dt * dunes_wear();
    /* Wheels: what each one throws, by surface, slip and speed. */
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        if(t->absent) continue;
        float dcam = vlen(vsub(t->pos, r_cam.pos));
        if(dcam > 90.0f) continue;
        tracks_update(ti, t);
        for(int i = 0; i < 4; i++) {
            Wheel *w = &t->wh[i];
            if(!w->contact) continue;
            rooster_update(ti, t, i, dcam, dt);
            float v = fabsf(w->omega * t->spec.radius);
            float rate = w->slip * 9.0f + v * 0.9f;
            bool wet = w->water > 0.08f;
            if((w->surf == SURF_ROCK || w->surf == SURF_SLICKROCK) && !wet) rate = w->slip * 2.0f;
            if(w->surf == SURF_SAND || w->surf == SURF_DEEPSAND) rate *= 1.5f;
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
                         1.6f + 1.2f * frand(), 0.45f, 1.5f, color_alpha(dust_of(w->surf), 0.42f), 1.4f, -0.15f);
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
    RV v[TRK_CHUNK * 2 + 2];   /* (a full strip can take the pair that joins it to the next block) */
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

/* A print's two vertices as they are now: fading out with age and
   toward the draw distance (by its square: no root per point), its
   edges slumped outward as it wears in sand. False once it has gone. */
typedef struct {
    V3 cam;
    float draw2, idraw;
} TrkView;

static inline bool trk_pair(const TrkView *tv, const TrkPt *q, RV *out) {
    float dx = q->l.x - tv->cam.x, dz = q->l.z - tv->cam.z, dist2 = dx * dx + dz * dz;
    float age = (trk_clock - q->born) * (float)q->wear * (1.0f / 256.0f);
    float fa = (trk_life - age) * trk_fade, fd = (tv->draw2 - dist2) * tv->idraw;
    bool live = fa > 0 && fd > 0;
    float a = live ? (float)(q->col >> 24) * (1.0f / 255.0f) * fminf(fa, 1.0f) * fminf(fd, 1.0f) : 0;
    uint32_t c = color_alpha(q->col, a);
    float v = q->v * (1.0f / 1.1f);
    if(trk_sand) {
        float sp = 0.22f * age * (1.0f / TRK_LIFE_DUNES);
        V3 e = v3((q->l.x - q->r.x) * sp, (q->l.y - q->r.y) * sp, (q->l.z - q->r.z) * sp);
        out[0] = (RV){q->l.x + e.x, q->l.y + e.y, q->l.z + e.z, 0, v, c};
        out[1] = (RV){q->r.x - e.x, q->r.y - e.y, q->r.z - e.z, 1, v, c};
    }
    else {
        out[0] = (RV){q->l.x, q->l.y, q->l.z, 0, v, c};
        out[1] = (RV){q->r.x, q->r.y, q->r.z, 1, v, c};
    }
    return live;
}

static void tracks_draw(void) {
    static TrkBuf b;
    TrkView tv = {.cam = r_cam.pos};
    V3 cam = tv.cam;
    r_hdr(h_tread);
    for(int ti = 0; ti < MAX_TRUCKS; ti++) {
        float draw = ti == 0 ? trk_draw : TRK_DRAW;
        tv.draw2 = draw * draw;
        tv.idraw = 1.0f / (tv.draw2 * 0.45f);
        for(int k = 0; k < 2; k++) {
            const TrkSlot *s = &trk[ti][k];
            int run = -1;
            b.n = 0;
            int idx = s->head - s->count;
            if(idx < 0) idx += s->cap;
            for(int j = 0; j < s->count; j++, idx = idx + 1 == s->cap ? 0 : idx + 1) {
                const TrkPt *q = &s->pt[idx];
                if(idx % (TRK_BLOCK * TRK_SUPER) == 0) {
                    /* A whole stretch out of range, out of sight or worn
                       away. */
                    const TrkBlk *sp = &s->sup[idx / (TRK_BLOCK * TRK_SUPER)];
                    int span = s->cap - idx < TRK_BLOCK * TRK_SUPER ? s->cap - idx : TRK_BLOCK * TRK_SUPER;
                    if(sp->r >= 0 && j + span <= s->count) {
                        float sx = sp->c.x - cam.x, sz = sp->c.z - cam.z, reach = draw + sp->r;
                        if(sx * sx + sz * sz >= reach * reach || trk_clock - q[span - 1].born >= trk_life ||
                           !r_sphere_visible(sp->c, sp->r)) {
                            chunk_flush(&b);
                            run = -1;
                            j += span - 1;
                            idx += span - 1;
                            continue;
                        }
                    }
                }
                const TrkBlk *bk = &s->blk[idx / TRK_BLOCK];
                if(idx % TRK_BLOCK == 0 && j + TRK_BLOCK <= s->count && bk->r >= 0) {
                    /* A whole block. Nothing of it to draw if it is out of
                       range, out of sight or worn away (its last point is
                       its newest). */
                    float bx = bk->c.x - cam.x, bz = bk->c.z - cam.z, reach = draw + bk->r, bd2 = bx * bx + bz * bz;
                    bool out = bd2 >= reach * reach || trk_clock - q[TRK_BLOCK - 1].born >= trk_life ||
                               !r_sphere_visible(bk->c, bk->r);
                    if(out || q[TRK_BLOCK - 1].run == q->run) {
                        /* (the strip so far runs on to this block's first print) */
                        if(!out && run == q->run && b.n >= 2) {
                            RV e[2];
                            trk_pair(&tv, q, e);
                            chunk_add(&b, &e[0]);
                            chunk_add(&b, &e[1]);
                        }
                        chunk_flush(&b);
                        run = -1;
                        j += TRK_BLOCK - 1;
                        idx += TRK_BLOCK - 1;
                        if(out) continue;
                        /* One run from end to end: sent as a strip on the
                           block's own sphere, on to the next block's first
                           print where the run goes on. In the dunes, from
                           every other print past TRK_COARSE and every
                           fourth past TRK_COARSER: a whole climb's prints
                           are in view from its top. */
                        int step = !trk_sand || bd2 < TRK_COARSE * TRK_COARSE ? 1 : bd2 < TRK_COARSER * TRK_COARSER ? 2 : 4;
                        RV v[2 * (TRK_BLOCK + 1)];
                        int n = 0;
                        bool any = false;
                        for(int i = 0; i < TRK_BLOCK; i += step, n += 2) any |= trk_pair(&tv, &q[i], &v[n]);
                        const TrkPt *nx = &s->pt[idx + 1 == s->cap ? 0 : idx + 1];
                        if(j + 1 < s->count && nx->run == q->run) {
                            any |= trk_pair(&tv, nx, &v[n]);
                            n += 2;
                        }
                        else if(step > 1) {
                            trk_pair(&tv, &q[TRK_BLOCK - 1], &v[n]);
                            n += 2;
                        }
                        if(any && n >= 4) r_strip_bounded(v, n, bk->c, bk->r + 2.6f);
                        continue;
                    }
                }
                RV e[2];
                bool live = trk_pair(&tv, q, e);
                if(!live || q->run != run) {
                    chunk_flush(&b);
                    run = -1;
                    if(!live) continue;
                    run = q->run;
                }
                else if(b.n >= TRK_CHUNK * 2) {
                    /* A full strip: the next starts from its last pair. */
                    RV l = b.v[b.n - 2], r = b.v[b.n - 1];
                    chunk_flush(&b);
                    chunk_add(&b, &l);
                    chunk_add(&b, &r);
                }
                chunk_add(&b, &e[0]);
                chunk_add(&b, &e[1]);
            }
            /* The run still being laid, on to the tyre. */
            if(s->tip_on && run == s->run && b.n >= 2) {
                if(b.n >= TRK_CHUNK * 2) {
                    RV l = b.v[b.n - 2], r = b.v[b.n - 1];
                    chunk_flush(&b);
                    chunk_add(&b, &l);
                    chunk_add(&b, &r);
                }
                float dx = s->tip_l.x - cam.x, dz = s->tip_l.z - cam.z, dist2 = dx * dx + dz * dz;
                float a = (float)(s->tip_col >> 24) * (1.0f / 255.0f) * clampf((tv.draw2 - dist2) * tv.idraw, 0, 1);
                uint32_t c = color_alpha(s->tip_col, a);
                float v = s->tip_v * (1.0f / 1.1f);
                chunk_add(&b, &(RV){s->tip_l.x, s->tip_l.y, s->tip_l.z, 0, v, c});
                chunk_add(&b, &(RV){s->tip_r.x, s->tip_r.y, s->tip_r.z, 1, v, c});
            }
            chunk_flush(&b);
        }
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
