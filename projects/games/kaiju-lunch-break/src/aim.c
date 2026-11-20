/* Aiming a throw (aim.h). */
#include "aim.h"
#include "city.h"
#include "life.h"

#define SPEED 95.0f          /* across the ground, as a free throw goes */
#define FREE_UP 22.0f        /* a free throw's lift */
#define REACH 240.0f         /* the farthest a lock looks */
#define CONE 0.93f           /* cos of the half angle a lock takes (about 22 degrees) */
#define KEEP 0.86f           /* and keeps, once it has it */

static struct {
    bool on;
    float g, t_end, fade, spin;
    V3 p0, v;
    V3 land;                 /* where it comes down */
    bool land_ground;        /* on the ground, not into something */
    int lock;                /* 0 none, 1 a building, 2 something in the air */
    int bld;                 /* the building locked */
    V3 target;               /* the lock's point, eased */
    float lock_r, lock_age;
} aim;
static const pvr_poly_hdr_t *h_ring, *h_ring_flat;

void aim_init(void) {
    /* The arc and the lock show through everything, the kaiju included
       (the camera is behind it): translucent, since punch-through
       polygons keep their depth test whatever the header says. */
    h_ring = r_header(TEX_RING, HF_TR | HF_CLAMP | HF_NOFOG | HF_NOZTEST | HF_NOZWRITE);
    h_ring_flat = r_header(TEX_RING, HF_PT | HF_CLAMP);
}

void aim_off(void) {
    aim.on = false;
    aim.lock = 0;
}

V3 aim_velocity(void) { return aim.v; }

const char *aim_locked(void) {
    if(!aim.on || !aim.lock) return NULL;
    if(aim.lock == 2) return "A HELICOPTER";
    const Building *b = &city_b[aim.bld];
    return b->h >= 100 ? "A TOWER" : b->h >= 30 ? "A BUILDING" : b->h >= 12 ? "A BLOCK" : "A HOUSE";
}

/* The throw that carries p to t in the time a free throw's speed takes. */
static V3 solve(V3 p, V3 t, float g) {
    V3 d = v3(t.x - p.x, 0, t.z - p.z);
    float l = sqrtf(d.x * d.x + d.z * d.z) + 1e-3f, time = fmaxf(l / SPEED, 0.3f);
    float vy = clampf((t.y - p.y) / time + 0.5f * g * time, -20.0f, 75.0f);
    return v3(d.x / l * (l / time), vy, d.z / l * (l / time));
}

void aim_update(V3 p, V3 fwd, float g, float dt) {
    if(!aim.on) {
        aim.on = true;
        aim.lock = 0;
        aim.fade = 0;
    }
    aim.fade = fminf(1.0f, aim.fade + dt * 4.0f);
    aim.spin += dt;
    aim.g = g;
    aim.p0 = p;
    /* The lock: something in the air first, a building else. One already
       held keeps it while it stays near enough ahead. */
    V3 want;
    float r = 10;
    int lock = 0, bld = -1;
    float cone = aim.lock ? KEEP : CONE;
    if(life_air_target(p, fwd, REACH, cone, &want)) lock = 2, r = 11;
    else {
        int prev = aim.lock == 1 ? aim.bld : -1;
        bld = city_aim(p, fwd, 55.0f, REACH, cone, prev);
        if(bld >= 0) {
            const Building *b = &city_b[bld];
            lock = 1;
            want = v3(b->x, fminf(b->h * 0.55f, 34.0f), b->z);
            r = clampf(fmaxf(b->hw, b->hd) + 3.0f, 8.0f, 28.0f);
        }
    }
    if(lock != aim.lock || (lock == 1 && bld != aim.bld)) {
        aim.lock_age = 0;
        if(lock) aim.target = want;
    }
    aim.lock = lock;
    aim.bld = bld;
    aim.lock_age += dt;
    if(lock) {
        aim.target = vadd(aim.target, vscale(vsub(want, aim.target), smooth_k(14.0f, dt)));
        aim.lock_r = r;
        aim.v = solve(p, want, g);
    }
    else aim.v = vadd(vscale(fwd, SPEED), v3(0, FREE_UP, 0));
    /* Where it comes down: stepped until it is in a building, under the
       ground, or at what it was thrown at. */
    aim.land_ground = true;
    float t = 0;
    V3 q = p;
    for(int k = 0; k < 120; k++) {
        t += 1.0f / 30.0f;
        q = v3(p.x + aim.v.x * t, p.y + aim.v.y * t - 0.5f * g * t * t, p.z + aim.v.z * t);
        if(lock == 2 && vlen(vsub(q, want)) < 12.0f) {
            aim.land_ground = false;
            break;
        }
        int b = city_at(q.x, q.z);
        if(b >= 0 && q.y < city_b[b].h) {
            aim.land_ground = false;
            break;
        }
        if(q.y <= 0) {
            /* Back to where it crossed the ground. */
            float vy = aim.v.y - g * t;
            float back = vy < -1e-3f ? q.y / vy : 0;
            t -= back;
            q = v3(p.x + aim.v.x * t, 0, p.z + aim.v.z * t);
            break;
        }
    }
    aim.t_end = t;
    aim.land = q;
}

void aim_draw_tr(void) {
    if(!aim.on || aim.t_end <= 0) return;
    /* The arc: rings running out along it, from just clear of the jaws. */
    r_hdr(h_ring);
    const float step = 0.12f;
    float phase = fmodf(aim.spin * 1.8f, 1.0f) * step;
    uint32_t col = aim.lock ? 0xffff5048 : 0xffffd650;
    for(float t = phase + 0.12f; t < aim.t_end - 0.03f; t += step) {
        V3 q = v3(aim.p0.x + aim.v.x * t, aim.p0.y + aim.v.y * t - 0.5f * aim.g * t * t, aim.p0.z + aim.v.z * t);
        float s = (1.1f + 1.6f * t / aim.t_end) * aim.fade;
        r_billboard(q, s, s, 0, 0, 0, 1, 1, col);
    }
    /* The lock: rings closing in on it and turning. */
    float pulse = 0.5f + 0.5f * sinf(aim.spin * 9.0f);
    if(aim.lock) {
        float in = clampf(aim.lock_age / 0.25f, 0, 1), r = aim.lock_r * (2.2f - 1.2f * ease_out(in));
        uint32_t c = color_lerp(0xffff3048, 0xffffe060, pulse);
        r_billboard(aim.target, r, r, aim.spin * 2.5f, 0, 0, 1, 1, c);
        r_billboard(aim.target, r * 0.55f, r * 0.55f, -aim.spin * 4.0f, 0, 0, 1, 1, 0xffffffff);
    }
    else if(!aim.land_ground) {
        /* Into something it was not aimed at: a small mark where. */
        r_billboard(aim.land, 6.0f * aim.fade, 6.0f * aim.fade, aim.spin * 3.0f, 0, 0, 1, 1, 0xffffd650);
    }
}

void aim_draw_pt(void) {
    if(!aim.on) return;
    float pulse = 0.5f + 0.5f * sinf(aim.spin * 9.0f);
    /* Where it lands: a ring on the ground. */
    if(aim.land_ground) {
        r_hdr(h_ring_flat);
        for(int k = 0; k < 2; k++) {
            float r = (k ? 4.0f : 9.0f + 1.5f * pulse) * aim.fade;
            uint32_t c = k ? 0xffffffff : color_lerp(0xffff5040, 0xffffd650, pulse);
            V3 l = aim.land;
            RV v[4] = {{l.x - r, 0.5f, l.z - r, 0, 0, c}, {l.x + r, 0.5f, l.z - r, 1, 0, c},
                       {l.x - r, 0.5f, l.z + r, 0, 1, c}, {l.x + r, 0.5f, l.z + r, 1, 1, c}};
            r_strip(v, 4);
        }
    }
}
