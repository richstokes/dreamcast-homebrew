/* Ridgeline recovery (truck.h): where a truck that is stuck, on its roof
   or drowned is put back on its wheels.

   Near a trail that is the trail, a little way back down it. Away from
   the trails it is where the truck lies, unless that is in water or on
   top of something: then it is the nearest safe ground, which the level
   marks vertex by vertex (LVL_SAFE: level, dry, clear, and joined to a
   trail by land, so never an island or a bank under a cliff). The nearest
   dry ground was the old rule, and the nearest dry ground to a truck in a
   river is the foot of the canyon wall. */
#include "truck.h"
#include "terrain.h"

/* Water over the ground anywhere under a truck standing at (x, z): its
   middle and a ring at about its corners. */
static const float ring[9][2] = {{0, 0}, {3.5f, 0}, {-3.5f, 0}, {0, 3.5f}, {0, -3.5f},
                                 {2.5f, 2.5f}, {-2.5f, 2.5f}, {2.5f, -2.5f}, {-2.5f, -2.5f}};

static bool spot_wet(float x, float z) {
    for(int k = 0; k < 9; k++) {
        float px = x + ring[k][0], pz = z + ring[k][1];
        if(water_at(px, pz) > terrain_height(px, pz)) return true;
    }
    return false;
}

/* A wall, a boulder or a deck under it there: the wheels would be stood
   on top (truck_place sits them on whatever is highest). */
static bool spot_perched(float x, float z) {
    for(int k = 0; k < 9; k++) {
        float px = x + ring[k][0] * 0.6f, pz = z + ring[k][1] * 0.6f;
        if(ground_at(px, pz, NULL, NULL) > terrain_height(px, pz) + 0.3f) return true;
    }
    return false;
}

static bool spot_clear(const Truck *t, float x, float z) {
    for(int o = 0; o < game.ntrucks; o++) {
        const Truck *u = &trucks[o];
        if(u == t || u->absent) continue;
        float dx = u->pos.x - x, dz = u->pos.z - z;
        if(dx * dx + dz * dz < 7.0f * 7.0f) return false;
    }
    return true;
}

/* The nearest safe ground to at, clear of the other trucks, facing away
   from where the truck was. False if the level has none. */
static bool recover_safe(Truck *t, V3 at) {
    float best = 1e18f;
    int bi = -1, bj = 0;
    for(int j = 0; j < LEVEL_N; j++) {
        const LvlLight *row = &lvl_light[j * LEVEL_N];
        float z = LEVEL_ORG + j * LEVEL_D, dz = z - at.z;
        if(dz * dz >= best) continue;
        for(int i = 0; i < LEVEL_N; i++) {
            if(!(row[i].flags & LVL_SAFE)) continue;
            float x = LEVEL_ORG + i * LEVEL_D, dx = x - at.x, dy = lvl_h(i, j) - at.y;
            float d = dx * dx + dz * dz + dy * dy;
            if(d >= best || !spot_clear(t, x, z)) continue;
            best = d;
            bi = i;
            bj = j;
        }
    }
    if(bi < 0) return false;
    float x = LEVEL_ORG + bi * LEVEL_D, z = LEVEL_ORG + bj * LEVEL_D;
    float dx = x - at.x, dz = z - at.z;
    truck_place(t, v3(x, 0, z), dx * dx + dz * dz > 1.0f ? atan2f(dx, dz) : truck_heading(t));
    return true;
}

/* here: at is a place to stand it whatever is there but water (a
   viewpoint); else it is where the truck lies. */
static void recover_near(Truck *t, V3 at, int only_route, bool here) {
    float best = 1e18f;
    int br = 0, bp = 0;
    for(int r = 0; r < LVL_ROUTES; r++) {
        if(only_route >= 0 && r != only_route) continue;
        for(int p = 0; p < lvl_routes[r].n; p++) {
            const lvl_pt_t *q = &lvl_routes[r].pts[p];
            float dx = q->x - at.x, dz = q->z - at.z, d = dx * dx + dz * dz + (q->y - at.y) * (q->y - at.y);
            if(d < best) {
                best = d;
                br = r;
                bp = p;
            }
        }
    }
    if(best > 60.0f * 60.0f) {
        /* Far from any route: stand it up where it is, or on safe ground
           if it is in a lake or a river, or lying on a wall. */
        if(!spot_wet(at.x, at.z) && (here || !spot_perched(at.x, at.z))) {
            truck_place(t, at, truck_heading(t));
            return;
        }
        if(recover_safe(t, at)) return;
        /* None in this level: back to the nearest route below. */
    }
    const lvl_route_t *r = &lvl_routes[br];
    /* A little way back down the route, out of the water and clear of the
       other trucks. Clear of trucks gives way to dry if both can't be had
       close by; if the route back is all water, look ahead instead. */
    int start = bp > 2 ? bp - 2 : 0, p = -1, dry = -1;
    for(int q = start; q >= 0 && p < 0; q--) {
        if(spot_wet(r->pts[q].x, r->pts[q].z)) continue;
        if(dry < 0) dry = q;
        if(spot_clear(t, r->pts[q].x, r->pts[q].z)) p = q;
        else if(q < dry - 12) break;
    }
    if(p < 0) p = dry;
    for(int q = start + 1; p < 0 && q < r->n; q++)
        if(!spot_wet(r->pts[q].x, r->pts[q].z)) p = q;
    if(p < 0) p = start;
    int p2 = p + 1 < r->n ? p + 1 : p;
    float h = atan2f(r->pts[p2].x - r->pts[p].x, r->pts[p2].z - r->pts[p].z);
    truck_place(t, v3(r->pts[p].x, 0, r->pts[p].z), h);
}

void truck_recover(Truck *t) {
    recover_near(t, t->pos, -1, false);
}

void truck_recover_on(Truck *t, int only_route) {
    recover_near(t, t->pos, only_route, false);
}

void truck_recover_at(Truck *t, V3 at) {
    recover_near(t, at, -1, true);
}
