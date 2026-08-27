/*
 * Ping Cube -- a network-aware KallistiOS demo for the Sega Dreamcast.
 *
 * The Broadband Adapter is acquired through KOS's built-in DHCP client. KOS's
 * IPv4 stack handles ICMP echo requests, so the assigned address is pingable
 * whenever the emulator exposes the BBA at layer 2. The address is drawn into
 * a texture and mapped onto every face of a slowly rotating cube using the
 * low-level PowerVR API.
 */

#include <kos.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXTURE_SIZE 256
#define TEXTURE_BYTES (TEXTURE_SIZE * TEXTURE_SIZE * sizeof(uint16_t))

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

typedef struct point3 {
    float x;
    float y;
    float z;
} point3_t;

typedef struct screen_point {
    float x;
    float y;
    float z;
} screen_point_t;

static const point3_t cube_vertices[8] = {
    {-1.0f, -1.0f, -1.0f},
    { 1.0f, -1.0f, -1.0f},
    {-1.0f,  1.0f, -1.0f},
    { 1.0f,  1.0f, -1.0f},
    {-1.0f, -1.0f,  1.0f},
    { 1.0f, -1.0f,  1.0f},
    {-1.0f,  1.0f,  1.0f},
    { 1.0f,  1.0f,  1.0f}
};

/* Each row is top-left, top-right, bottom-left, bottom-right. */
static const uint8_t cube_faces[6][4] = {
    {2, 3, 0, 1}, /* front */
    {7, 6, 5, 4}, /* back */
    {6, 2, 4, 0}, /* left */
    {3, 7, 1, 5}, /* right */
    {6, 7, 2, 3}, /* top */
    {0, 1, 4, 5}  /* bottom */
};

static const float face_brightness[6] = {
    1.00f, 0.78f, 0.86f, 0.92f, 1.00f, 0.72f
};

static const float face_uv[4][2] = {
    {0.0f, 0.0f},
    {1.0f, 0.0f},
    {0.0f, 1.0f},
    {1.0f, 1.0f}
};

static pvr_ptr_t cube_texture;
static pvr_poly_hdr_t cube_header;

static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xf8) << 8) |
                      ((g & 0xfc) << 3) |
                      ((b & 0xf8) >> 3));
}

static void fill_texture_background(uint16_t *pixels) {
    int x;
    int y;

    for(y = 0; y < TEXTURE_SIZE; ++y) {
        for(x = 0; x < TEXTURE_SIZE; ++x) {
            unsigned r = 5 + (unsigned)y / 18;
            unsigned g = 18 + (unsigned)y / 7;
            unsigned b = 33 + (unsigned)y / 5;

            if(((x / 16) + (y / 16)) & 1) {
                g += 3;
                b += 5;
            }

            pixels[y * TEXTURE_SIZE + x] = rgb565(r, g, b);
        }
    }

    for(y = 0; y < TEXTURE_SIZE; ++y) {
        for(x = 0; x < TEXTURE_SIZE; ++x) {
            if(x < 5 || x >= TEXTURE_SIZE - 5 ||
               y < 5 || y >= TEXTURE_SIZE - 5) {
                pixels[y * TEXTURE_SIZE + x] = rgb565(32, 216, 220);
            }
            else if(x < 8 || x >= TEXTURE_SIZE - 8 ||
                    y < 8 || y >= TEXTURE_SIZE - 8) {
                pixels[y * TEXTURE_SIZE + x] = rgb565(9, 72, 89);
            }
        }
    }
}

static void draw_centered_text(uint16_t *pixels, int y, const char *text,
                               uint16_t color) {
    const size_t length = strlen(text);
    int x = (TEXTURE_SIZE - (int)length * BFONT_THIN_WIDTH) / 2;
    uint16_t *shadow;
    uint16_t *target;

    if(x < 0)
        x = 0;

    shadow = pixels + (y + 2) * TEXTURE_SIZE + x + 2;
    target = pixels + y * TEXTURE_SIZE + x;

    bfont_draw_str_ex(shadow, TEXTURE_SIZE, rgb565(0, 8, 12), 0,
                      16, false, text);
    bfont_draw_str_ex(target, TEXTURE_SIZE, color, 0,
                      16, false, text);
}

static int make_cube_texture(const char *ip_address, const char *network_status) {
    uint16_t *pixels = aligned_alloc(32, TEXTURE_BYTES);
    pvr_poly_cxt_t context;

    if(!pixels)
        return -1;

    fill_texture_background(pixels);
    draw_centered_text(pixels, 24,  "PING CUBE",     rgb565(255, 255, 255));
    draw_centered_text(pixels, 64,  "DREAMCAST IP",  rgb565(127, 238, 255));
    draw_centered_text(pixels, 108, ip_address,       rgb565(255, 226, 76));
    draw_centered_text(pixels, 150, network_status,   rgb565(188, 255, 210));
    /* The BIOS font consumes ISO-8859-1, where 0xa9 is the copyright sign. */
    draw_centered_text(pixels, 192, "\xa9 Rich Stokes 2026",
                       rgb565(155, 192, 205));
    draw_centered_text(pixels, 222, "START: EXIT",    rgb565(255, 255, 255));

    cube_texture = pvr_mem_malloc(TEXTURE_BYTES);
    if(!cube_texture) {
        free(pixels);
        return -1;
    }

    pvr_txr_load(pixels, cube_texture, TEXTURE_BYTES);
    free(pixels);

    pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     TEXTURE_SIZE, TEXTURE_SIZE, cube_texture,
                     PVR_FILTER_BILINEAR);
    /* All strips wind clockwise when viewed from outside the cube. */
    context.gen.culling = PVR_CULLING_CCW;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = true;
    context.txr.env = PVR_TXRENV_MODULATE;
    context.txr.uv_clamp = PVR_UVCLAMP_UV;
    pvr_poly_compile(&cube_header, &context);

    return 0;
}

static void transform_cube(float angle_x, float angle_y,
                           screen_point_t transformed[8]) {
    const float sin_x = fsin(angle_x);
    const float cos_x = fcos(angle_x);
    const float sin_y = fsin(angle_y);
    const float cos_y = fcos(angle_y);
    const float focal_length = 430.0f;
    int i;

    for(i = 0; i < 8; ++i) {
        const point3_t *source = &cube_vertices[i];
        const float x_yaw = source->x * cos_y + source->z * sin_y;
        const float z_yaw = -source->x * sin_y + source->z * cos_y;
        const float y_pitch = source->y * cos_x - z_yaw * sin_x;
        const float z_pitch = source->y * sin_x + z_yaw * cos_x;
        const float camera_z = z_pitch + 4.5f;
        const float inverse_z = 1.0f / camera_z;

        transformed[i].x = 320.0f + x_yaw * focal_length * inverse_z;
        transformed[i].y = 240.0f - y_pitch * focal_length * inverse_z;
        transformed[i].z = inverse_z;
    }
}

static void draw_cube(float angle_x, float angle_y) {
    screen_point_t transformed[8];
    pvr_vertex_t vertex;
    int face;
    int corner;

    transform_cube(angle_x, angle_y, transformed);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    for(face = 0; face < 6; ++face) {
        const uint32_t color = PVR_PACK_COLOR(1.0f,
                                              face_brightness[face],
                                              face_brightness[face],
                                              face_brightness[face]);

        pvr_prim(&cube_header, sizeof(cube_header));

        for(corner = 0; corner < 4; ++corner) {
            const screen_point_t *point =
                &transformed[cube_faces[face][corner]];

            vertex.flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vertex.x = point->x;
            vertex.y = point->y;
            vertex.z = point->z;
            vertex.u = face_uv[corner][0];
            vertex.v = face_uv[corner][1];
            vertex.argb = color;
            vertex.oargb = 0;
            pvr_prim(&vertex, sizeof(vertex));
        }
    }

    pvr_list_finish();
    pvr_scene_finish();
}

static int start_pressed(void) {
    MAPLE_FOREACH_BEGIN(MAPLE_FUNC_CONTROLLER, cont_state_t, state)
        if(state->buttons & CONT_START)
            return 1;
    MAPLE_FOREACH_END()

    return 0;
}

static void describe_network(char ip_address[32], char network_status[32]) {
    if(net_default_dev &&
       (net_default_dev->ip_addr[0] || net_default_dev->ip_addr[1] ||
        net_default_dev->ip_addr[2] || net_default_dev->ip_addr[3])) {
        const uint8_t *ip = net_default_dev->ip_addr;

        snprintf(ip_address, 32, "%u.%u.%u.%u",
                 ip[0], ip[1], ip[2], ip[3]);
        snprintf(network_status, 32, "ICMP ECHO READY");
    }
    else {
        snprintf(ip_address, 32, "NO NETWORK");
        snprintf(network_status, 32, "ENABLE FLYCAST BBA");
    }
}

int main(int argc, char **argv) {
    char ip_address[32];
    char network_status[32];
    const uint64_t start_time = timer_ms_gettime64();

    (void)argc;
    (void)argv;

    describe_network(ip_address, network_status);
    printf("Ping Cube network: %s (%s)\n", ip_address, network_status);

    if(net_default_dev) {
        const uint8_t *mask = net_default_dev->netmask;
        const uint8_t *gateway = net_default_dev->gateway;

        printf("Ping Cube netif: %s, netmask %u.%u.%u.%u, "
               "gateway %u.%u.%u.%u\n",
               net_default_dev->name,
               mask[0], mask[1], mask[2], mask[3],
               gateway[0], gateway[1], gateway[2], gateway[3]);
        printf("Ping Cube: KOS ICMP echo replies are active\n");
    }

    pvr_init_defaults();
    pvr_set_bg_color(0.015f, 0.035f, 0.065f);

    if(make_cube_texture(ip_address, network_status) < 0) {
        printf("Ping Cube: unable to allocate the cube texture\n");
        return 1;
    }

    while(!start_pressed()) {
        const float elapsed =
            (float)(timer_ms_gettime64() - start_time) * 0.001f;
        const float angle_y = 0.55f + elapsed * 0.28f;
        const float angle_x = -0.32f + fsin(elapsed * 0.20f) * 0.24f;

        draw_cube(angle_x, angle_y);
    }

    pvr_mem_free(cube_texture);
    return 0;
}
