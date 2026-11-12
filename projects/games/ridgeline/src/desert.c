/* Ridgeline: tumbleweeds and dust devils (desert.h).

   A tumbleweed starts out of sight upwind of the camera on open ground
   and rolls downwind, hopping, quicker in the gusts, until it runs into a
   slope or water or is left behind. Each is a quad of twigs turned about
   the camera's line of sight as it rolls.

   A dust devil is a leaning column of the dust puff, sixteen of them
   climbing and circling, that wanders the flats for a minute around the
   middle of the day: far enough from the camera to be a thing in the
   landscape (close to, soft sprites that size are what the PVR is slow
   at), thinning out if a truck drives up to it.

   Their randomness is their own, so a demo or a bench that seeds the
   game's generator plays out the same. */
#include "desert.h"
#include "world.h"
#include "terrain.h"
#include "tod.h"

#define WEEDS 6
#define DEVIL_PUFFS 16
#define WEED_FAR 110.0f

typedef struct {
    V3 p;
    float vy, roll, size, speed, hop;
    bool live;
} Weed;

static Weed weeds[WEEDS];
static const pvr_poly_hdr_t *h_weed, *h_dust;
static float wind_t, spawn_t;
static bool on;

static struct {
    bool live;
    V3 p;
    float age, life, head, spin;
} devil;
static float devil_wait;

static uint32_t seed = 0x7ab1e5u;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}

/* Open, level, dry ground: where the wind has a run. */
static bool open_ground(float x, float z) {
    if(fabsf(x) > 370.0f || fabsf(z) > 370.0f) return false;
    int sf = surface_at(x, z);
    if(sf != SURF_GRASS && sf != SURF_SAND && sf != SURF_DIRT && sf != SURF_HARDPACK) return false;
    if(water_at(x, z) > -999.0f) return false;
    V3 n;
    terrain_height_n(x, z, &n);
    return n.y > 0.97f;
}

void desert_init(void) {
    h_weed = r_header(TEX_WEED, HF_PT);
    h_dust = r_header(TEX_PUFF, HF_TR);
    on = lvl_area == AREA_MOAB || lvl_area == AREA_DUNES;
    for(int i = 0; i < WEEDS; i++) weeds[i].live = false;
    devil.live = false;
    devil_wait = 20.0f;
    spawn_t = 2.0f;
}

void desert_update(float dt) {
    if(!on || game_frozen()) return;
    wind_t += dt;
    V3 c = cam.pos;
    /* The wind comes in gusts a tumbleweed waits for. */
    float gust = 0.55f + 0.45f * sinf(wind_t * 0.23f) * sinf(wind_t * 0.071f + 1.3f);
    if(gust < 0.15f) gust = 0.15f;
    spawn_t -= dt * gust;
    if(spawn_t <= 0) {
        spawn_t = 5.0f + 9.0f * rnd();
        for(int i = 0; i < WEEDS; i++) {
            Weed *w = &weeds[i];
            if(w->live) continue;
            /* Upwind and off to one side, to cross ahead of the camera. */
            float back = 45.0f + 35.0f * rnd(), side = (rnd() * 2.0f - 1.0f) * 55.0f;
            float x = c.x - WIND_X * back - WIND_Z * side + cam.fwd.x * 30.0f;
            float z = c.z - WIND_Z * back + WIND_X * side + cam.fwd.z * 30.0f;
            if(!open_ground(x, z)) break;
            *w = (Weed){.p = v3(x, terrain_height(x, z), z), .size = 0.45f + 0.45f * rnd(), .speed = 3.0f + 3.0f * rnd(),
                        .hop = rnd() * 6.0f, .live = true};
            break;
        }
    }
    for(int i = 0; i < WEEDS; i++) {
        Weed *w = &weeds[i];
        if(!w->live) continue;
        float v = w->speed * (0.4f + gust);
        float nx = w->p.x + WIND_X * v * dt, nz = w->p.z + WIND_Z * v * dt;
        float dx = nx - c.x, dz = nz - c.z;
        if(dx * dx + dz * dz > WEED_FAR * WEED_FAR || !open_ground(nx, nz)) {
            w->live = false;   /* caught in something, or gone */
            continue;
        }
        float g = terrain_height(nx, nz) + w->size * 0.8f;
        /* It bounds along: a hop every so often, more of them at speed. */
        w->hop -= dt * v;
        w->vy -= 9.0f * dt;
        float y = w->p.y + w->vy * dt;
        if(y <= g) {
            y = g;
            w->vy = 0;
            if(w->hop <= 0) {
                w->vy = 1.2f + 1.6f * rnd();
                w->hop = 4.0f + 8.0f * rnd();
            }
        }
        w->p = v3(nx, y, nz);
        w->roll += v * dt / w->size;
    }
    /* The dust devil: late morning to late afternoon, one at a time. */
    if(devil.live) {
        devil.age += dt;
        devil.head += (rnd() - 0.5f) * 1.2f * dt;
        float nx = devil.p.x + sinf(devil.head) * 3.2f * dt, nz = devil.p.z + cosf(devil.head) * 3.2f * dt;
        if(open_ground(nx, nz)) devil.p = v3(nx, terrain_height(nx, nz), nz);
        else devil.head += 1.5f;   /* turned by the edge of the open ground */
        devil.spin += dt * 5.0f;
        if(devil.age > devil.life) devil.live = false;
    }
    else if((devil_wait -= dt) <= 0) {
        devil_wait = 50.0f + 70.0f * rnd();
        if(tod.hour > 10.5f && tod.hour < 17.0f) {
            float a = rnd() * ENG_TAU, d = 70.0f + 60.0f * rnd();
            float x = c.x + sinf(a) * d, z = c.z + cosf(a) * d;
            if(open_ground(x, z)) {
                devil.live = true;
                devil.p = v3(x, terrain_height(x, z), z);
                devil.age = 0;
                devil.life = 40.0f + 30.0f * rnd();
                devil.head = rnd() * ENG_TAU;
            }
            else devil_wait = 6.0f;
        }
    }
}

void desert_draw_pt(void) {
    if(!on) return;
    bool hdr = false;
    for(int i = 0; i < WEEDS; i++) {
        const Weed *w = &weeds[i];
        if(!w->live || !r_sphere_visible(w->p, w->size * 1.5f)) continue;
        if(!hdr) {
            r_hdr(h_weed);
            hdr = true;
        }
        /* Lit as the ground under it is. */
        uint32_t g = lvl_c((int)((w->p.x - LEVEL_ORG) / LEVEL_D + 0.5f), (int)((w->p.z - LEVEL_ORG) / LEVEL_D + 0.5f));
        int l = (int)(((g >> 16) & 255) * 5 + ((g >> 8) & 255) * 9 + (g & 255) * 4) / 11;
        if(l > 255) l = 255;
        uint32_t col = 0xff000000u | (uint32_t)l << 16 | (uint32_t)l << 8 | (uint32_t)l;
        r_billboard(w->p, w->size, w->size, w->roll, 0, 0, 1, 1, col);
    }
}

void desert_draw_tr(void) {
    if(!on || !devil.live) return;
    float dist = vlen(vsub(devil.p, r_cam.pos));
    if(dist > 260.0f || !r_sphere_visible(vadd(devil.p, v3(0, 12, 0)), 15.0f)) return;
    /* In and out over a few seconds; gone close to (see above). */
    float k = clampf(devil.age * 0.3f, 0, 1) * clampf((devil.life - devil.age) * 0.25f, 0, 1) *
              clampf((dist - 22.0f) * (1.0f / 25.0f), 0, 1) * (1.0f - pal->dark);
    if(k < 0.03f) return;
    /* Paler than the ground it came off: dust in the sun. */
    uint32_t dust = world_lit(lvl_area == AREA_DUNES ? 0xffe4dcc8 : 0xfff0c8a4, v3(0, 1, 0));
    r_hdr(h_dust);
    for(int i = 0; i < DEVIL_PUFFS; i++) {
        /* Each puff climbs the column and starts again at its foot. */
        float t = (float)i / DEVIL_PUFFS + devil.age * 0.09f;
        t -= floorf(t);
        float y = 24.0f * t, r = 0.4f + 2.4f * t * t, a = devil.spin + i * 1.9f + t * 5.0f;
        V3 p = v3(devil.p.x + sinf(a) * r + 3.0f * t * WIND_X, devil.p.y + y + 0.6f, devil.p.z + cosf(a) * r + 3.0f * t * WIND_Z);
        float s = 1.5f + 2.6f * t;
        r_billboard(p, s, s, a, 0, 0, 1, 1, color_alpha(dust, 0.5f * k * (1.0f - 0.55f * t)));
    }
}
