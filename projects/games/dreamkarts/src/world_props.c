/* Props more than one track places: the procedural ones (palms, huts,
   rocks, boats, the gantry and grandstand) and prop_place, which instances
   the props baked in Blender. */
#include "tsudoken_props.h"   /* for dk_prop_t (its meshes are Tsudoken's) */
#include "world_int.h"

void add_palm(V3 base, float height, float lean_yaw, float lean, uint32_t seed) {
    if(palm_n >= MAX_PALMS) return;
    palms[palm_n++] = (Palm){base, height};
    chunk_begin(HF_OP, 135.0f);
    V3 lean_dir = heading_fwd(lean_yaw);
    const int segs = 5;
    V3 prev = base;
    V3 ring_c[6];
    for(int i = 0; i <= segs; i++) {
        float t = (float)i / segs;
        V3 c = vadd(base, v3(0, height * t, 0));
        c = vmad(c, lean_dir, lean * height * t * t);
        ring_c[i] = c;
    }
    (void)prev;
    for(int side = 0; side < 5; side++) {
        float a0 = side / 5.0f * DK_TAU, a1 = (side + 1) / 5.0f * DK_TAU;
        V3 n0 = v3(sinf(a0), 0, cosf(a0)), n1 = v3(sinf(a1), 0, cosf(a1));
        strip_begin(h_bark);
        for(int i = 0; i <= segs; i++) {
            float r = lerpf(0.34f, 0.2f, (float)i / segs);
            float v = (float)i * 0.9f;
            svp(vmad(ring_c[i], n0, r), 0, v, shade(argb(255, 255, 255, 255), n0));
            svp(vmad(ring_c[i], n1, r), 1, v, shade(argb(255, 255, 255, 255), n1));
        }
        strip_end();
    }
    chunk_end();

    /* Fronds: alpha-tested bent leaves in the punch-through list. */
    chunk_begin(HF_PT, 135.0f);
    V3 top = ring_c[segs];
    int fronds = 8;
    for(int f = 0; f < fronds; f++) {
        float a = (float)f / fronds * DK_TAU + (float)(seed % 7) * 0.3f;
        V3 dir = v3(sinf(a), 0, cosf(a)), side = v3(cosf(a), 0, -sinf(a));
        float len = height * (0.50f + 0.08f * (float)((seed + f * 3) % 4));
        float rise = 0.55f, w = len * 0.30f;
        strip_begin(h_frond);
        for(int k = 0; k <= 3; k++) {
            float t = (float)k / 3.0f;
            V3 c = vadd(top, vadd(vscale(dir, len * t), v3(0, len * (rise * t - 1.15f * t * t), 0)));
            float ww = w * (k == 0 ? 0.25f : 1.0f);
            uint32_t col = color_scale(argb(255, 255, 255, 255), 0.85f + 0.25f * t);
            svp(vmad(c, side, -ww), 0, 1.0f - t, col);
            svp(vmad(c, side, ww), 1, 1.0f - t, col);
        }
        strip_end();
    }
    chunk_end();
}

void add_umbrella(V3 base, uint32_t ca, uint32_t cb) {
    chunk_begin(HF_OP, 260.0f);
    box(h_col, vadd(base, v3(0, 1.1f, 0)), v3(0.05f, 1.1f, 0.05f), 0, argb(255, 240, 240, 240), 1);
    cone(h_col, vadd(base, v3(0, 2.6f, 0)), 1.9f, 0.7f, 8, ca, cb);
    /* Towel and cool box. */
    float yaw = base.x * 0.37f;
    box(h_col, vadd(base, v3(1.2f, 0.03f, 0.4f)), v3(0.55f, 0.03f, 1.0f), yaw, cb == 0xffffffff ? ca : cb, 1);
    box(h_col, vadd(base, v3(-0.9f, 0.22f, -0.8f)), v3(0.3f, 0.22f, 0.22f), yaw, argb(255, 60, 140, 230), 1);
    chunk_end();
}

void add_hut(V3 c, float yaw) {
    chunk_begin(HF_OP, 600.0f);
    box(h_wood, vadd(c, v3(0, 1.4f, 0)), v3(2.6f, 1.4f, 2.2f), yaw, argb(255, 255, 240, 220), 0.5f);
    /* Thatch roof: four sloped faces meeting at a ridge point. */
    V3 ax = vscale(heading_right(yaw), 3.2f), az = vscale(heading_fwd(yaw), 2.8f);
    V3 roof_base = vadd(c, v3(0, 2.7f, 0)), apex = vadd(c, v3(0, 4.6f, 0));
    V3 corners[4] = {vsub(vsub(roof_base, ax), az), vsub(vadd(roof_base, ax), az),
                     vadd(vadd(roof_base, ax), az), vadd(vsub(roof_base, ax), az)};
    for(int i = 0; i < 4; i++) {
        V3 a = corners[i], b = corners[(i + 1) % 4];
        V3 n = vcross(vsub(b, a), vsub(apex, a));
        if(n.y < 0) n = vscale(n, -1);
        uint32_t col = shade(argb(255, 255, 255, 255), n);
        strip_begin(h_thatch);
        svp(apex, 0.5f, 0, col);
        svp(a, 0, 2, col);
        svp(b, 2, 2, col);
        strip_end();
    }
    chunk_end();
}

void add_rock(V3 c, float r, float hgt, uint32_t seed, bool grass_top) {
    chunk_begin(HF_OP, 2000.0f);
    const int sides = 7;
    float rad[3][7];
    for(int k = 0; k < 3; k++)
        for(int i = 0; i < sides; i++) {
            seed = seed * 1103515245u + 12345u;
            rad[k][i] = r * (0.75f + 0.5f * (float)((seed >> 16) & 255) / 255.0f) * (k == 0 ? 1.0f : k == 1 ? 0.92f : 0.7f);
        }
    float ys[3] = {-3.0f, hgt * 0.55f, hgt};
    for(int k = 0; k < 2; k++) {
        strip_begin(h_rock);
        for(int i = 0; i <= sides; i++) {
            int ii = i % sides;
            float a = (float)i / sides * DK_TAU;
            V3 n = v3(sinf(a), 0.25f, cosf(a));
            V3 top = vadd(c, v3(sinf(a) * rad[k + 1][ii], ys[k + 1], cosf(a) * rad[k + 1][ii]));
            V3 bot = vadd(c, v3(sinf(a) * rad[k][ii], ys[k], cosf(a) * rad[k][ii]));
            float u = (float)i / sides * 3.0f;
            svp(top, u, ys[k + 1] / 8.0f, shade(0xffffffff, n));
            svp(bot, u, ys[k] / 8.0f, shade(0xffffffff, n));
        }
        strip_end();
    }
    /* Cap. */
    strip_begin(grass_top ? h_grass : h_rock);
    V3 capc = vadd(c, v3(0, hgt + r * 0.15f, 0));
    for(int i = 0; i <= sides; i++) {
        int ii = i % sides;
        float a = (float)i / sides * DK_TAU;
        V3 p = vadd(c, v3(sinf(a) * rad[2][ii], hgt, cosf(a) * rad[2][ii]));
        svp(p, p.x / 5.0f, p.z / 5.0f, shade(0xffffffff, v3(sinf(a), 1.5f, cosf(a))));
        svp(capc, capc.x / 5.0f, capc.z / 5.0f, 0xffffffff);
    }
    strip_end();
    chunk_end();
}

/* Emerald Coast's signature: a natural rock arch standing in the sea. */
void add_arch(V3 c, float yaw) {
    chunk_begin(HF_OP, 2000.0f);
    const int segs = 9, sides = 6;
    V3 ax = heading_right(yaw);
    float span = 26.0f, rise = 24.0f;
    for(int s = 0; s < segs; s++) {
        float t0 = (float)s / segs, t1 = (float)(s + 1) / segs;
        float a0 = DK_PI * t0, a1 = DK_PI * t1;
        V3 c0 = vadd(c, vadd(vscale(ax, -cosf(a0) * span), v3(0, sinf(a0) * rise - 4.0f, 0)));
        V3 c1 = vadd(c, vadd(vscale(ax, -cosf(a1) * span), v3(0, sinf(a1) * rise - 4.0f, 0)));
        float th0 = 6.5f - 3.0f * sinf(a0), th1 = 6.5f - 3.0f * sinf(a1);
        V3 tan0 = vnorm(vsub(c1, c0));
        V3 up0 = vnorm(vcross(heading_fwd(yaw), tan0));
        V3 fw = heading_fwd(yaw);
        strip_begin(h_rock);
        for(int i = 0; i <= sides; i++) {
            float a = (float)i / sides * DK_TAU;
            V3 off0 = vadd(vscale(up0, cosf(a) * th0), vscale(fw, sinf(a) * th0 * 0.8f));
            V3 off1 = vadd(vscale(up0, cosf(a) * th1), vscale(fw, sinf(a) * th1 * 0.8f));
            V3 n = vnorm(off0);
            svp(vadd(c1, off1), (float)i / sides * 2.0f, t1 * 6.0f, shade(0xffffffff, n));
            svp(vadd(c0, off0), (float)i / sides * 2.0f, t0 * 6.0f, shade(0xffffffff, n));
        }
        strip_end();
    }
    chunk_end();
    /* Greenery on top. */
    add_rock(vadd(c, v3(0, rise - 6.0f, 0)), 5.0f, 5.5f, 77, true);
}

void add_lighthouse(V3 c) {
    add_rock(c, 22.0f, 7.0f, 991, true);
    chunk_begin(HF_OP, 2000.0f);
    V3 b = vadd(c, v3(0, 7.0f, 0));
    for(int band = 0; band < 5; band++) {
        float y0 = band * 4.2f, y1 = y0 + 4.2f;
        float r0 = lerpf(3.0f, 1.9f, y0 / 21.0f), r1 = lerpf(3.0f, 1.9f, y1 / 21.0f);
        uint32_t col = (band & 1) ? argb(255, 220, 40, 40) : argb(255, 250, 250, 245);
        cylinder(h_col, vadd(b, v3(0, y0, 0)), r0, r1, 4.2f, 8, col, col, 1, 1);
    }
    cylinder(h_col, vadd(b, v3(0, 21.0f, 0)), 2.3f, 2.3f, 2.6f, 8, argb(255, 255, 240, 160), argb(255, 255, 250, 200), 1, 1);
    cone(h_col, vadd(b, v3(0, 25.6f, 0)), 2.8f, 2.0f, 8, argb(255, 200, 40, 40), argb(255, 170, 30, 30));
    chunk_end();
}

void add_boat(V3 c, float yaw, uint32_t hull) {
    chunk_begin(HF_OP, 2000.0f);
    box(h_col, vadd(c, v3(0, 0.6f, 0)), v3(1.6f, 0.7f, 5.0f), yaw, hull, 1);
    box(h_col, vadd(c, v3(0, 5.0f, 0)), v3(0.08f, 4.0f, 0.08f), yaw, argb(255, 230, 230, 230), 1);
    V3 fw = heading_fwd(yaw);
    V3 mast_top = vadd(c, v3(0, 9.0f, 0)), mast_bot = vadd(c, v3(0, 1.5f, 0));
    V3 clew = vadd(vmad(c, fw, -4.2f), v3(0, 1.6f, 0));
    strip_begin(h_col);
    svp(mast_top, 0, 0, argb(255, 255, 255, 255));
    svp(mast_bot, 0, 0, argb(255, 235, 235, 240));
    svp(clew, 0, 0, argb(255, 225, 228, 236));
    strip_end();
    V3 bow = vadd(vmad(c, fw, 5.5f), v3(0, 1.4f, 0));
    strip_begin(h_col);
    svp(mast_top, 0, 0, argb(255, 255, 200, 60));
    svp(mast_bot, 0, 0, argb(255, 255, 170, 40));
    svp(bow, 0, 0, argb(255, 255, 150, 30));
    strip_end();
    chunk_end();
}

void add_gantry(void) {
    const TrackSample *t = &track.smp[0];
    chunk_begin(HF_OP, 900.0f);
    for(int side = -1; side <= 1; side += 2) {
        V3 p = vmad(t->p, t->r, side * 10.8f);
        box(h_col, vadd(p, v3(0, 3.4f, 0)), v3(0.45f, 3.4f, 0.45f), 0, argb(255, 240, 240, 245), 1);
        box(h_col, vadd(p, v3(0, 0.3f, 0)), v3(0.7f, 0.3f, 0.7f), 0, argb(255, 40, 90, 200), 1);
    }
    /* Banner panel, both faces. */
    V3 l = vmad(t->p, t->r, -10.4f), r = vmad(t->p, t->r, 10.4f);
    float y0 = 5.0f, y1 = 6.8f;
    V3 off = vscale(t->t, 0.3f);
    for(int f = -1; f <= 1; f += 2) {
        V3 o = vscale(off, (float)f);
        /* Text must read left-to-right from whichever side it faces. */
        V3 a = vadd(vadd(f < 0 ? l : r, o), v3(0, y1, 0)), b = vadd(vadd(f < 0 ? r : l, o), v3(0, y1, 0));
        V3 c = vadd(vadd(f < 0 ? l : r, o), v3(0, y0, 0)), d = vadd(vadd(f < 0 ? r : l, o), v3(0, y0, 0));
        strip_begin(h_banner);
        svp(a, 0, 0, 0xffffffff); svp(b, 1, 0, 0xffffffff);
        svp(c, 0, 0.5f, 0xffffffff); svp(d, 1, 0.5f, 0xffffffff);
        strip_end();
    }
    V3 top0 = vadd(l, v3(0, y1, 0)), top1 = vadd(r, v3(0, y1, 0));
    quad(h_col, vadd(top0, vscale(off, -1)), vadd(top1, vscale(off, -1)), vadd(top0, off), vadd(top1, off),
         0, 0, 1, 1, argb(255, 30, 70, 190));
    chunk_end();

    /* Chequered start line, slightly above the asphalt. */
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_checker);
    for(int k = 0; k <= 1; k++) {
        float s = (k ? 1.2f : -1.2f);
        V3 a = track_point(s, -8.0f), b = track_point(s, 8.0f);
        a.y = b.y = 0.025f;
        svp(a, 0, s / 1.2f, 0xffffffff);
        svp(b, 8, s / 1.2f, 0xffffffff);
    }
    strip_end();
    chunk_end();
}

/* Tiered seating along z = zf, spectators facing north. */
void add_grandstand(float x0, float x1, float zf) {
    chunk_begin(HF_OP, 900.0f);
    float dz = zf + 82.0f;
    for(int tier = 0; tier < 5; tier++) {
        float z = -82.0f + dz - tier * 2.6f, y = 0.9f + tier * 1.2f;
        /* Seats with spectators facing the track (north). */
        V3 a = v3(x0, y, z), b = v3(x1, y, z), c = v3(x0, y - 1.2f, z + 0.1f), d = v3(x1, y - 1.2f, z + 0.1f);
        quad(h_crowd, a, b, c, d, 0, 0, (x1 - x0) / 3.0f, 0.6f, argb(255, 255, 255, 255));
        V3 e = v3(x0, y, z - 2.6f), f = v3(x1, y, z - 2.6f);
        quad(h_col, e, f, a, b, 0, 0, 1, 1, argb(255, 190, 196, 210));
    }
    box(h_col, v3((x0 + x1) * 0.5f, 3.6f, -97.5f + dz), v3((x1 - x0) * 0.5f, 3.6f, 0.4f), 0, argb(255, 150, 160, 185), 1);
    /* Striped canopy on posts. */
    for(int i = 0; i <= 6; i++) {
        float x = lerpf(x0, x1, (float)i / 6.0f);
        box(h_col, v3(x, 4.5f, -82.5f + dz), v3(0.15f, 4.5f, 0.15f), 0, argb(255, 235, 235, 240), 1);
    }
    for(int i = 0; i < 12; i++) {
        float xa = lerpf(x0, x1, (float)i / 12.0f), xb = lerpf(x0, x1, (float)(i + 1) / 12.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 40, 140, 230);
        quad(h_col, v3(xa, 10.5f, -98.0f + dz), v3(xb, 10.5f, -98.0f + dz), v3(xa, 9.0f, -81.5f + dz),
             v3(xb, 9.0f, -81.5f + dz), 0, 0, 1, 1, col);
    }
    chunk_end();
}

void add_sign(V3 c, float yaw) {
    chunk_begin(HF_OP, 700.0f);
    V3 ax = heading_right(yaw), fw = heading_fwd(yaw);
    for(int side = -1; side <= 1; side += 2)
        box(h_col, vadd(vmad(c, ax, side * 7.0f), v3(0, 3.0f, 0)), v3(0.3f, 3.0f, 0.3f), yaw, argb(255, 230, 230, 235), 1);
    for(int f = -1; f <= 1; f += 2) {
        V3 o = vscale(fw, 0.25f * f);
        V3 l = vadd(vmad(c, ax, (float)f * 8.0f), o), r = vadd(vmad(c, ax, (float)f * -8.0f), o);
        strip_begin(h_banner);
        svp(vadd(l, v3(0, 9.0f, 0)), 0, 0, 0xffffffff);
        svp(vadd(r, v3(0, 9.0f, 0)), 1, 0, 0xffffffff);
        svp(vadd(l, v3(0, 5.0f, 0)), 0, 0.5f, 0xffffffff);
        svp(vadd(r, v3(0, 5.0f, 0)), 1, 0.5f, 0xffffffff);
        strip_end();
    }
    chunk_end();
}

void add_pier(float x, float z0) {
    chunk_begin(HF_OP, 900.0f);
    float z1 = z0 + 58.0f, w = 2.2f, y = 0.45f;
    strip_begin(h_board);
    svp(v3(x - w, y, z0), 0, 0, 0xffffffff);
    svp(v3(x + w, y, z0), 1, 0, 0xffffffff);
    svp(v3(x - w, y, z1), 0, (z1 - z0) / 8.0f, 0xffffffff);
    svp(v3(x + w, y, z1), 1, (z1 - z0) / 8.0f, 0xffffffff);
    strip_end();
    for(float z = z0 + 4.0f; z < z1; z += 8.0f)
        for(int side = -1; side <= 1; side += 2)
            box(h_wood, v3(x + side * (w - 0.2f), -0.6f, z), v3(0.18f, 1.1f, 0.18f), 0, 0xffffffff, 1);
    add_hut(v3(x, y, z1 - 3.0f), 0.0f);
    chunk_end();
}

void add_pond(V3 c, float rx, float rz) {
    chunk_begin(HF_OP, 600.0f);
    const int n = 16;
    /* Sand rim then water, as rings from the centre. */
    for(int ring = 0; ring < 2; ring++) {
        strip_begin(ring ? h_water : h_sand);
        float ro = ring ? 0.82f : 1.0f, ri = ring ? 0.0f : 0.82f, y = ring ? 0.08f : 0.04f;
        for(int i = 0; i <= n; i++) {
            float a = (float)i / n * DK_TAU;
            V3 o = v3(c.x + cosf(a) * rx * ro, c.y + y, c.z + sinf(a) * rz * ro);
            V3 in = v3(c.x + cosf(a) * rx * ri, c.y + y, c.z + sinf(a) * rz * ri);
            uint32_t co = ring ? argb(255, 200, 250, 250) : argb(255, 255, 255, 255);
            uint32_t ci = ring ? argb(255, 120, 200, 235) : argb(255, 230, 225, 210);
            svp(o, o.x / 6.0f, o.z / 6.0f, co);
            svp(in, in.x / 6.0f, in.z / 6.0f, ci);
        }
        strip_end();
    }
    chunk_end();
}

/* One baked prop at p, turned by yaw (its +z to heading yaw) and scaled,
   each vertex lit by its normal. mats maps the prop's materials to
   headers, and pt_mask says which of them go in the punch-through list.
   The strips of one material run on as one strip (small pieces cost a
   submit each on the console), in a chunk per list the prop needs. */
void prop_place(const dk_prop_t *m, const pvr_poly_hdr_t *const *mats, uint32_t pt_mask, V3 p, float yaw,
                float scale, float lod_far, bool far) {
    float cs = cosf(yaw), sn = sinf(yaw);
    float unit = (m->cm ? 0.01f : 0.001f) * scale;
    for(int list = 0; list < 2; list++) {
        bool any = false;
        for(int s = 0; s < m->nstrips; s++)
            if((int)((pt_mask >> m->strip_mat[s]) & 1u) == list) any = true;
        if(!any) continue;
        chunk_begin(list ? HF_PT : HF_OP, lod_far);
        if(cur_chunk && far) cur_chunk->far = 1;
        int at = 0, cur_mat = -1;
        for(int s = 0; s < m->nstrips; s++) {
            int n = m->strip_len[s], mat = m->strip_mat[s];
            if((int)((pt_mask >> mat) & 1u) != list) {
                at += n;
                continue;
            }
            if(mat != cur_mat) {
                if(cur_mat >= 0) strip_end();
                strip_begin(mats[mat]);
                cur_mat = mat;
            }
            for(int q = 0; q < n; q++) {
                const dk_prop_vert_t *v = &m->verts[m->index[at + q]];
                float x = v->x * unit, y = v->y * unit, z = v->z * unit;
                V3 w = v3(p.x + x * cs + z * sn, p.y + y, p.z - x * sn + z * cs);
                V3 nn = v3(v->nx * cs + v->nz * sn, v->ny, -v->nx * sn + v->nz * cs);
                uint32_t col = shade(argb(255, v->r, v->g, v->b), nn);
                if(q == 0) svj(w, v->u / 256.0f, v->v / 256.0f, col);
                else svp(w, v->u / 256.0f, v->v / 256.0f, col);
            }
            at += n;
        }
        if(cur_mat >= 0) strip_end();
        chunk_end();
    }
}
