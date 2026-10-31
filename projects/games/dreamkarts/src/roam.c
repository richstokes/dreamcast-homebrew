/* Free Roam: the lot's obstacles and the grid (roam.h). Collisions work
   as the barriers do in kart.c: the kart is pushed back to the surface
   and the velocity into it is reflected, so a glancing blow slides along
   a wall and a square one thuds. */
#include "roam.h"
#include "track.h"

const float lot_row_z[LOT_ROWS] = {-40.0f, -12.0f, 16.0f, 44.0f};

#define ROAM_SOLIDS 200
typedef struct {
    float x, z, cs, sn;   /* centre and the yaw it is turned by (cs = cos, sn = sin) */
    float hx, hz;         /* half extents; a disc has hz < 0 and its radius in hx */
    float top;
} Solid;
static Solid solids[ROAM_SOLIDS];
static int solid_n;

void roam_solids_clear(void) {
    solid_n = 0;
}

void roam_solid_box(V3 c, float hx, float hz, float yaw, float top) {
    if(solid_n >= ROAM_SOLIDS) return;
    solids[solid_n++] = (Solid){c.x, c.z, cosf(yaw), sinf(yaw), hx, hz, top};
}

void roam_solid_disc(V3 c, float r, float top) {
    if(solid_n >= ROAM_SOLIDS) return;
    solids[solid_n++] = (Solid){c.x, c.z, 1.0f, 0.0f, r, -1.0f, top};
}

/* How far p (a disc of radius r about it) is into solid o, and the way
   out: the normal and the depth, or a negative depth when clear. */
static float solid_push(const Solid *o, float px, float pz, float r, float *nx, float *nz) {
    float dx = px - o->x, dz = pz - o->z;
    if(o->hz < 0) {
        float d2 = dx * dx + dz * dz, reach = o->hx + r;
        if(d2 >= reach * reach) return -1.0f;
        float d = sqrtf(d2);
        if(d < 1e-3f) { *nx = 1; *nz = 0; return reach; }
        *nx = dx / d;
        *nz = dz / d;
        return reach - d;
    }
    /* Into the box's own frame (world_tsudoken.c turns props the same way). */
    float lx = dx * o->cs - dz * o->sn, lz = dx * o->sn + dz * o->cs;
    float ex = o->hx + r - fabsf(lx), ez = o->hz + r - fabsf(lz);
    if(ex <= 0 || ez <= 0) return -1.0f;
    float bx, bz, depth;
    if(ex < ez) { bx = lx < 0 ? -1.0f : 1.0f; bz = 0; depth = ex; }
    else { bx = 0; bz = lz < 0 ? -1.0f : 1.0f; depth = ez; }
    *nx = bx * o->cs + bz * o->sn;
    *nz = -bx * o->sn + bz * o->cs;
    return depth;
}

/* The bounce off a wall: pushed back by over along n (which points out
   of the wall, toward the kart), the velocity into it reflected, the
   speed cut by how square the hit was, the heading eased toward the
   wall's tangent so a glancing blow slides along it (the barriers'
   bounce in kart.c, shared by the lot's walls and solids). */
static void bounce(Kart *k, float nx, float nz, float over, float tx, float tz) {
    k->pos.x += nx * over;
    k->pos.z += nz * over;
    float vn = k->vel.x * nx + k->vel.z * nz;   /* negative: moving into the wall */
    if(vn >= 0) return;
    k->vel.x -= nx * vn * 1.35f;
    k->vel.z -= nz * vn * 1.35f;
    float spd = fabsf(k->speed);
    float frac = spd > 0.1f ? clampf(-vn / spd, 0, 1) : 0;
    k->speed *= 1.0f - 0.75f * frac;
    if(tx != 0 || tz != 0) {
        float along = atan2f(tx, tz);
        if(k->speed < 0) along = wrap_angle(along + DK_PI);
        float diff = wrap_angle(along - k->heading);
        if(fabsf(diff) < 1.2f) k->heading += diff * 0.5f * frac;
    }
    if(-vn > 2.0f && k->bump_t <= 0) {
        k->events |= EV_WALL;
        k->wall_v = -vn;
        k->bump_t = 0.35f;
    }
}

/* A wall along the ring road at lateral offset w, two-sided: a kart
   within KART_RADIUS of it is pushed back to whichever side it is on. */
static void ring_wall(Kart *k, float w) {
    float d = k->tp.d - w;
    if(fabsf(d) >= KART_RADIUS) return;
    float side = d < 0 ? -1.0f : 1.0f;
    float nx = k->tp.right.x * side, nz = k->tp.right.z * side;
    bounce(k, nx, nz, KART_RADIUS - fabsf(d), k->tp.tangent.x, k->tp.tangent.z);
}

/* A wall along x at z = w, two-sided. */
static void z_wall(Kart *k, float w) {
    float d = k->pos.z - w;
    if(fabsf(d) >= KART_RADIUS) return;
    bounce(k, 0, d < 0 ? -1.0f : 1.0f, KART_RADIUS - fabsf(d), 1.0f, 0.0f);
}

void roam_kart_walls(Kart *k) {
    float s = k->tp.s, d = k->tp.d;
    /* The ring's barrier, out beyond the deck along it. */
    ring_wall(k, lot_wall_d(s));
    /* The deck's parapets for a kart up on it (a kart at the ground
       counts as on it only round a ramp's foot, where it would pop up
       onto the ramp from the side); the inner one is open at the gaps.
       Under the rest of the deck the ground is the ground, and a kart
       drives through between the pillars. */
    float dy = lot_deck_y(s);
    if(lot_in_deck(s) && dy > 0.5f) {
        float d0 = lot_deck_d0(s), d1 = lot_deck_d1(s);
        if(k->pos.y > dy - 2.5f) {
            if(!lot_deck_gap(s)) ring_wall(k, d0);
            ring_wall(k, d1);
        }
        else if(lot_deck_banked(s)) {
            /* At the ground by the ramp's embankment, which is solid
               earth: its retaining wall on the road side, the toe of its
               bank on the other, and under the deck the end wall where
               the span begins. A kart inside it (it drove in under the
               deck before that wall existed, or fell in) leaves by the
               nearest face. */
            float sd, depth = lot_deck_emb_depth(s, &sd) + KART_RADIUS;
            float in_d = d - d0 + KART_RADIUS, out_d = d1 + LOT_RAMP_BATTER * dy + 0.3f + KART_RADIUS - d;
            if(depth > 0 && in_d > 0 && out_d > 0) {
                if(in_d <= out_d && in_d <= depth) bounce(k, -k->tp.right.x, -k->tp.right.z, in_d, k->tp.tangent.x, k->tp.tangent.z);
                else if(out_d <= depth) bounce(k, k->tp.right.x, k->tp.right.z, out_d, k->tp.tangent.x, k->tp.tangent.z);
                else bounce(k, k->tp.tangent.x * sd, k->tp.tangent.z * sd, depth, k->tp.right.x, k->tp.right.z);
            }
        }
    }
    /* The overpass's, the same: its parapets for a kart up on it, the
       toe of its embankment for one at the ground beside that. */
    if(d < 0 && fabsf(k->pos.x) < LOT_OVER_FOOT) {
        float oy = lot_over_y(k->pos.x);
        if(oy > 0.5f) {
            if(k->pos.y > oy - 2.5f) {
                z_wall(k, LOT_OVER_Z - LOT_OVER_HW);
                z_wall(k, LOT_OVER_Z + LOT_OVER_HW);
            }
            else if(lot_over_banked(k->pos.x)) {
                /* Its embankment is solid too: the toes either side and
                   the end wall under the span at the abutment. */
                float toe = LOT_OVER_HW + LOT_RAMP_BATTER * oy + 0.3f + KART_RADIUS;
                float south = toe - (LOT_OVER_Z - k->pos.z), north = toe - (k->pos.z - LOT_OVER_Z);
                float sx = k->pos.x < 0 ? 1.0f : -1.0f;   /* toward the span */
                float depth = fabsf(k->pos.x) - (LOT_OVER_FOOT - LOT_OVER_EMB) + KART_RADIUS;
                if(south > 0 && north > 0 && depth > 0) {
                    if(south <= north && south <= depth) bounce(k, 0, -1.0f, south, 1.0f, 0.0f);
                    else if(north <= depth) bounce(k, 0, 1.0f, north, 1.0f, 0.0f);
                    else bounce(k, sx, 0, depth, 0.0f, 1.0f);
                }
            }
        }
    }
    roam_kart_solids(k);
    track_locate(k->pos, k->tp.idx, &k->tp);
}

void roam_kart_solids(Kart *k) {
    for(int i = 0; i < solid_n; i++) {
        const Solid *o = &solids[i];
        if(k->pos.y > o->top) continue;   /* over it: a charge jump clears a planter */
        float ax = fabsf(k->pos.x - o->x), az = fabsf(k->pos.z - o->z), reach = o->hx + fmaxf(o->hz, 0.0f) + KART_RADIUS;
        if(ax > reach || az > reach) continue;
        float nx, nz;
        float depth = solid_push(o, k->pos.x, k->pos.z, KART_RADIUS, &nx, &nz);
        if(depth <= 0) continue;
        bounce(k, nx, nz, depth, 0, 0);
    }
}

bool roam_solid(V3 p, float margin) {
    for(int i = 0; i < solid_n; i++) {
        const Solid *o = &solids[i];
        if(p.y > o->top + margin) continue;
        float nx, nz;
        if(solid_push(o, p.x, p.z, margin, &nx, &nz) > 0) return true;
    }
    return false;
}

V3 roam_bay(int bay, float *heading) {
    /* The north side of the third row, from two bays west of the row's
       middle eastward with a bay between each pair, nosed in facing
       south, so pulling out is straight ahead in reverse or a turn into
       the aisle. */
    float x = -1.25f + 5.0f * (float)bay;
    *heading = DK_PI;
    return v3(x, 0, lot_row_z[2] + LOT_ROW_HALF * 0.5f);
}

void roam_place_karts(void) {
    int cpu = 0;
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        int player = -1;
        for(int p = 0; p < game.num_players; p++)
            if(game.pl[p].kart == i) player = p;
        /* Online the karts are in grid order and all somebody's: a bay
           each (the remote ones are soon where the network says). */
        if(game.online) player = i;
        if(player >= 0) {
            float h;
            V3 p = roam_bay(player, &h);
            k->pos = p;
            k->pos.y = k->gnd = 0;
            k->heading = h;
            k->vel = v3(0, 0, 0);
            k->speed = 0;
            track_locate(k->pos, -1, &k->tp);
            k->progress = k->tp.s;
        }
        else {
            /* Spread round the ring, two lanes, cruising a little under
               the racing pace. */
            float s = track_wrap(60.0f + (float)cpu * track.length / 6.0f);
            float d = (cpu & 1) ? 3.0f : -3.0f;
            k->pos = track_point(s, d);
            k->pos.y = k->gnd = 0;
            V3 t = track_tangent(s);
            k->heading = atan2f(t.x, t.z);
            k->vel = v3(0, 0, 0);
            k->speed = 0;
            k->skill *= 0.82f;
            track_locate(k->pos, (int)(s / track.ds), &k->tp);
            k->progress = k->tp.s;
            cpu++;
        }
        k->lap = 0;
        k->checkpoint = true;
        k->grounded = true;
    }
}
