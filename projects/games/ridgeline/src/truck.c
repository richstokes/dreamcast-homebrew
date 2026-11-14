/* Ridgeline truck physics (truck.h).

   Each display frame runs SUBSTEPS steps of 1/240 s. A step:

   1. Suspension. Each wheel hangs from its droop position along the body's
      up axis. The tyre's profile is sampled at five points along its
      rolling direction (and two across it): the deepest penetration of the
      ground (or a boulder, log or ledge top) into the tyre's circle sets
      the compression. When that point is ahead of or behind the hub, the
      contact normal leans toward the hub, so a ledge pushes back on the
      wheel until it climbs it, as a real tyre does.
   2. Tyres. A bristle model per tyre and axis: the contact patch deflects
      with the sliding speed and relaxes as the tyre rolls (relaxation
      length ~0.4 m), so it holds a parked truck on a slope without creep
      and gives slip-angle behaviour at speed. Force is capped by the
      friction circle at the surface's mu times the load, dropping to a
      sliding value once it breaks away.
   3. Driveline. The wheel speeds are solved implicitly (the tyres are far
      too stiff for an explicit step), in coordinates set by the drive
      mode: 2WD drives the rear axle through an open diff; 4WD locks the
      front and rear shafts together with open diffs on each axle; LOCK
      adds low range and locks both axle diffs. The engine's inertia joins
      the driven shaft once the torque converter couples it.
   4. Chassis. Points round the body's box collide with the ground and the
      obstacles (spring-damper with friction); water gives buoyancy, drag
      and, over the air intake, a stall.
   5. Rigid-body integration, semi-implicit Euler with the inertia tensor in
      body axes and the axes re-orthonormalised every step. */
#include "truck.h"
#include "terrain.h"
#include "engine/prof.h"
#if __has_include("trucks.h")
#include "trucks.h"
#define HAVE_TRUCK_MODELS 1
#endif

Truck trucks[MAX_TRUCKS];

#define SUBSTEPS 4
#define RPM_PER_RADS 9.5492966f   /* 60 / 2 pi */

/* ---- Specs ---- */

void truck_spec(TruckSpec *s, int model) {
    memset(s, 0, sizeof(*s));
    s->grip = 1.0f;
    s->damp_bump = 0.28f;
    s->damp_rebound = 0.45f;
    s->fade = 0.30f;
    if(model == 0) {
        /* CASCADE 84: an early-80s compact pickup. A 2.4 L four, light,
           short wheelbase, solid front axle. */
        s->name = "CASCADE 84";
        s->mass = 1550.0f;
        s->inertia = v3(2500.0f, 2600.0f, 640.0f);
        s->cog_y = 0.78f;
        s->cog_z = 0.27f;    /* ~60% on the front wheels, as an empty pickup */
        s->lsd = 110.0f;
        s->radius = 0.40f;
        s->width = 0.27f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.72f : -0.72f, s->radius, (i < 2) ? 1.31f : -1.31f);
        s->travel = 0.30f;
        s->sag = 0.36f;
        s->wheel_inertia = 1.6f;
        s->max_steer = 0.62f;
        s->body_lo[0] = -0.84f; s->body_lo[1] = 0.48f; s->body_lo[2] = -2.25f;
        s->body_hi[0] = 0.84f; s->body_hi[1] = 1.80f; s->body_hi[2] = 2.15f;
        s->wade = 0.62f;
        s->idle_rpm = 800.0f;
        s->redline_rpm = 5000.0f;
        s->peak_torque = 192.0f;
        s->peak_rpm = 2800.0f;
        s->gears = 5;
        static const float r[5] = {3.95f, 2.33f, 1.42f, 1.00f, 0.85f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 4.18f;
        s->final_drive = 4.10f;
        s->low_range = 2.28f;
        s->brake_torque = 1500.0f;
    }
    else if(model == 1) {
        /* KESTREL: a modern two-door 4x4. Twin-turbo V6, long-travel
           suspension, 35 inch tyres, a crawl ratio to match. */
        s->name = "KESTREL";
        s->mass = 2050.0f;
        s->inertia = v3(3500.0f, 3600.0f, 900.0f);
        s->cog_y = 0.86f;
        s->cog_z = 0.08f;    /* ~53% front */
        s->lsd = 140.0f;
        s->radius = 0.45f;
        s->width = 0.32f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.82f : -0.82f, s->radius, (i < 2) ? 1.28f : -1.28f);
        s->travel = 0.34f;
        s->sag = 0.36f;
        s->wheel_inertia = 2.4f;
        s->max_steer = 0.60f;
        s->body_lo[0] = -0.95f; s->body_lo[1] = 0.58f; s->body_lo[2] = -2.15f;
        s->body_hi[0] = 0.95f; s->body_hi[1] = 1.92f; s->body_hi[2] = 2.15f;
        s->wade = 0.85f;
        s->idle_rpm = 700.0f;
        s->tcs = true;
        s->redline_rpm = 5800.0f;
        s->peak_torque = 540.0f;
        s->peak_rpm = 3100.0f;
        s->gears = 6;
        static const float r[6] = {4.70f, 2.98f, 2.15f, 1.60f, 1.24f, 1.00f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 4.87f;
        s->final_drive = 4.46f;
        s->low_range = 3.06f;
        s->brake_torque = 2100.0f;
    }
    else if(model == 2) {
        /* BOBCAT: a short two-door 4x4 with the top off. A 4.0 L straight
           six, solid axles both ends, tight steering and a 4:1 transfer
           case, the deepest of the three (the Kestrel's lower gearbox
           still crawls slower overall). */
        s->name = "BOBCAT";
        s->mass = 1800.0f;
        s->inertia = v3(2700.0f, 2800.0f, 760.0f);
        s->cog_y = 0.80f;
        s->cog_z = 0.12f;    /* ~55% front */
        s->lsd = 120.0f;
        s->radius = 0.42f;
        s->width = 0.30f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.80f : -0.80f, s->radius, (i < 2) ? 1.21f : -1.21f);
        s->travel = 0.36f;
        s->sag = 0.36f;
        s->wheel_inertia = 1.9f;
        s->max_steer = 0.66f;
        s->body_lo[0] = -0.85f; s->body_lo[1] = 0.55f; s->body_lo[2] = -2.05f;
        s->body_hi[0] = 0.85f; s->body_hi[1] = 1.85f; s->body_hi[2] = 1.95f;
        s->wade = 0.78f;
        s->idle_rpm = 750.0f;
        s->redline_rpm = 5200.0f;
        s->peak_torque = 320.0f;
        s->peak_rpm = 3000.0f;
        s->gears = 5;
        static const float r[5] = {3.59f, 2.19f, 1.41f, 1.00f, 0.83f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 3.16f;
        s->final_drive = 4.10f;
        s->low_range = 4.00f;
        s->brake_torque = 1800.0f;
    }
    else if(model == 3) {
        /* FFR: a mid-90s midsize extended-cab pickup with a long bed. A
           3.0 L pushrod V6 that wants revs, a 3.2 m wheelbase that rides
           washboard well but beaches on sharp crests and turns wide, and
           ~58% of the weight on the front with the bed empty. */
        s->name = "FFR";
        s->mass = 1750.0f;
        s->inertia = v3(3900.0f, 4000.0f, 720.0f);
        s->cog_y = 0.80f;
        s->cog_z = 0.25f;
        s->lsd = 110.0f;
        s->radius = 0.40f;
        s->width = 0.27f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.735f : -0.735f, s->radius, (i < 2) ? 1.59f : -1.59f);
        s->travel = 0.30f;
        s->sag = 0.36f;
        s->wheel_inertia = 1.7f;
        s->max_steer = 0.58f;
        s->body_lo[0] = -0.84f; s->body_lo[1] = 0.53f; s->body_lo[2] = -2.67f;
        s->body_hi[0] = 0.84f; s->body_hi[1] = 1.80f; s->body_hi[2] = 2.43f;
        s->wade = 0.66f;
        s->idle_rpm = 750.0f;
        s->redline_rpm = 5400.0f;
        s->peak_torque = 250.0f;
        s->peak_rpm = 3200.0f;
        s->gears = 5;
        static const float r[5] = {3.40f, 2.08f, 1.40f, 1.00f, 0.79f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 3.20f;
        s->final_drive = 4.10f;
        s->low_range = 2.72f;
        s->brake_torque = 1650.0f;
    }
    else if(model == 4) {
        /* TACO: a 2026 midsize double-cab pickup. A 2.4 L turbo four with
           its torque low down, an eight-speed automatic, independent front
           suspension and traction control. A long wheelbase (3.35 m):
           steady at speed, wide in the hairpins, and quick to ground its
           belly on a sharp crest. */
        s->name = "TACO";
        s->mass = 2050.0f;
        s->inertia = v3(4900.0f, 5000.0f, 950.0f);
        s->cog_y = 0.84f;
        s->cog_z = 0.20f;    /* ~56% front */
        s->lsd = 130.0f;
        s->radius = 0.42f;
        s->width = 0.29f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.82f : -0.82f, s->radius, (i < 2) ? 1.675f : -1.675f);
        s->travel = 0.32f;
        s->sag = 0.36f;
        s->wheel_inertia = 2.0f;
        s->max_steer = 0.60f;
        s->body_lo[0] = -0.94f; s->body_lo[1] = 0.57f; s->body_lo[2] = -2.81f;
        s->body_hi[0] = 0.94f; s->body_hi[1] = 1.90f; s->body_hi[2] = 2.62f;
        s->wade = 0.72f;
        s->idle_rpm = 700.0f;
        s->tcs = true;
        s->redline_rpm = 5800.0f;
        s->peak_torque = 430.0f;
        s->peak_rpm = 2000.0f;
        s->gears = 8;
        static const float r[8] = {4.41f, 2.81f, 1.95f, 1.51f, 1.27f, 1.00f, 0.79f, 0.65f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 3.99f;
        s->final_drive = 3.58f;
        s->low_range = 2.57f;
        s->brake_torque = 2100.0f;
    }
    else if(model == 5) {
        /* GOOSE: a four-seat sport side-by-side. Half the weight of the
           trucks on a 3.43 m wheelbase, a 900 cc turbo
           triple behind the seats (~45% of the weight on the front) that
           revs to 8000, half a metre of wheel travel on long arms with
           dampers to match, and soft knobbly tyres that grip better than
           anything else here. The belt drive's range is stood in for by
           six close gears. */
        s->name = "GOOSE";
        s->mass = 1050.0f;
        s->inertia = v3(2600.0f, 2750.0f, 450.0f);
        s->cog_y = 0.58f;
        s->cog_z = -0.17f;
        s->lsd = 90.0f;
        s->radius = 0.40f;
        s->width = 0.27f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.78f : -0.78f, s->radius, (i < 2) ? 1.715f : -1.715f);
        s->travel = 0.50f;
        s->sag = 0.38f;
        s->damp_bump = 0.36f;
        s->damp_rebound = 0.60f;
        s->grip = 1.12f;
        s->wheel_inertia = 1.2f;
        s->max_steer = 0.62f;
        s->body_lo[0] = -0.90f; s->body_lo[1] = 0.45f; s->body_lo[2] = -2.10f;
        s->body_hi[0] = 0.90f; s->body_hi[1] = 1.78f; s->body_hi[2] = 2.20f;
        s->wade = 0.70f;
        s->idle_rpm = 1200.0f;
        s->redline_rpm = 8000.0f;
        s->peak_torque = 175.0f;
        s->peak_rpm = 6000.0f;
        s->gears = 6;
        static const float r[6] = {5.60f, 4.20f, 3.30f, 2.70f, 2.30f, 2.00f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 5.00f;
        s->final_drive = 4.30f;
        s->low_range = 1.90f;
        s->brake_torque = 1300.0f;
    }
    else if(model == 6) {
        /* ONE TEN: a mid-80s British long-wheelbase station wagon. A 3.5 L
           carburettor V8 with its torque at 2500, out of breath past 4000
           (fade puts its 85 kW there) and the least power for its weight
           here. Coil-sprung live
           axles with long travel keep the tyres on the ground, the weight
           sits evenly on them (~48% front), and it is tall on a narrow
           track with a poor steering lock. The transfer case's 1.41 high
           range is in the final drive; its 3.32 low is what is left. */
        s->name = "ONE TEN";
        s->mass = 2000.0f;
        s->inertia = v3(4000.0f, 4100.0f, 1000.0f);
        s->cog_y = 0.84f;
        s->cog_z = -0.05f;
        s->lsd = 100.0f;
        s->radius = 0.40f;
        s->width = 0.22f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.745f : -0.745f, s->radius, (i < 2) ? 1.397f : -1.397f);
        s->travel = 0.36f;
        s->sag = 0.38f;
        s->wheel_inertia = 1.8f;
        s->max_steer = 0.54f;
        s->body_lo[0] = -0.90f; s->body_lo[1] = 0.52f; s->body_lo[2] = -2.55f;
        s->body_hi[0] = 0.90f; s->body_hi[1] = 2.02f; s->body_hi[2] = 2.05f;
        s->wade = 0.70f;
        s->idle_rpm = 650.0f;
        s->redline_rpm = 5000.0f;
        s->peak_torque = 251.0f;
        s->peak_rpm = 2500.0f;
        s->fade = 0.53f;
        s->gears = 5;
        static const float r[5] = {3.65f, 2.18f, 1.44f, 1.00f, 0.79f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 3.82f;
        s->final_drive = 4.99f;
        s->low_range = 2.35f;
        s->brake_torque = 1850.0f;
    }
    else {
        /* EAGLE: a 2026 full-size desert-running crew cab, to its maker's
           2026 figures. A high-output 3.5 L twin-turbo V6 (691 N m at
           3500; fade puts its 336 kW near 5250) through a ten-speed
           automatic, 14 and 15 inches of wheel travel with firmer dampers
           than the other pickups, 35 inch tyres and traction control. The
           biggest and heaviest thing here: 2.2 m across the flares on a
           3.69 m wheelbase, so it is the steadiest at speed, the widest in
           the hairpins, and a narrow trail is narrower for it. */
        s->name = "EAGLE";
        s->mass = 2600.0f;
        s->inertia = v3(7500.0f, 7700.0f, 1500.0f);
        s->cog_y = 0.90f;
        s->cog_z = 0.24f;    /* ~56% front */
        s->lsd = 150.0f;
        s->radius = 0.437f;
        s->width = 0.315f;
        for(int i = 0; i < 4; i++)
            s->wheel[i] = v3((i & 1) ? 0.94f : -0.94f, s->radius, (i < 2) ? 1.846f : -1.846f);
        s->travel = 0.37f;
        s->sag = 0.37f;
        s->damp_bump = 0.32f;
        s->damp_rebound = 0.52f;
        s->wheel_inertia = 2.6f;
        s->max_steer = 0.60f;
        s->body_lo[0] = -1.10f; s->body_lo[1] = 0.55f; s->body_lo[2] = -3.12f;
        s->body_hi[0] = 1.10f; s->body_hi[1] = 2.02f; s->body_hi[2] = 2.78f;
        s->wade = 0.81f;
        s->idle_rpm = 650.0f;
        s->tcs = true;
        s->redline_rpm = 6200.0f;
        s->peak_torque = 691.0f;
        s->peak_rpm = 3500.0f;
        s->fade = 0.28f;
        s->gears = 10;
        static const float r[10] = {4.70f, 2.99f, 2.15f, 1.77f, 1.52f, 1.26f, 1.00f, 0.85f, 0.69f, 0.64f};
        memcpy(s->ratio, r, sizeof(r));
        s->reverse = 4.87f;
        s->final_drive = 4.10f;
        s->low_range = 2.64f;
        s->brake_torque = 2800.0f;
    }
#ifdef HAVE_TRUCK_MODELS
    const rl_truck_info_t *ti = &rl_truck_info[model];
    s->radius = ti->wheel_radius;
    s->width = ti->wheel_width;
    for(int i = 0; i < 4; i++) s->wheel[i] = v3(ti->wheel_pos[i][0], ti->wheel_pos[i][1], ti->wheel_pos[i][2]);
    for(int a = 0; a < 3; a++) {
        s->body_lo[a] = ti->body_min[a];
        s->body_hi[a] = ti->body_max[a];
    }
    /* The body's box sits above the wheels' bottoms. */
    s->body_lo[1] = fmaxf(s->body_lo[1], s->radius * 1.15f);
#endif
    /* What another truck hits: the body and the wheels, from just off the
       ground to the roof. */
    float hx = fmaxf(s->body_hi[0], -s->body_lo[0]), zlo = s->body_lo[2], zhi = s->body_hi[2];
    for(int i = 0; i < 4; i++) {
        hx = fmaxf(hx, fabsf(s->wheel[i].x) + s->width * 0.5f);
        zlo = fminf(zlo, s->wheel[i].z - s->radius);
        zhi = fmaxf(zhi, s->wheel[i].z + s->radius);
    }
    s->hull_c[0] = 0;
    s->hull_c[1] = (0.12f + s->body_hi[1]) * 0.5f;
    s->hull_c[2] = (zlo + zhi) * 0.5f;
    s->hull_h[0] = hx;
    s->hull_h[1] = (s->body_hi[1] - 0.12f) * 0.5f;
    s->hull_h[2] = (zhi - zlo) * 0.5f;
    /* Low range holds the automatic to the gears whose redline is under
       LOW_RANGE_TOP_MPS, as modern 4L modes do: 2nd in the pickup (~22 mph),
       4th in the Kestrel (~28 mph), 3rd in the Bobcat (~22 mph), 2nd in the
       FFR and the Taco (~22 mph), 3rd in the Goose (~27 mph), the One Ten
       and the Eagle (~28 mph). */
    s->low_top_gear = 1;
    for(int g = 1; g <= s->gears; g++) {
        float v = s->redline_rpm / RPM_PER_RADS / (s->ratio[g - 1] * s->final_drive * s->low_range) * s->radius;
        if(v <= LOW_RANGE_TOP_MPS) s->low_top_gear = g;
    }
    /* Springs: each corner's share of the weight (more on the front, with
       the centre of gravity ahead of mid-wheelbase) compresses sag of the
       travel, so the truck sits level. Dampers about a third and a half of
       critical, more on the Eagle and the Goose. */
    float zf = s->wheel[WFL].z, zr = s->wheel[WRL].z;
    float front = clampf((s->cog_z - zr) / (zf - zr), 0.3f, 0.7f);
    for(int a = 0; a < 2; a++) {
        float corner = s->mass * 0.5f * (a ? 1.0f - front : front);
        s->k[a] = corner * GRAVITY / (s->sag * s->travel);
        float crit = 2.0f * sqrtf(s->k[a] * corner);
        s->c_bump[a] = s->damp_bump * crit;
        s->c_rebound[a] = s->damp_rebound * crit;
    }
    s->arb_front = s->k[0] * 0.35f;
    s->arb_rear = s->k[1] * 0.25f;
}

/* ---- Pose helpers ---- */

static inline V3 body_to_world(const Truck *t, V3 b) {
    return vadd(t->pos, vadd(vadd(vscale(t->ax[0], b.x), vscale(t->ax[1], b.y)), vscale(t->ax[2], b.z)));
}

V3 truck_to_world(const Truck *t, V3 m) {
    return body_to_world(t, v3(m.x, m.y - t->spec.cog_y, m.z - t->spec.cog_z));
}

V3 truck_dir_to_world(const Truck *t, V3 d) {
    return vadd(vadd(vscale(t->ax[0], d.x), vscale(t->ax[1], d.y)), vscale(t->ax[2], d.z));
}

float truck_heading(const Truck *t) {
    return atan2f(t->ax[2].x, t->ax[2].z);
}

void truck_place(Truck *t, V3 base, float heading) {
    int model = t->model, livery = t->livery, mode = t->mode;
    bool cpu = t->cpu, tcs_off = t->tcs_off, remote = t->remote, absent = t->absent;
    uint8_t lights = t->lights;
    memset(t, 0, sizeof(*t));
    t->model = model;
    t->livery = livery;
    t->cpu = cpu;
    t->remote = remote;
    t->absent = absent;
    t->lights = lights;
    t->tcs_off = tcs_off;
    truck_spec(&t->spec, model);
    /* Sit it on the ground under its four wheels: on a steep trail a level
       truck would start with its body in the hill. */
    V3 f = heading_fwd(heading), r = heading_right(heading);
    const TruckSpec *S = &t->spec;
    V3 g[4];
    float gy = 0;
    for(int i = 0; i < 4; i++) {
        V3 p = vadd(vadd(v3(base.x, 0, base.z), vscale(r, S->wheel[i].x)), vscale(f, S->wheel[i].z));
        p.y = ground_at(p.x, p.z, NULL, NULL);
        g[i] = p;
        gy += p.y * 0.25f;
    }
    V3 along = vsub(vscale(vadd(g[WFL], g[WFR]), 0.5f), vscale(vadd(g[WRL], g[WRR]), 0.5f));
    V3 across = vsub(vscale(vadd(g[WFR], g[WRR]), 0.5f), vscale(vadd(g[WFL], g[WRL]), 0.5f));
    V3 up = vnorm(vcross(along, across));
    if(up.y < 0) up = vscale(up, -1.0f);
    if(up.y < 0.5f) up = v3(0, 1, 0);
    t->ax[1] = up;
    t->ax[2] = vnorm(vsub(f, vscale(up, vdot(f, up))));
    t->ax[0] = vcross(up, t->ax[2]);
    t->pos = vadd(vadd(v3(base.x, gy, base.z), vscale(up, t->spec.cog_y + 0.08f)), vscale(t->ax[2], t->spec.cog_z));
    t->gear = 1;
    t->rpm = t->spec.idle_rpm;
    t->mode = mode;
    t->tc = 1.0f;
    for(int i = 0; i < 4; i++) t->wh[i].comp = t->wh[i].comp_prev = t->spec.sag * t->spec.travel;
}

int truck_set_mode(Truck *t, int mode) {
    if(mode < 0 || mode >= DRIVE_COUNT || mode == t->mode) return MODE_SAME;
    bool range = (mode == DRIVE_LOCK) != (t->mode == DRIVE_LOCK);
    /* 2WD and 4WD change on the move; low range, like a real transfer
       case, only when nearly stopped. */
    if(range && fabsf(vdot(t->vel, t->ax[2])) > LOW_RANGE_SHIFT_MPS) return MODE_TOO_FAST;
    t->mode = mode;
    if(range) {
        /* Into or out of low range the box goes back to first. */
        if(t->gear > 1) t->gear = 1;
        t->shift_t = 0.3f;
    }
    return MODE_CHANGED;
}

int truck_top_gear(const Truck *t) {
    return t->mode == DRIVE_LOCK ? t->spec.low_top_gear : t->spec.gears;
}

/* ---- Engine ---- */

static float engine_torque(const TruckSpec *s, float rpm) {
    float x;
    if(rpm < s->peak_rpm) {
        x = clampf((rpm - s->idle_rpm * 0.6f) / (s->peak_rpm - s->idle_rpm * 0.6f), 0, 1);
        return s->peak_torque * (0.55f + 0.45f * x * (2.0f - x));
    }
    x = clampf((rpm - s->peak_rpm) / (s->redline_rpm - s->peak_rpm), 0, 1.2f);
    return s->peak_torque * (1.0f - s->fade * x * x);
}

static float engine_friction(const TruckSpec *s, float rpm) {
    return s->peak_torque * (0.06f + 0.00004f * rpm);
}

static float total_ratio(const Truck *t) {
    const TruckSpec *s = &t->spec;
    float r = t->gear < 0 ? -s->reverse : s->ratio[t->gear - 1];
    r *= s->final_drive;
    if(t->mode == DRIVE_LOCK) r *= s->low_range;
    return r;
}

/* Is world point p inside u's body box? Its depth and the way out. */
/* Two trucks' hulls (oriented boxes) against each other, by separating
   axes. Only horizontal axes are tried, so the push between them is always
   sideways along the ground: trucks shove and spin each other but can't be
   pushed up onto one another and rolled. */
typedef struct {
    V3 n;          /* from t towards u, horizontal */
    float depth;
    V3 p;          /* where they meet */
} PairHit;

static float hull_reach(const Truck *t, V3 d) {
    const float *h = t->spec.hull_h;
    return h[0] * fabsf(vdot(t->ax[0], d)) + h[1] * fabsf(vdot(t->ax[1], d)) + h[2] * fabsf(vdot(t->ax[2], d));
}

static bool hull_has(const Truck *u, V3 p) {
    const TruckSpec *S = &u->spec;
    V3 d = vsub(p, truck_to_world(u, v3(S->hull_c[0], S->hull_c[1], S->hull_c[2])));
    for(int a = 0; a < 3; a++)
        if(fabsf(vdot(d, u->ax[a])) > S->hull_h[a]) return false;
    return true;
}

static bool truck_pair(const Truck *t, const Truck *u, PairHit *h) {
    const TruckSpec *St = &t->spec, *Su = &u->spec;
    V3 ct = truck_to_world(t, v3(St->hull_c[0], St->hull_c[1], St->hull_c[2]));
    V3 cu = truck_to_world(u, v3(Su->hull_c[0], Su->hull_c[1], Su->hull_c[2]));
    V3 dc = vsub(cu, ct);
    V3 up = v3(0, 1, 0);
    if(hull_reach(t, up) + hull_reach(u, up) <= fabsf(dc.y)) return false;
    float best = 1e9f;
    for(int k = 0; k < 6; k++) {
        const Truck *T = k < 3 ? t : u;
        V3 d = T->ax[(k % 3 == 0) ? 0 : (k % 3 == 1) ? 2 : 1];
        d.y = 0;
        float l = vlen(d);
        if(l < 0.25f) continue;    /* the up axis, until a truck is well over */
        d = vscale(d, 1.0f / l);
        float sep = vdot(dc, d);
        float ov = hull_reach(t, d) + hull_reach(u, d) - fabsf(sep);
        if(ov <= 0) return false;
        if(ov < best) {
            best = ov;
            h->n = sep >= 0 ? d : vscale(d, -1.0f);
        }
    }
    h->depth = best;
    /* Where: the corners of each inside the other, or between the two. */
    V3 sum = v3(0, 0, 0);
    int cnt = 0;
    for(int side = 0; side < 2; side++) {
        const Truck *A = side ? u : t, *B = side ? t : u;
        const float *c = A->spec.hull_c, *hh = A->spec.hull_h;
        for(int k = 0; k < 8; k++) {
            V3 p = truck_to_world(A, v3(c[0] + ((k & 1) ? hh[0] : -hh[0]), c[1] + ((k & 2) ? hh[1] : -hh[1]),
                                        c[2] + ((k & 4) ? hh[2] : -hh[2])));
            if(hull_has(B, p)) {
                sum = vadd(sum, p);
                cnt++;
            }
        }
    }
    h->p = cnt ? vscale(sum, 1.0f / cnt) : vscale(vadd(ct, cu), 0.5f);
    return true;
}

float truck_overlap(const Truck *t, const Truck *u) {
    PairHit h;
    return truck_pair(t, u, &h) ? h.depth : 0.0f;
}

/* ---- The driveline solve ---- */

/* Solve n x n (n <= 4) A x = b in place by Gaussian elimination. */
static void solve(float A[4][4], float *b, int n) {
    for(int c = 0; c < n; c++) {
        int p = c;
        for(int r = c + 1; r < n; r++)
            if(fabsf(A[r][c]) > fabsf(A[p][c])) p = r;
        if(p != c) {
            for(int k = 0; k < n; k++) {
                float tmp = A[c][k];
                A[c][k] = A[p][k];
                A[p][k] = tmp;
            }
            float tmp = b[c];
            b[c] = b[p];
            b[p] = tmp;
        }
        float d = A[c][c];
        if(fabsf(d) < 1e-9f) d = 1e-9f;
        for(int r = c + 1; r < n; r++) {
            float f = A[r][c] / d;
            for(int k = c; k < n; k++) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for(int c = n - 1; c >= 0; c--) {
        float s = b[c];
        for(int k = c + 1; k < n; k++) s -= A[c][k] * b[k];
        b[c] = s / (fabsf(A[c][c]) < 1e-9f ? 1e-9f : A[c][c]);
    }
}

/* Generalised coordinates for the mode: wheel speeds w = S q, and the
   driven shaft's speed = shaft . q. Returns the dimension. */
static int mode_coords(int mode, float S[4][4], float shaft[4]) {
    memset(S, 0, sizeof(float) * 16);
    memset(shaft, 0, sizeof(float) * 4);
    switch(mode) {
        case DRIVE_2WD:
            /* q = rear mean, rear difference, front left, front right. */
            S[WRL][0] = 1; S[WRL][1] = 1;
            S[WRR][0] = 1; S[WRR][1] = -1;
            S[WFL][2] = 1;
            S[WFR][3] = 1;
            shaft[0] = 1;
            return 4;
        case DRIVE_4WD:
            /* q = shaft, front difference, rear difference. */
            S[WFL][0] = 1; S[WFL][1] = 1;
            S[WFR][0] = 1; S[WFR][1] = -1;
            S[WRL][0] = 1; S[WRL][2] = 1;
            S[WRR][0] = 1; S[WRR][2] = -1;
            shaft[0] = 1;
            return 3;
        default:
            for(int i = 0; i < 4; i++) S[i][0] = 1;
            shaft[0] = 1;
            return 1;
    }
}

/* ---- One step ---- */

typedef struct {
    V3 n;          /* contact normal */
    V3 cp;         /* contact point */
    V3 fw;         /* rolling direction */
    float N;       /* normal force along n */
    float lf_v, ls_v;   /* contact velocity along and across */
    V3 lf, ls;
    float mu, rr, slide;
} Contact;

static const V3 body_pts_dummy;

static void step(Truck *t, float dt) {
    const TruckSpec *S = &t->spec;
    V3 R = t->ax[0], U = t->ax[1], F = t->ax[2];
    V3 force = v3(0, -GRAVITY * S->mass, 0), torque = v3(0, 0, 0);
    Contact ct[4];
    float comp_v[4];
    bool any_contact = false;

    /* 1. Suspension: where each tyre meets the ground. */
    for(int i = 0; i < 4; i++) {
        Wheel *w = &t->wh[i];
        V3 m = S->wheel[i];
        V3 droop = truck_to_world(t, v3(m.x, m.y - S->sag * S->travel, m.z));
        float st = w->steer;
        V3 fw = vadd(vscale(F, cosf(st)), vscale(R, sinf(st)));
        V3 fh = vnorm(v3(fw.x, 0, fw.z));
        V3 rh = v3(fh.z, 0, -fh.x);
        float best = -1e9f, best_d = 0;
        V3 best_n = v3(0, 1, 0), best_q = droop;
        int best_s = SURF_GRASS;
        static const float off[7][2] = {{0, 0}, {-0.65f, 0}, {-0.33f, 0}, {0.33f, 0}, {0.65f, 0}, {0, -0.4f}, {0, 0.4f}};
        /* The highest a tyre can stand: its travel and most of a radius
           above the tyre's bottom at full droop. Taller is a wall. */
        float reach = droop.y + S->travel - 0.2f * S->radius;
        for(int k = 0; k < 7; k++) {
            float d = off[k][0] * S->radius;
            V3 q = vadd(vmad(droop, fh, d), vscale(rh, off[k][1] * S->width));
            V3 n;
            int sf;
            float g = ground_below(q.x, q.z, reach, &n, &sf) - surf_props[sf].sink;
            float pen = g - (droop.y - sqrtf(S->radius * S->radius - d * d));
            if(pen > best) {
                best = pen;
                best_n = n;
                best_q = v3(q.x, g, q.z);
                best_s = sf;
                best_d = d;
            }
        }
        float c = 0;
        if(best > 0) c = best / fmaxf(U.y, 0.4f);
        float bump = 0;
        if(c > S->travel) {
            bump = c - S->travel;
            c = S->travel;
        }
        w->comp_prev = w->comp;
        w->comp = c;
        comp_v[i] = (c - w->comp_prev) / dt;
        w->centre = vmad(droop, U, c);
        w->contact = best > 0;
        w->surf = best_s;
        float wl = water_at(best_q.x, best_q.z);
        w->water = wl > -999.0f ? fmaxf(0, wl - best_q.y) : 0;
        Contact *k = &ct[i];
        k->fw = fw;
        k->cp = best_q;
        w->cp = best_q;     /* for the effects: what the tyre throws, its track */
        if(best_d != 0) {
            /* Off the hub's foot: the normal leans from the contact toward the hub. */
            V3 radial = vnorm(vsub(w->centre, best_q));
            k->n = vnorm(vadd(vscale(best_n, 0.35f), vscale(radial, 0.65f)));
        }
        else k->n = best_n;
        k->N = 0;
        if(w->contact) {
            any_contact = true;
            int ax = i >> 1;
            float spring = S->k[ax] * c + (comp_v[i] > 0 ? S->c_bump[ax] : S->c_rebound[ax]) * comp_v[i];
            spring += bump * S->k[ax] * 8.0f;
            k->N = fmaxf(0, spring);
        }
        const SurfProps *sp = &surf_props[best_s];
        k->mu = sp->mu * S->grip * (w->water > 0.05f ? 0.85f : 1.0f);
        k->slide = sp->slide;
        k->rr = sp->rr + 0.08f * fminf(w->water, 1.0f);
    }
    /* Anti-roll bars: each axle's left/right difference, front bar off in
       low range (articulation for crawling). */
    for(int a = 0; a < 2; a++) {
        int l = a * 2, r = a * 2 + 1;
        float k = a ? S->arb_rear : (t->mode == DRIVE_LOCK ? 0.0f : S->arb_front);
        float f = k * (t->wh[l].comp - t->wh[r].comp);
        if(ct[l].N > 0 || t->wh[l].contact) ct[l].N = fmaxf(0, ct[l].N + f);
        if(ct[r].N > 0 || t->wh[r].contact) ct[r].N = fmaxf(0, ct[r].N - f);
    }

    /* 2. Tyre frames and the normal forces. */
    float Lf[4];
    for(int i = 0; i < 4; i++) {
        Contact *k = &ct[i];
        Wheel *w = &t->wh[i];
        Lf[i] = 0;
        if(!w->contact) continue;
        float nu = fmaxf(vdot(k->n, U), 0.5f);
        float N = k->N / nu;
        Lf[i] = fminf(N, S->mass * GRAVITY * 1.5f);
        V3 fn = vscale(k->n, N);
        force = vadd(force, fn);
        torque = vadd(torque, vcross(vsub(w->centre, t->pos), fn));
        k->lf = vnorm(vsub(k->fw, vscale(k->n, vdot(k->fw, k->n))));
        k->ls = vcross(k->n, k->lf);
        V3 vcp = vadd(t->vel, vcross(t->w, vsub(k->cp, t->pos)));
        k->lf_v = vdot(vcp, k->lf);
        k->ls_v = vdot(vcp, k->ls);
    }

    /* 3. Lateral tyre forces (bristles), before the driveline. */
    float Fy[4] = {0}, Fx[4] = {0};
    const float KB_LAT = 12.0f, CB_LAT = 0.25f, KB_LONG = 15.0f, CB_LONG = 0.35f, RELAX = 0.40f;
    for(int i = 0; i < 4; i++) {
        Wheel *w = &t->wh[i];
        if(!w->contact) {
            w->rl_lat = w->rl_long = 0;
            continue;
        }
        Contact *k = &ct[i];
        float kb = KB_LAT * Lf[i], cb = CB_LAT * Lf[i];
        float kappa = 1.0f / (1.0f + fabsf(k->lf_v) / RELAX * dt);
        w->rl_lat = (w->rl_lat + k->ls_v * dt) * kappa;
        Fy[i] = -(kb * w->rl_lat + cb * k->ls_v);
    }

    /* 4. The driveline: wheel speeds, implicitly. */
    float Sm[4][4], shaft[4];
    int nq = mode_coords(t->mode, Sm, shaft);
    float G = total_ratio(t);
    float shift_lock = t->shift_t > 0 ? 0.0f : 1.0f;
    float throttle = t->stalled ? 0.0f : t->throttle * t->tc;
    if(t->rpm > S->redline_rpm) throttle = 0;
    /* Shaft speed now, from the wheels. */
    float q_shaft = 0;
    {
        float wsum = 0, ws = 0;
        for(int i = 0; i < 4; i++)
            for(int c = 0; c < nq; c++)
                if(Sm[i][c] != 0 && shaft[c] != 0) {
                    wsum += t->wh[i].omega;
                    ws += 1;
                }
        q_shaft = ws > 0 ? wsum / ws : 0;
    }
    float rpm_shaft = fabsf(q_shaft * G) * RPM_PER_RADS;
    float couple_rpm = S->idle_rpm + 280.0f;
    bool coupled = shift_lock > 0 && rpm_shaft >= couple_rpm && !t->stalled;
    float I_e = S->peak_torque * 0.0006f;   /* ~0.12-0.32 kg m2 */
    float T_shaft;   /* torque at the wheels from the engine, summed */
    if(coupled) {
        float Te = throttle * engine_torque(S, t->rpm) - engine_friction(S, t->rpm);
        T_shaft = Te * G;
    }
    else {
        /* The torque converter slips: the engine spins up toward its stall
           speed and multiplies torque into the shaft; idle creeps. The
           creep fades as the brake goes on, as an automatic's neutral idle
           does at a stop: in low range it would otherwise outpull the
           brakes and the parked truck would crawl off on its own. */
        float stall = S->idle_rpm + 1500.0f;
        float target = t->stalled ? 0.0f : S->idle_rpm + throttle * (stall - S->idle_rpm);
        t->rpm = approach(t->rpm, target, dt * 5000.0f);
        float sr = clampf(rpm_shaft / fmaxf(t->rpm, 1.0f), 0, 1);
        float creep = t->stalled ? 0.0f : 0.12f * S->peak_torque * (1 - throttle) * (1 - fminf(t->brake, 1.0f));
        float Te = (throttle * engine_torque(S, t->rpm) + creep) * (1.0f + 0.9f * (1.0f - sr));
        T_shaft = Te * G * shift_lock;
    }
    float r_w = S->radius, Iw = S->wheel_inertia;
    float a[4], b[4];
    bool sat[4] = {false};
    float Fsat[4] = {0};
    float omega_new[4];
    for(int pass = 0; pass < 2; pass++) {
        for(int i = 0; i < 4; i++) {
            Wheel *w = &t->wh[i];
            Contact *k = &ct[i];
            a[i] = 0.2f;   /* bearings */
            b[i] = 0;
            float brake = t->brake;
            if(t->in.handbrake && i >= 2) brake = fmaxf(brake, 1.0f);
            float tb = brake * S->brake_torque * (i < 2 ? 1.15f : 0.85f);
            /* Implicit, as a damper that reaches full torque at 0.01 rad/s,
               so a held brake stops the wheel rather than letting it turn
               slowly against any torque. */
            a[i] += tb / fmaxf(fabsf(w->omega), 0.01f);
            if(!w->contact) continue;
            a[i] += k->rr * Lf[i] * r_w / fmaxf(fabsf(w->omega), 2.0f);
            if(sat[i]) {
                b[i] += -r_w * Fsat[i];
                continue;
            }
            float kb = KB_LONG * Lf[i], cb = CB_LONG * Lf[i];
            float kappa = 1.0f / (1.0f + fabsf(k->lf_v) / RELAX * dt);
            float K = kb * kappa * dt + cb;
            a[i] += K * r_w * r_w;
            b[i] += r_w * (kb * kappa * w->rl_long + K * k->lf_v);
        }
        float A[4][4] = {{0}}, rhs[4] = {0};
        for(int r = 0; r < nq; r++) {
            for(int c = 0; c < nq; c++) {
                float sum = 0;
                for(int i = 0; i < 4; i++) sum += Sm[i][r] * (Iw / dt + a[i]) * Sm[i][c];
                A[r][c] = sum;
            }
            float sr = 0;
            for(int i = 0; i < 4; i++) sr += Sm[i][r] * (Iw / dt * t->wh[i].omega + b[i]);
            rhs[r] = sr + shaft[r] * T_shaft;
        }
        /* The limited-slip rear: a viscous coupling between the rear wheels,
           so a spinning inside wheel still sends drive to the outside one
           and the tail can be powered round. Its coordinate is the rear
           difference (wheels at shaft +- d, so the torque on d is 4 c d). */
        int rear_d = t->mode == DRIVE_2WD ? 1 : t->mode == DRIVE_4WD ? 2 : -1;
        if(rear_d >= 0) A[rear_d][rear_d] += 4.0f * S->lsd;
        if(coupled) {
            /* The engine's inertia on the shaft, through the gearing. */
            float Ie = I_e * G * G;
            float shaft_prev = t->rpm / RPM_PER_RADS / G;
            /* Pushed past redline (rolling downhill in too low a gear), the
               engine fights back harder the further over it is: drag grows
               by OVERRUN_DRAG x peak torque per 1000 rpm over. Implicit, as
               a damper toward the redline shaft speed, so it stays stable
               however steep. */
            const float OVERRUN_DRAG = 0.5f;
            float Co = 0, shaft_red = 0;
            if(t->rpm > S->redline_rpm) {
                Co = OVERRUN_DRAG * S->peak_torque / (1000.0f / RPM_PER_RADS) * G * G;
                shaft_red = S->redline_rpm / RPM_PER_RADS / G * signf(q_shaft * G);
            }
            for(int r = 0; r < nq; r++) {
                for(int c = 0; c < nq; c++) A[r][c] += (Ie / dt + Co) * shaft[r] * shaft[c];
                rhs[r] += Ie / dt * shaft[r] * shaft_prev + Co * shaft[r] * shaft_red;
            }
        }
        solve(A, rhs, nq);
        for(int i = 0; i < 4; i++) {
            float wv = 0;
            for(int c = 0; c < nq; c++) wv += Sm[i][c] * rhs[c];
            omega_new[i] = wv;
        }
        /* Longitudinal forces with the new speeds, then the friction circle. */
        bool again = false;
        for(int i = 0; i < 4; i++) {
            Wheel *w = &t->wh[i];
            Contact *k = &ct[i];
            if(!w->contact) {
                Fx[i] = 0;
                continue;
            }
            if(sat[i]) {
                Fx[i] = Fsat[i];
                continue;
            }
            float kb = KB_LONG * Lf[i], cb = CB_LONG * Lf[i];
            float kappa = 1.0f / (1.0f + fabsf(k->lf_v) / RELAX * dt);
            float s = k->lf_v - omega_new[i] * r_w;
            float dl = (w->rl_long + s * dt) * kappa;
            float fx = -(kb * dl + cb * s);
            float cap = k->mu * Lf[i];
            float mag = sqrtf(fx * fx + Fy[i] * Fy[i]);
            if(mag > cap && pass == 0) {
                /* Breaks away: both directions slide at the lower mu. */
                /* Breaks away: the sliding force lies along the slip, so the
                   sideways force shrinks with the driving one. (Scaling only
                   the drive left a spinning tyre half its cornering grip.) */
                float slide = cap * k->slide / mag;
                Fsat[i] = fx * slide;
                Fy[i] *= slide;
                sat[i] = true;
                again = true;
            }
            else if(pass == 0) {
                Fx[i] = fx;
            }
            else {
                Fx[i] = fx;
            }
        }
        if(!again) break;
    }
    for(int i = 0; i < 4; i++) {
        Wheel *w = &t->wh[i];
        Contact *k = &ct[i];
        w->omega = clampf(omega_new[i], -300.0f, 300.0f);
        if(!w->contact) {
            w->fx = w->fy = 0;
            w->slip = 0;
            continue;
        }
        float cap = k->mu * Lf[i];
        float mag = sqrtf(Fx[i] * Fx[i] + Fy[i] * Fy[i]);
        if(mag > cap) {
            float sc = cap * k->slide / mag;
            Fx[i] *= sc;
            Fy[i] *= sc;
        }
        float kb_l = KB_LONG * Lf[i], kb_t = KB_LAT * Lf[i];
        /* Deflections that match the forces actually applied. */
        float s_long = k->lf_v - w->omega * r_w;
        float kappa = 1.0f / (1.0f + fabsf(k->lf_v) / RELAX * dt);
        float dl = (w->rl_long + s_long * dt) * kappa;
        if(fabsf(kb_l * dl) > fabsf(Fx[i]) + 1.0f) dl = -Fx[i] / fmaxf(kb_l, 1.0f);
        w->rl_long = dl;
        if(fabsf(kb_t * w->rl_lat) > fabsf(Fy[i]) + 1.0f) w->rl_lat = -Fy[i] / fmaxf(kb_t, 1.0f);
        w->slip = sqrtf(s_long * s_long + k->ls_v * k->ls_v);
        w->fx = Fx[i];
        w->fy = Fy[i];
        V3 ft = vadd(vscale(k->lf, Fx[i]), vscale(k->ls, Fy[i]));
        force = vadd(force, ft);
        torque = vadd(torque, vcross(vsub(k->cp, t->pos), ft));
    }
    /* The engine follows the shaft when coupled. */
    if(coupled) {
        float wsum = 0, ws = 0;
        for(int i = 0; i < 4; i++)
            for(int c = 0; c < nq; c++)
                if(Sm[i][c] != 0 && shaft[c] != 0) {
                    wsum += t->wh[i].omega;
                    ws += 1;
                }
        t->rpm = fmaxf(fabsf(wsum / ws * G) * RPM_PER_RADS, S->idle_rpm * 0.9f);
    }
    t->load = clampf(throttle, 0, 1);

    /* 5. The chassis against the ground, obstacles and water. */
    float hit = 0, scrape = 0, chassis_n = 0;
    V3 lo = v3(S->body_lo[0], S->body_lo[1], S->body_lo[2]), hi = v3(S->body_hi[0], S->body_hi[1], S->body_hi[2]);
    V3 pts[14];
    int np = 0;
    for(int c = 0; c < 8; c++)
        pts[np++] = v3((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
    pts[np++] = v3(0, lo.y, lo.z);
    pts[np++] = v3(0, lo.y, hi.z);
    pts[np++] = v3(lo.x, lo.y, 0);
    pts[np++] = v3(hi.x, lo.y, 0);
    pts[np++] = v3(0, lo.y - 0.05f, 0);
    pts[np++] = v3(0, (lo.y + hi.y) * 0.5f, hi.z + 0.05f);
    const float KC = 260000.0f, CC = 14000.0f, MUC = 0.45f;
    float water_top = -1000.0f;
    for(int k = 0; k < np; k++) {
        V3 p = truck_to_world(t, pts[k]);
        V3 vp = vadd(t->vel, vcross(t->w, vsub(p, t->pos)));
        ObsContact oc[4];
        int nc = 0;
        V3 gn;
        float g = ground_below(p.x, p.z, p.y + 0.45f, &gn, NULL);
        if(p.y < g) oc[nc++] = (ObsContact){gn, g - p.y};
        nc += obstacle_contacts(p, 0.02f, oc + nc, 4 - nc);
        for(int c = 0; c < nc; c++) {
            V3 n = oc[c].n;
            float depth = fminf(oc[c].depth, 0.5f);
            float vn = vdot(vp, n);
            float fn = KC * depth - CC * fminf(vn, 0.0f);
            if(fn <= 0) continue;
            if(-vn > hit) hit = -vn;
            chassis_n += fn;
            V3 vt = vsub(vp, vscale(n, vn));
            float vtl = vlen(vt);
            V3 f = vscale(n, fn);
            if(vtl > 0.01f) {
                float ff = fminf(MUC * fn, vtl * 6000.0f);
                f = vmad(f, vt, -ff / vtl);
                scrape = fmaxf(scrape, vtl);
            }
            force = vadd(force, f);
            torque = vadd(torque, vcross(vsub(p, t->pos), f));
        }
        /* Water: some buoyancy, a lot of drag. */
        float wl = water_at(p.x, p.z);
        if(wl > p.y) {
            float d = fminf(wl - p.y, 1.0f);
            V3 f = v3(0, S->mass * GRAVITY * 0.30f / np * d, 0);
            f = vmad(f, vp, -260.0f * d);
            force = vadd(force, f);
            torque = vadd(torque, vcross(vsub(p, t->pos), f));
            if(wl > water_top) water_top = wl;
        }
    }
    /* Other trucks: a spring and damper between the two hulls, pushing at
       the height of my centre of gravity so a knock shoves and turns the
       truck rather than levering it over, and some rub between the bodies.
       Each truck feels its own half, so the pair stays symmetric. */
    for(int o = 0; o < game.ntrucks; o++) {
        const Truck *u = &trucks[o];
        if(u == t || u->absent || vlen(vsub(u->pos, t->pos)) > 8.0f) continue;
        PairHit ph;
        if(!truck_pair(t, u, &ph)) continue;
        const float KT = 350000.0f, CT = 30000.0f, MUT = 0.35f;
        V3 p = v3(ph.p.x, t->pos.y, ph.p.z), n = vscale(ph.n, -1.0f);
        V3 vme = vadd(t->vel, vcross(t->w, vsub(p, t->pos)));
        V3 vot = vadd(u->vel, vcross(u->w, vsub(p, u->pos)));
        V3 vr = vsub(vme, vot);
        float vn = vdot(vr, n);
        float fn = KT * fminf(ph.depth, 0.5f) - CT * fminf(vn, 0.0f);
        if(fn <= 0) continue;
        if(-vn > hit) hit = -vn;
        V3 f = vscale(n, fn);
        V3 vt = vsub(vr, vscale(n, vn));
        vt.y = 0;
        float vtl = vlen(vt);
        if(vtl > 0.01f) f = vmad(f, vt, -fminf(MUT * fn, vtl * 8000.0f) / vtl);
        force = vadd(force, f);
        torque = vadd(torque, vcross(vsub(p, t->pos), f));
    }
    /* Posts (the lookout's legs) against the whole hull, wheels and all:
       one is thinner than the gaps between the hull's contact points, and
       found its way in between them to hold the truck. In the hull's frame
       the post is a circle against its rectangle, where it crosses the
       hull's mid-plane (a pitched truck meets a post ahead of it higher or
       lower than its centre). As with another truck, the push and the rub
       are level and at my centre of gravity's height: a corner caught
       going over the crest nose down otherwise levered the truck onto its
       side. */
    for(int k = 0; k < LVL_POSTS; k++) {
        const lvl_post_t *q = &lvl_posts[k];
        float ex = q->x0 - t->pos.x, ez = q->z0 - t->pos.z;
        if(ex * ex + ez * ez > (6.0f + q->r) * (6.0f + q->r)) continue;
        V3 hc = truck_to_world(t, v3(S->hull_c[0], S->hull_c[1], S->hull_c[2]));
        /* e: the post's step for each metre it climbs. */
        V3 foot = v3(q->x0, q->y0, q->z0);
        V3 e = vscale(v3(q->x1 - q->x0, q->y1 - q->y0, q->z1 - q->z0), 1.0f / (q->y1 - q->y0));
        float h = hc.y, eu = vdot(e, U);
        if(eu > 0.3f) h -= vdot(vsub(vmad(foot, e, h - q->y0), hc), U) / eu;
        V3 a = vmad(foot, e, clampf(h, q->y0, q->y1) - q->y0);
        V3 d = vsub(a, hc);
        float lx = vdot(d, R), ly = vdot(d, U), lz = vdot(d, F);
        float hx = S->hull_h[0], hz = S->hull_h[2];
        if(fabsf(ly) > S->hull_h[1]) continue;
        float cx = clampf(lx, -hx, hx), cz = clampf(lz, -hz, hz);
        float nx, nz, depth;
        if(cx != lx || cz != lz) {
            float dx = lx - cx, dz = lz - cz, dl = sqrtf(dx * dx + dz * dz);
            if(dl >= q->r) continue;
            nx = dx / dl;
            nz = dz / dl;
            depth = q->r - dl;
        }
        else if(hx - fabsf(lx) < hz - fabsf(lz)) {
            nx = signf(lx);
            nz = 0;
            depth = hx - fabsf(lx) + q->r;
        }
        else {
            nx = 0;
            nz = signf(lz);
            depth = hz - fabsf(lz) + q->r;
        }
        /* Toward the post, level: the truck goes the other way. */
        V3 n = vadd(vscale(R, nx), vscale(F, nz));
        n.y = 0;
        float nl = vlen(n);
        if(nl < 1e-3f) continue;
        n = vscale(n, -1.0f / nl);
        V3 p = vmad(a, n, q->r);
        p.y = t->pos.y;
        V3 vp = vadd(t->vel, vcross(t->w, vsub(p, t->pos)));
        float vn = vdot(vp, n);
        float fn = KC * fminf(depth, 0.5f) - CC * fminf(vn, 0.0f);
        if(fn <= 0) continue;
        if(-vn > hit) hit = -vn;
        V3 f = vscale(n, fn);
        V3 vt = vsub(vp, vscale(n, vn));
        vt.y = 0;
        float vtl = vlen(vt);
        if(vtl > 0.01f) {
            f = vmad(f, vt, -fminf(MUC * fn, vtl * 6000.0f) / vtl);
            scrape = fmaxf(scrape, vtl);
        }
        force = vadd(force, f);
        torque = vadd(torque, vcross(vsub(p, t->pos), f));
    }
    /* Wheels against tree trunks and the sides of ledges. */
    for(int i = 0; i < 4; i++) {
        Wheel *w = &t->wh[i];
        ObsContact oc[3];
        int nc = obstacle_contacts(w->centre, S->radius * 0.9f, oc, 3);
        for(int c = 0; c < nc; c++) {
            if(fabsf(oc[c].n.y) > 0.5f) continue;
            V3 vp = vadd(t->vel, vcross(t->w, vsub(w->centre, t->pos)));
            float vn = vdot(vp, oc[c].n);
            float fn = KC * fminf(oc[c].depth, 0.3f) - CC * fminf(vn, 0.0f);
            if(fn <= 0) continue;
            if(-vn > hit) hit = -vn;
            V3 f = vscale(oc[c].n, fn);
            force = vadd(force, f);
            torque = vadd(torque, vcross(vsub(w->centre, t->pos), f));
        }
    }
    /* Water over the air intake for more than a splash stalls the engine. */
    V3 intake = truck_to_world(t, v3(0, S->wade, 0.6f));
    if(water_top > intake.y && !t->stalled) {
        t->drown_t += dt;
        if(t->drown_t > 1.5f) {
            t->stalled = true;
            t->stall_t = 0;
        }
    }
    else t->drown_t = fmaxf(0, t->drown_t - dt);
    if(hit > t->hit) t->hit = hit;
    t->dbg_chassis = chassis_n;
    if(scrape > t->scrape) t->scrape = scrape;
    /* A little air drag. */
    float v2 = vdot(t->vel, t->vel);
    if(v2 > 0.01f) force = vmad(force, t->vel, -0.95f * sqrtf(v2));

    /* 6. Integrate. */
    t->vel = vmad(t->vel, force, dt / S->mass);
    V3 wb = v3(vdot(t->w, R), vdot(t->w, U), vdot(t->w, F));
    V3 tb = v3(vdot(torque, R), vdot(torque, U), vdot(torque, F));
    V3 Iwb = v3(S->inertia.x * wb.x, S->inertia.y * wb.y, S->inertia.z * wb.z);
    V3 gyro = vcross(wb, Iwb);
    wb.x += (tb.x - gyro.x) / S->inertia.x * dt;
    wb.y += (tb.y - gyro.y) / S->inertia.y * dt;
    wb.z += (tb.z - gyro.z) / S->inertia.z * dt;
    t->w = vadd(vadd(vscale(R, wb.x), vscale(U, wb.y)), vscale(F, wb.z));
    t->w = vscale(t->w, 1.0f - 0.15f * dt);
    t->pos = vmad(t->pos, t->vel, dt);
    for(int k = 0; k < 3; k++) t->ax[k] = vmad(t->ax[k], vcross(t->w, t->ax[k]), dt);
    t->ax[2] = vnorm(t->ax[2]);
    t->ax[0] = vnorm(vcross(t->ax[1], t->ax[2]));
    t->ax[1] = vcross(t->ax[2], t->ax[0]);
    /* Keep it on the map. */
    float lim = -LEVEL_ORG - 6.0f;
    if(fabsf(t->pos.x) > lim) { t->pos.x = signf(t->pos.x) * lim; t->vel.x = 0; }
    if(fabsf(t->pos.z) > lim) { t->pos.z = signf(t->pos.z) * lim; t->vel.z = 0; }
    for(int i = 0; i < 4; i++) t->wh[i].spin += t->wh[i].omega * dt;
    if(!any_contact) t->airborne_t += dt;
    else t->airborne_t = 0;
    (void)body_pts_dummy;
}

/* ---- The frame: controls, gearbox, steps ---- */

void truck_update(Truck *t, float dt) {
    const TruckSpec *S = &t->spec;
    TruckInput *in = &t->in;
    float fwd_speed = vdot(t->vel, t->ax[2]);
    /* Auto reverse: hold the brake at a standstill to back up; in reverse
       the pedals swap. */
    float throttle = in->throttle, brake = in->brake;
    if(t->gear > 0) {
        if(fabsf(fwd_speed) < 0.7f && brake > 0.5f && throttle < 0.1f) {
            t->rev_hold += dt;
            if(t->rev_hold > 0.35f) {
                t->gear = -1;
                t->rev_hold = 0;
                t->shift_t = 0.25f;
            }
        }
        else t->rev_hold = 0;
    }
    else {
        if(fabsf(fwd_speed) < 0.7f && throttle > 0.3f && brake < 0.1f) {
            t->gear = 1;
            t->shift_t = 0.25f;
        }
        float tmp = throttle;
        throttle = brake;
        brake = tmp;
        /* The engine only drives reverse up to REVERSE_TOP_MPS, fading over
           the last ~2 mph. Gravity can still roll it faster down a slope. */
        throttle *= clampf((REVERSE_TOP_MPS - -fwd_speed) / 0.9f, 0, 1);
    }
    /* Parked: nothing on the pedals at a standstill holds the brakes on. */
    if(fabsf(fwd_speed) < 0.4f && throttle < 0.05f) brake = fmaxf(brake, 1.0f);
    t->throttle = throttle;
    t->brake = brake;
    /* Gearbox. */
    if(t->shift_t > 0) t->shift_t -= dt;
    /* The automatic shifts on road speed, not wheel speed, and holds its
       gear while the tyres are spinning or just after a downshift, so a
       crawl up a loose climb doesn't hunt between gears. */
    if(t->shift_hold > 0) t->shift_hold -= dt;
    if(t->gear > 0 && t->shift_t <= 0) {
        float G = total_ratio(t);
        float rpm_v = fabsf(fwd_speed) / S->radius * fabsf(G) * RPM_PER_RADS;
        float slip = 0;
        for(int i = 0; i < 4; i++) slip = fmaxf(slip, t->wh[i].slip);
        float up = S->redline_rpm * (0.52f + 0.4f * throttle);
        float down = S->redline_rpm * (0.24f + 0.16f * throttle);
        /* Spinning the tyres (a crawl, or a power slide) holds the gear
           until the engine itself nears redline, so a slide keeps its drive. */
        bool spinning = slip >= 1.2f && (fabsf(fwd_speed) < 4.0f || t->rpm < S->redline_rpm * 0.96f);
        if(rpm_v > up && t->gear < truck_top_gear(t) && !spinning && t->shift_hold <= 0) {
            t->gear++;
            t->shift_t = 0.22f;
            t->shift_hold = 0.8f;
        }
        else if(t->gear > 1 && rpm_v < down) {
            t->gear--;
            t->shift_t = 0.18f;
            t->shift_hold = 1.5f;
        }
    }
    /* Steering: slewed, eased off at speed, Ackermann geometry. */
    /* Less lock at speed for stability, but all of it back once the truck
       is sliding (its body more than ~5 degrees off its path), so a slide
       can be caught and held with opposite lock. */
    float beta = atan2f(fabsf(vdot(t->vel, t->ax[0])), fmaxf(fabsf(fwd_speed), 1.0f));
    float sliding = clampf((beta - 0.08f) / 0.17f, 0, 1);
    float steer_t = in->steer * S->max_steer / (1.0f + fabsf(fwd_speed) / 16.0f * (1.0f - sliding));
    static float steer_now[MAX_TRUCKS];
    int ti = (int)(t - trucks);
    if(ti < 0 || ti >= MAX_TRUCKS) ti = 0;
    steer_now[ti] = approach(steer_now[ti], steer_t, dt * 2.4f);
    float st = steer_now[ti];
    float wb = S->wheel[WFL].z - S->wheel[WRL].z, track = S->wheel[WFR].x - S->wheel[WFL].x;
    if(fabsf(st) > 1e-3f) {
        float Rt = wb / tanf(fabsf(st));
        float inner = atanf(wb / (Rt - track * 0.5f)), outer = atanf(wb / (Rt + track * 0.5f));
        float sg = signf(st);
        t->wh[WFL].steer = sg * (st > 0 ? outer : inner);
        t->wh[WFR].steer = sg * (st > 0 ? inner : outer);
    }
    else t->wh[WFL].steer = t->wh[WFR].steer = 0;
    /* Stalled in deep water: restarts by itself once out. */
    if(t->stalled) {
        t->stall_t += dt;
        V3 intake = truck_to_world(t, v3(0, S->wade, 0.6f));
        if(t->stall_t > 2.5f && water_at(intake.x, intake.z) < intake.y - 0.1f) {
            t->stalled = false;
            t->rpm = S->idle_rpm;
        }
    }
    /* Traction control (the Kestrel and the Taco have it, the older trucks
       leave it to your foot) trims the engine when the tyres spin up past
       the ground, in 2WD and 4WD; the driver switches it off to slide the tail. Never
       with the lockers in: crawling over rock wants the wheels to turn
       however much they slip. */
    t->tc = 1.0f;
    static float tc_s[MAX_TRUCKS];
    int ti2 = (int)(t - trucks) & (MAX_TRUCKS - 1);
    if(!t->spec.tcs || t->tcs_off || t->mode == DRIVE_LOCK) tc_s[ti2] = 1.0f;
    else {
        float slip = 0;
        for(int i = 0; i < 4; i++)
            if(t->wh[i].contact) slip = fmaxf(slip, -(vdot(t->vel, t->ax[2]) - t->wh[i].omega * S->radius) * signf(t->wh[i].omega));
        float want = clampf(1.0f - (slip - 1.2f) / 2.5f, 0.3f, 1.0f);
        tc_s[ti2] += (want - tc_s[ti2]) * smooth_k(want < tc_s[ti2] ? 12.0f : 3.0f, dt);
        if(tc_s[ti2] <= 0) tc_s[ti2] = 1.0f;
        t->tc = tc_s[ti2];
    }
    t->hit = t->scrape = 0;
    PROF_BEGIN(pp);
    /* CPU trucks well away from the camera take half the steps. */
    int n = SUBSTEPS;
    if(t->cpu && vlen(vsub(t->pos, r_cam.pos)) > 40.0f) n = SUBSTEPS / 2;
    for(int s = 0; s < n; s++) step(t, dt / n);
    PROF_LAP(PS_PHYS, pp);
    t->speed = vdot(t->vel, t->ax[2]);
    t->pitch = asinf(clampf(t->ax[2].y, -1, 1)) * (180.0f / ENG_PI);
    t->roll = asinf(clampf(t->ax[0].y, -1, 1)) * (180.0f / ENG_PI);
    t->water = 0;
    for(int i = 0; i < 4; i++) t->water = fmaxf(t->water, t->wh[i].water);
    if(t->stalled) t->rpm = approach(t->rpm, 0, dt * 3000.0f);
}

/* ---- Remote trucks ---- */

void truck_set_pose(Truck *t, V3 pos, const V3 ax[3], V3 vel, float steer, const float spin[4], float dt) {
    const TruckSpec *S = &t->spec;
    t->pos = pos;
    t->ax[0] = ax[0];
    t->ax[1] = ax[1];
    t->ax[2] = ax[2];
    t->vel = vel;
    t->w = v3(0, 0, 0);
    V3 U = ax[1];
    float fwd = vdot(vel, ax[2]);
    /* Each wheel where its suspension would put it on this ground: the
       sender's ground is the same mountain, so the tyres meet it. */
    for(int i = 0; i < 4; i++) {
        Wheel *w = &t->wh[i];
        V3 m = S->wheel[i];
        V3 droop = truck_to_world(t, v3(m.x, m.y - S->sag * S->travel, m.z));
        int sf;
        float g = ground_below(droop.x, droop.z, droop.y + S->travel, NULL, &sf) - surf_props[sf].sink;
        float pen = g - (droop.y - S->radius);
        float c = clampf(pen / fmaxf(U.y, 0.4f), 0, S->travel);
        w->comp_prev = w->comp;
        w->comp = c;
        w->centre = vmad(droop, U, c);
        w->contact = pen > -0.05f;
        w->surf = sf;
        w->cp = v3(droop.x, g, droop.z);
        float wl = water_at(droop.x, droop.z);
        w->water = wl > -999.0f ? fmaxf(0, wl - g) : 0;
        w->steer = i < 2 ? steer : 0;
        w->omega = (fwd + spin[i]) / S->radius;
        w->spin += w->omega * dt;
    }
    t->speed = fwd;
    t->pitch = asinf(clampf(ax[2].y, -1, 1)) * (180.0f / ENG_PI);
    t->roll = asinf(clampf(ax[0].y, -1, 1)) * (180.0f / ENG_PI);
    t->water = 0;
    for(int i = 0; i < 4; i++) t->water = fmaxf(t->water, t->wh[i].water);
}
