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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXTURE_SIZE 256
#define TEXTURE_BYTES (TEXTURE_SIZE * TEXTURE_SIZE * sizeof(uint16_t))
#define CONSOLE_WIDTH 512
#define CONSOLE_HEIGHT 256
#define CONSOLE_BYTES (CONSOLE_WIDTH * CONSOLE_HEIGHT * sizeof(uint16_t))
#define CONSOLE_LOG_LINES 6
#define PING_INTERVAL_MS 250
#define PING_TIMEOUT_MS 2500
#define PING_PENDING_SLOTS 16
#define PING_IDENTIFIER 0x5043

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

typedef struct color_tint {
    float r;
    float g;
    float b;
} color_tint_t;

typedef struct pending_ping {
    uint16_t sequence;
    uint64_t sent_ms;
    int active;
} pending_ping_t;

typedef enum ping_result {
    PING_RESULT_WAITING,
    PING_RESULT_REPLY,
    PING_RESULT_TIMEOUT,
    PING_RESULT_ERROR
} ping_result_t;

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
static pvr_ptr_t console_texture;
static pvr_poly_hdr_t console_header;
static uint16_t *console_pixels;

static const uint8_t ping_target[4] = {8, 8, 8, 8};
static const uint8_t ping_payload[] = "PING CUBE DREAMCAST";
static pending_ping_t pending_pings[PING_PENDING_SLOTS];
static char ping_log[CONSOLE_LOG_LINES][48];
static char console_source_ip[32];
static uint16_t next_ping_sequence;
static uint32_t transmitted_count;
static uint32_t received_count;
static uint32_t timeout_count;
static uint64_t total_reply_us;
static uint32_t latest_reply_us;
static ping_result_t latest_result = PING_RESULT_WAITING;
static int console_dirty = 1;

/* Written by KOS's network receive path and consumed by the render thread. */
static volatile uint32_t reply_event;
static volatile uint16_t reply_sequence;
static volatile uint32_t reply_delta_us;
static volatile uint8_t reply_ttl;

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

static void draw_console_text(int y, const char *text, uint16_t color) {
    bfont_draw_str_ex(console_pixels + y * CONSOLE_WIDTH + 14,
                      CONSOLE_WIDTH, color, 0, 16, false, text);
}

static void add_ping_log(const char *format, ...) {
    va_list arguments;
    int i;

    for(i = 0; i < CONSOLE_LOG_LINES - 1; ++i)
        memcpy(ping_log[i], ping_log[i + 1], sizeof(ping_log[i]));

    va_start(arguments, format);
    vsnprintf(ping_log[CONSOLE_LOG_LINES - 1],
              sizeof(ping_log[CONSOLE_LOG_LINES - 1]), format, arguments);
    va_end(arguments);
    console_dirty = 1;
}

static void update_console_texture(void) {
    char line[48];
    uint32_t completed = received_count + timeout_count;
    uint32_t loss_percent = completed ? (timeout_count * 100) / completed : 0;
    uint32_t average_tenths = received_count
        ? (uint32_t)(total_reply_us / received_count / 100)
        : 0;
    int x;
    int y;
    int i;

    for(y = 0; y < CONSOLE_HEIGHT; ++y) {
        for(x = 0; x < CONSOLE_WIDTH; ++x) {
            unsigned glow = ((x / 32) + (y / 24)) & 1;
            unsigned scanline = (y & 3) == 0;

            console_pixels[y * CONSOLE_WIDTH + x] =
                rgb565(2 + glow, 7 + glow * 2 + scanline,
                       9 + glow * 2 + scanline);
        }
    }

    for(x = 0; x < CONSOLE_WIDTH; ++x) {
        console_pixels[0 * CONSOLE_WIDTH + x] = rgb565(8, 41, 35);
        console_pixels[1 * CONSOLE_WIDTH + x] = rgb565(8, 41, 35);
        console_pixels[(CONSOLE_HEIGHT - 2) * CONSOLE_WIDTH + x] =
            rgb565(8, 41, 35);
        console_pixels[(CONSOLE_HEIGHT - 1) * CONSOLE_WIDTH + x] =
            rgb565(8, 41, 35);
    }

    snprintf(line, sizeof(line), "PING 8.8.8.8 FROM %.15s", console_source_ip);
    draw_console_text(10, line, rgb565(40, 128, 102));

    snprintf(line, sizeof(line), "TX %lu  RX %lu  LOSS %lu%%  AVG %lu.%lu MS",
             (unsigned long)transmitted_count,
             (unsigned long)received_count,
             (unsigned long)loss_percent,
             (unsigned long)(average_tenths / 10),
             (unsigned long)(average_tenths % 10));
    draw_console_text(38, line, rgb565(31, 92, 77));

    if(latest_result == PING_RESULT_REPLY) {
        uint32_t latest_tenths = latest_reply_us / 100;

        snprintf(line, sizeof(line), "LATEST %lu.%lu MS  STATUS REPLY",
                 (unsigned long)(latest_tenths / 10),
                 (unsigned long)(latest_tenths % 10));
    }
    else if(latest_result == PING_RESULT_TIMEOUT) {
        snprintf(line, sizeof(line), "LATEST TIMEOUT  STATUS UNREACHABLE");
    }
    else if(latest_result == PING_RESULT_ERROR) {
        snprintf(line, sizeof(line), "LATEST SEND ERROR  STATUS DEGRADED");
    }
    else {
        snprintf(line, sizeof(line), "LATEST --.- MS  STATUS WAITING");
    }
    draw_console_text(66, line, rgb565(40, 112, 91));

    for(i = 0; i < CONSOLE_LOG_LINES; ++i)
        draw_console_text(94 + i * 26, ping_log[i], rgb565(28, 79, 69));

    pvr_txr_load(console_pixels, console_texture, CONSOLE_BYTES);
    console_dirty = 0;
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

static int make_console_texture(void) {
    pvr_poly_cxt_t context;

    console_pixels = aligned_alloc(32, CONSOLE_BYTES);
    if(!console_pixels)
        return -1;

    console_texture = pvr_mem_malloc(CONSOLE_BYTES);
    if(!console_texture) {
        free(console_pixels);
        console_pixels = NULL;
        return -1;
    }

    pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     CONSOLE_WIDTH, CONSOLE_HEIGHT, console_texture,
                     PVR_FILTER_BILINEAR);
    context.gen.culling = PVR_CULLING_NONE;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = true;
    context.txr.env = PVR_TXRENV_MODULATE;
    context.txr.uv_clamp = PVR_UVCLAMP_UV;
    pvr_poly_compile(&console_header, &context);

    update_console_texture();
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

static void draw_console_panel(void) {
    static const float positions[4][2] = {
        {24.0f, 104.0f},
        {616.0f, 104.0f},
        {24.0f, 400.0f},
        {616.0f, 400.0f}
    };
    pvr_vertex_t vertex;
    int corner;

    pvr_prim(&console_header, sizeof(console_header));
    for(corner = 0; corner < 4; ++corner) {
        vertex.flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vertex.x = positions[corner][0];
        vertex.y = positions[corner][1];
        vertex.z = 0.02f;
        vertex.u = face_uv[corner][0];
        vertex.v = face_uv[corner][1];
        vertex.argb = PVR_PACK_COLOR(1.0f, 0.58f, 0.58f, 0.58f);
        vertex.oargb = 0;
        pvr_prim(&vertex, sizeof(vertex));
    }
}

static void draw_scene(float angle_x, float angle_y, color_tint_t tint) {
    screen_point_t transformed[8];
    pvr_vertex_t vertex;
    int face;
    int corner;

    transform_cube(angle_x, angle_y, transformed);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    draw_console_panel();

    for(face = 0; face < 6; ++face) {
        const uint32_t color = PVR_PACK_COLOR(1.0f,
                                              face_brightness[face] * tint.r,
                                              face_brightness[face] * tint.g,
                                              face_brightness[face] * tint.b);

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

static color_tint_t ping_color(void) {
    color_tint_t tint;

    if(latest_result == PING_RESULT_REPLY) {
        float latency_ms = (float)latest_reply_us * 0.001f;

        if(latency_ms < 50.0f) {
            float amount = latency_ms / 50.0f;

            tint.r = 0.14f + amount * 0.20f;
            tint.g = 1.00f;
            tint.b = 0.16f - amount * 0.02f;
        }
        else {
            float amount = (latency_ms - 50.0f) / 250.0f;

            if(amount > 1.0f)
                amount = 1.0f;
            tint.r = 0.34f + amount * 0.66f;
            tint.g = 1.00f - amount * 0.86f;
            tint.b = 0.14f - amount * 0.04f;
        }
    }
    else if(latest_result == PING_RESULT_TIMEOUT ||
            latest_result == PING_RESULT_ERROR) {
        tint.r = 1.00f;
        tint.g = 0.12f;
        tint.b = 0.10f;
    }
    else {
        tint.r = 0.56f;
        tint.g = 0.76f;
        tint.b = 1.00f;
    }

    return tint;
}

static void ping_reply_callback(const uint8_t *ip, uint16_t sequence,
                                uint64_t delta_us, uint8_t ttl,
                                const uint8_t *data, size_t length) {
    (void)data;
    (void)length;

    if(memcmp(ip, ping_target, sizeof(ping_target)) != 0)
        return;

    reply_sequence = sequence;
    reply_delta_us = delta_us > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_us;
    reply_ttl = ttl;
    reply_event++;
}

static pending_ping_t *find_pending_ping(uint16_t sequence) {
    pending_ping_t *pending = &pending_pings[sequence % PING_PENDING_SLOTS];

    if(pending->active && pending->sequence == sequence)
        return pending;
    return NULL;
}

static void process_ping_reply(uint32_t *handled_event) {
    pending_ping_t *pending;
    uint16_t sequence;
    uint32_t delta_us;
    uint8_t ttl;

    if(*handled_event == reply_event)
        return;

    sequence = reply_sequence;
    delta_us = reply_delta_us;
    ttl = reply_ttl;
    *handled_event = reply_event;
    pending = find_pending_ping(sequence);

    if(pending)
        pending->active = 0;

    received_count++;
    total_reply_us += delta_us;
    latest_reply_us = delta_us;
    latest_result = PING_RESULT_REPLY;
    add_ping_log("#%04u  %lu.%lu ms  ttl=%u  reply",
                 sequence,
                 (unsigned long)(delta_us / 1000),
                 (unsigned long)((delta_us / 100) % 10), ttl);
    printf("Ping Cube: reply from 8.8.8.8 seq=%u ttl=%u time=%lu.%lu ms\n",
           sequence, ttl,
           (unsigned long)(delta_us / 1000),
           (unsigned long)((delta_us / 100) % 10));
}

static void process_ping_timeouts(uint64_t now_ms) {
    int i;

    for(i = 0; i < PING_PENDING_SLOTS; ++i) {
        pending_ping_t *pending = &pending_pings[i];

        if(pending->active && now_ms - pending->sent_ms >= PING_TIMEOUT_MS) {
            pending->active = 0;
            timeout_count++;
            latest_result = PING_RESULT_TIMEOUT;
            add_ping_log("#%04u  timeout after %u ms",
                         pending->sequence, PING_TIMEOUT_MS);
            printf("Ping Cube: timeout from 8.8.8.8 seq=%u after %u ms\n",
                   pending->sequence, PING_TIMEOUT_MS);
        }
    }
}

static void send_ping(uint64_t now_ms) {
    pending_ping_t *pending;
    uint16_t sequence = next_ping_sequence++;
    int result;

    if(!net_default_dev) {
        latest_result = PING_RESULT_ERROR;
        add_ping_log("#%04u  no network device", sequence);
        return;
    }

    pending = &pending_pings[sequence % PING_PENDING_SLOTS];
    pending->sequence = sequence;
    pending->sent_ms = now_ms;
    pending->active = 1;

    result = net_icmp_send_echo(net_default_dev, ping_target,
                                PING_IDENTIFIER, sequence,
                                ping_payload, sizeof(ping_payload) - 1);
    if(result < 0) {
        pending->active = 0;
        latest_result = PING_RESULT_ERROR;
        add_ping_log("#%04u  send failed (%d)", sequence, result);
        printf("Ping Cube: ICMP send failed for seq=%u (%d)\n",
               sequence, result);
        return;
    }

    transmitted_count++;
    console_dirty = 1;
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
    uint64_t next_ping_ms = start_time + 250;
    uint32_t handled_reply_event = reply_event;
    net_echo_cb previous_echo_callback;

    (void)argc;
    (void)argv;

    describe_network(ip_address, network_status);
    snprintf(console_source_ip, sizeof(console_source_ip), "%s", ip_address);
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

    add_ping_log("icmp console initialized");
    add_ping_log("target 8.8.8.8 every 250 ms");
    if(make_console_texture() < 0) {
        printf("Ping Cube: unable to allocate the console texture\n");
        pvr_mem_free(cube_texture);
        return 1;
    }

    previous_echo_callback = net_icmp_echo_cb;
    net_icmp_echo_cb = ping_reply_callback;

    while(!start_pressed()) {
        const uint64_t now_ms = timer_ms_gettime64();
        const float elapsed = (float)(now_ms - start_time) * 0.001f;
        const float angle_y = 0.55f + elapsed * 0.28f;
        const float angle_x = -0.32f + fsin(elapsed * 0.20f) * 0.24f;

        process_ping_reply(&handled_reply_event);
        process_ping_timeouts(now_ms);

        if(now_ms >= next_ping_ms) {
            send_ping(now_ms);
            next_ping_ms = now_ms + PING_INTERVAL_MS;
        }

        if(console_dirty)
            update_console_texture();

        draw_scene(angle_x, angle_y, ping_color());
    }

    net_icmp_echo_cb = previous_echo_callback;
    free(console_pixels);
    pvr_mem_free(console_texture);
    pvr_mem_free(cube_texture);
    return 0;
}
