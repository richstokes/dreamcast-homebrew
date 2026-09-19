/*
 * Ping Cube -- a network-aware KallistiOS demo for the Sega Dreamcast.
 *
 * The Broadband Adapter is acquired through KOS's built-in DHCP client. When
 * no Ethernet adapter is present the demo dials a DreamPi-style PPP peer with
 * the Dreamcast modem instead (see modem.c). KOS's IPv4 stack handles ICMP
 * echo requests, so the assigned address is pingable whenever the emulator or
 * link exposes the console at layer 2/3.
 *
 * Each of the six cube faces carries a different panel of link information,
 * drawn with the BIOS font into its own texture and mapped with the low-level
 * PowerVR API. The analog triggers speed up and slow down the rotation.
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

#include "ping-cube.h"

#define TEXTURE_SIZE 256
#define TEXTURE_BYTES (TEXTURE_SIZE * TEXTURE_SIZE * sizeof(uint16_t))
#define CONSOLE_WIDTH 512
#define CONSOLE_HEIGHT 256
#define CONSOLE_BYTES (CONSOLE_WIDTH * CONSOLE_HEIGHT * sizeof(uint16_t))
#define CONSOLE_LOG_LINES 5
#define PING_INTERVAL_MS 250
#define PING_TIMEOUT_MS 2500
#define PING_PENDING_SLOTS 16
#define PING_IDENTIFIER 0x5043

/* Faces, in the order of cube_faces below. */
#define FACE_FRONT 0
#define FACE_BACK 1
#define FACE_LEFT 2
#define FACE_RIGHT 3
#define FACE_TOP 4
#define FACE_BOTTOM 5
#define FACE_COUNT 6
#define FACE_LINES 6
#define FACE_TEXT_MAX 21
#define FACE_ALL_DIRTY ((1u << FACE_COUNT) - 1u)

/* The triggers accelerate the spin by this much per second at full pressure. */
#define SPIN_ACCELERATION 1.6f
#define SPIN_MAXIMUM 8.0f
#define TRIGGER_DEADZONE 16

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

typedef enum link_kind {
    LINK_NONE,
    LINK_ETHERNET,
    LINK_MODEM
} link_kind_t;

typedef struct face_panel {
    char line[FACE_LINES][FACE_TEXT_MAX + 1];
    uint16_t color[FACE_LINES];
} face_panel_t;

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
static const uint8_t cube_faces[FACE_COUNT][4] = {
    {2, 3, 0, 1}, /* front */
    {7, 6, 5, 4}, /* back */
    {6, 2, 4, 0}, /* left */
    {3, 7, 1, 5}, /* right */
    {6, 7, 2, 3}, /* top */
    {0, 1, 4, 5}  /* bottom */
};

static const float face_brightness[FACE_COUNT] = {
    1.00f, 0.78f, 0.86f, 0.92f, 1.00f, 0.72f
};

static const float face_uv[4][2] = {
    {0.0f, 0.0f},
    {1.0f, 0.0f},
    {0.0f, 1.0f},
    {1.0f, 1.0f}
};

/* Baselines for the six text rows of a face panel. */
static const int face_text_rows[FACE_LINES] = {24, 64, 108, 150, 192, 222};

static pvr_ptr_t face_textures[FACE_COUNT];
static pvr_poly_hdr_t face_headers[FACE_COUNT];
static uint16_t *face_background;
static uint16_t *face_staging;
static uint32_t face_dirty = FACE_ALL_DIRTY;

static pvr_ptr_t console_texture;
static pvr_poly_hdr_t console_header;
static uint16_t *console_pixels;
static uint16_t *console_background;

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
static uint32_t minimum_reply_us;
static uint32_t maximum_reply_us;
static ping_result_t latest_result = PING_RESULT_WAITING;
static int console_dirty = 1;

static link_kind_t link_kind;
static modem_stage_t modem_stage = MODEM_STAGE_IDLE;
static uint64_t uptime_seconds;
static float spin_speed = 1.0f;
static unsigned spin_hundredths = 100;
static int exiting;

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

#define COLOR_TITLE  rgb565(255, 255, 255)
#define COLOR_LABEL  rgb565(127, 238, 255)
#define COLOR_VALUE  rgb565(255, 226, 76)
#define COLOR_STATUS rgb565(188, 255, 210)
#define COLOR_DIM    rgb565(155, 192, 205)

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
    char clipped[FACE_TEXT_MAX + 1];
    size_t length;
    int x;
    uint16_t *shadow;
    uint16_t *target;

    snprintf(clipped, sizeof(clipped), "%s", text);
    length = strlen(clipped);
    if(!length)
        return;

    x = (TEXTURE_SIZE - (int)length * BFONT_THIN_WIDTH) / 2;
    if(x < 0)
        x = 0;

    shadow = pixels + (y + 2) * TEXTURE_SIZE + x + 2;
    target = pixels + y * TEXTURE_SIZE + x;

    bfont_draw_str_ex(shadow, TEXTURE_SIZE, rgb565(0, 8, 12), 0,
                      16, false, clipped);
    bfont_draw_str_ex(target, TEXTURE_SIZE, color, 0,
                      16, false, clipped);
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

/* ------------------------------------------------------------------ */
/* Link description                                                     */
/* ------------------------------------------------------------------ */

static int has_address(const uint8_t address[4]) {
    return address[0] | address[1] | address[2] | address[3];
}

static netif_t *active_netif(void) {
    netif_t *netif = net_default_dev;

    if(link_kind == LINK_MODEM && modem_stage != MODEM_STAGE_READY)
        return NULL;
    if(netif && has_address(netif->ip_addr))
        return netif;
    return NULL;
}

static int link_ready(void) {
    return active_netif() != NULL;
}

static void format_address(char *buffer, size_t size, const uint8_t ip[4]) {
    if(has_address(ip))
        snprintf(buffer, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    else
        snprintf(buffer, size, "NONE");
}

static void format_latency(char *buffer, size_t size, uint32_t microseconds) {
    uint32_t tenths = microseconds / 100;

    snprintf(buffer, size, "%lu.%lu MS",
             (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

static const char *adapter_name(void) {
    netif_t *netif = net_default_dev;

    if(link_kind == LINK_MODEM)
        return "DIAL-UP MODEM";
    if(netif && !strcmp(netif->name, "bba"))
        return "BROADBAND ADAPTER";
    if(netif && !strcmp(netif->name, "la"))
        return "LAN ADAPTER";
    if(netif)
        return netif->descr ? "KOS NET DEVICE" : "UNKNOWN ADAPTER";
    return "NONE DETECTED";
}

/* Short status for the front face: at most FACE_TEXT_MAX characters. */
static const char *link_status_text(void) {
    if(exiting)
        return "HANGING UP...";

    if(link_kind == LINK_MODEM) {
        switch(modem_stage) {
            case MODEM_STAGE_IDLE:
            case MODEM_STAGE_DETECTING:
                return "FINDING MODEM";
            case MODEM_STAGE_DIALING:
                return "DIALING DREAMPI";
            case MODEM_STAGE_NEGOTIATING:
                return "PPP NEGOTIATION";
            case MODEM_STAGE_FAILED:
                return modem_link_failure();
            case MODEM_STAGE_READY:
                break;
        }
    }

    if(link_ready())
        return "ICMP ECHO READY";
    if(link_kind == LINK_ETHERNET)
        return "WAITING FOR DHCP";
    return "NO NETWORK LINK";
}

static void link_address_text(char *buffer, size_t size) {
    netif_t *netif = active_netif();

    if(netif)
        format_address(buffer, size, netif->ip_addr);
    else if(link_kind == LINK_MODEM && modem_stage != MODEM_STAGE_FAILED)
        snprintf(buffer, size, "CONNECTING");
    else
        snprintf(buffer, size, "NO ADDRESS");
}

/* ------------------------------------------------------------------ */
/* Face panels                                                          */
/* ------------------------------------------------------------------ */

static void set_line(face_panel_t *panel, int index, uint16_t color,
                     const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(panel->line[index], sizeof(panel->line[index]), format,
              arguments);
    va_end(arguments);
    panel->color[index] = color;
}

static void build_identity_panel(face_panel_t *panel) {
    char address[FACE_TEXT_MAX + 1];

    link_address_text(address, sizeof(address));
    set_line(panel, 0, COLOR_TITLE, "PING CUBE");
    set_line(panel, 1, COLOR_LABEL, "DREAMCAST IP");
    set_line(panel, 2, COLOR_VALUE, "%s", address);
    set_line(panel, 3, COLOR_STATUS, "%s", link_status_text());
    /* The BIOS font consumes ISO-8859-1, where 0xa9 is the copyright sign. */
    set_line(panel, 4, COLOR_DIM, "\xa9 Rich Stokes 2026");
    set_line(panel, 5, COLOR_TITLE, "START: EXIT");
}

static void build_route_panel(face_panel_t *panel) {
    netif_t *netif = active_netif();
    char gateway[FACE_TEXT_MAX + 1] = "UNKNOWN";
    char netmask[FACE_TEXT_MAX + 1] = "UNKNOWN";

    if(netif) {
        format_address(gateway, sizeof(gateway), netif->gateway);
        format_address(netmask, sizeof(netmask), netif->netmask);
    }

    set_line(panel, 0, COLOR_TITLE, "ROUTE");
    set_line(panel, 1, COLOR_LABEL,
             link_kind == LINK_MODEM ? "PPP PEER" : "GATEWAY");
    set_line(panel, 2, COLOR_VALUE, "%s", gateway);
    set_line(panel, 3, COLOR_LABEL, "NETMASK");
    set_line(panel, 4, COLOR_VALUE, "%s", netmask);
    set_line(panel, 5, COLOR_DIM, "%s",
             link_kind == LINK_MODEM ? "POINT TO POINT" : "IPV4 SUBNET");
}

static void build_name_panel(face_panel_t *panel) {
    netif_t *netif = active_netif();
    char dns[FACE_TEXT_MAX + 1] = "UNKNOWN";
    char broadcast[FACE_TEXT_MAX + 1] = "UNKNOWN";
    int mtu = netif ? netif->mtu : 0;

    if(netif) {
        format_address(dns, sizeof(dns), netif->dns);
        format_address(broadcast, sizeof(broadcast), netif->broadcast);
    }

    set_line(panel, 0, COLOR_TITLE, "NAMES");
    set_line(panel, 1, COLOR_LABEL, "DNS SERVER");
    set_line(panel, 2, COLOR_VALUE, "%s", dns);
    set_line(panel, 3, COLOR_LABEL, "BROADCAST");
    set_line(panel, 4, COLOR_VALUE, "%s", broadcast);
    if(mtu)
        set_line(panel, 5, COLOR_DIM, "MTU %d BYTES", mtu);
    else
        set_line(panel, 5, COLOR_DIM, "");
}

static void build_adapter_panel(face_panel_t *panel) {
    netif_t *netif = net_default_dev;
    int rate = modem_link_rate();

    set_line(panel, 0, COLOR_TITLE, "ADAPTER");
    set_line(panel, 1, COLOR_LABEL, "HARDWARE");
    set_line(panel, 2, COLOR_VALUE, "%s", adapter_name());

    if(link_kind == LINK_MODEM) {
        set_line(panel, 3, COLOR_LABEL, "CARRIER");
        if(rate > 0)
            set_line(panel, 4, COLOR_VALUE, "%d BPS", rate);
        else
            set_line(panel, 4, COLOR_VALUE, "%s", link_status_text());
        set_line(panel, 5, COLOR_DIM, "PPP OVER DREAMPI");
    }
    else if(netif) {
        set_line(panel, 3, COLOR_LABEL, "MAC ADDRESS");
        set_line(panel, 4, COLOR_VALUE, "%02X:%02X:%02X:%02X:%02X:%02X",
                 netif->mac_addr[0], netif->mac_addr[1], netif->mac_addr[2],
                 netif->mac_addr[3], netif->mac_addr[4], netif->mac_addr[5]);
        set_line(panel, 5, COLOR_DIM, "INTERFACE %s%d",
                 netif->name, netif->index);
    }
    else {
        set_line(panel, 3, COLOR_LABEL, "STATUS");
        set_line(panel, 4, COLOR_VALUE, "%s", link_status_text());
        set_line(panel, 5, COLOR_DIM, "BBA OR MODEM");
    }
}

static void build_ping_panel(face_panel_t *panel) {
    char latency[FACE_TEXT_MAX + 1];
    uint32_t completed = received_count + timeout_count;
    uint32_t loss_percent = completed ? (timeout_count * 100) / completed : 0;

    set_line(panel, 0, COLOR_TITLE, "PING 8.8.8.8");
    set_line(panel, 1, COLOR_LABEL, "LATEST REPLY");

    switch(latest_result) {
        case PING_RESULT_REPLY:
            format_latency(latency, sizeof(latency), latest_reply_us);
            set_line(panel, 2, COLOR_VALUE, "%s", latency);
            break;
        case PING_RESULT_TIMEOUT:
            set_line(panel, 2, COLOR_VALUE, "TIMEOUT");
            break;
        case PING_RESULT_ERROR:
            set_line(panel, 2, COLOR_VALUE, "SEND ERROR");
            break;
        default:
            set_line(panel, 2, COLOR_VALUE,
                     link_ready() ? "WAITING" : "NO LINK");
            break;
    }

    if(received_count) {
        format_latency(latency, sizeof(latency),
                       (uint32_t)(total_reply_us / received_count));
        set_line(panel, 3, COLOR_STATUS, "AVG %s", latency);
        set_line(panel, 4, COLOR_STATUS, "MIN %lu MAX %lu MS",
                 (unsigned long)(minimum_reply_us / 1000),
                 (unsigned long)(maximum_reply_us / 1000));
    }
    else {
        set_line(panel, 3, COLOR_STATUS, "AVG --.- MS");
        set_line(panel, 4, COLOR_STATUS, "MIN -- MAX -- MS");
    }

    set_line(panel, 5, COLOR_DIM, "SENT %lu LOSS %lu%%",
             (unsigned long)transmitted_count, (unsigned long)loss_percent);
}

static void build_session_panel(face_panel_t *panel) {
    set_line(panel, 0, COLOR_TITLE, "SESSION");
    set_line(panel, 1, COLOR_LABEL, "UPTIME");
    set_line(panel, 2, COLOR_VALUE, "%02lu:%02lu:%02lu",
             (unsigned long)(uptime_seconds / 3600),
             (unsigned long)((uptime_seconds / 60) % 60),
             (unsigned long)(uptime_seconds % 60));
    set_line(panel, 3, COLOR_LABEL, "SPIN SPEED");
    set_line(panel, 4, COLOR_VALUE, "x%u.%02u",
             spin_hundredths / 100, spin_hundredths % 100);
    set_line(panel, 5, COLOR_DIM, "L SLOWER  R FASTER");
}

static void build_face_panel(int face, face_panel_t *panel) {
    memset(panel, 0, sizeof(*panel));

    switch(face) {
        case FACE_FRONT:  build_identity_panel(panel); break;
        /* The cube turns front, left, back, right: IP, route, DNS, adapter. */
        case FACE_LEFT:   build_route_panel(panel);    break;
        case FACE_BACK:   build_name_panel(panel);     break;
        case FACE_RIGHT:  build_adapter_panel(panel);  break;
        case FACE_TOP:    build_ping_panel(panel);     break;
        default:          build_session_panel(panel);  break;
    }
}

static void update_face_texture(int face) {
    face_panel_t panel;
    int i;

    build_face_panel(face, &panel);
    memcpy(face_staging, face_background, TEXTURE_BYTES);

    for(i = 0; i < FACE_LINES; ++i)
        draw_centered_text(face_staging, face_text_rows[i], panel.line[i],
                           panel.color[i]);

    pvr_txr_load(face_staging, face_textures[face], TEXTURE_BYTES);
}

static void update_dirty_faces(void) {
    int face;

    for(face = 0; face < FACE_COUNT; ++face) {
        if(face_dirty & (1u << face))
            update_face_texture(face);
    }

    face_dirty = 0;
}

static int make_face_textures(void) {
    pvr_poly_cxt_t context;
    int face;

    face_background = aligned_alloc(32, TEXTURE_BYTES);
    face_staging = aligned_alloc(32, TEXTURE_BYTES);
    if(!face_background || !face_staging)
        return -1;

    fill_texture_background(face_background);

    for(face = 0; face < FACE_COUNT; ++face) {
        face_textures[face] = pvr_mem_malloc(TEXTURE_BYTES);
        if(!face_textures[face])
            return -1;

        pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
                         PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         TEXTURE_SIZE, TEXTURE_SIZE, face_textures[face],
                         PVR_FILTER_BILINEAR);
        /* All strips wind clockwise when viewed from outside the cube. */
        context.gen.culling = PVR_CULLING_CCW;
        context.depth.comparison = PVR_DEPTHCMP_GREATER;
        context.depth.write = true;
        context.txr.env = PVR_TXRENV_MODULATE;
        context.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&face_headers[face], &context);
    }

    face_dirty = FACE_ALL_DIRTY;
    update_dirty_faces();
    return 0;
}

static void free_face_textures(void) {
    int face;

    for(face = 0; face < FACE_COUNT; ++face) {
        if(face_textures[face]) {
            pvr_mem_free(face_textures[face]);
            face_textures[face] = NULL;
        }
    }

    free(face_background);
    free(face_staging);
    face_background = NULL;
    face_staging = NULL;
}

/* ------------------------------------------------------------------ */
/* Console panel                                                        */
/* ------------------------------------------------------------------ */

static void fill_console_background(uint16_t *pixels) {
    int x;
    int y;

    for(y = 0; y < CONSOLE_HEIGHT; ++y) {
        for(x = 0; x < CONSOLE_WIDTH; ++x) {
            unsigned glow = ((x / 32) + (y / 24)) & 1;
            unsigned scanline = (y & 3) == 0;

            pixels[y * CONSOLE_WIDTH + x] =
                rgb565(2 + glow, 7 + glow * 2 + scanline,
                       9 + glow * 2 + scanline);
        }
    }

    for(x = 0; x < CONSOLE_WIDTH; ++x) {
        pixels[0 * CONSOLE_WIDTH + x] = rgb565(8, 41, 35);
        pixels[1 * CONSOLE_WIDTH + x] = rgb565(8, 41, 35);
        pixels[(CONSOLE_HEIGHT - 2) * CONSOLE_WIDTH + x] = rgb565(8, 41, 35);
        pixels[(CONSOLE_HEIGHT - 1) * CONSOLE_WIDTH + x] = rgb565(8, 41, 35);
    }
}

static void console_link_summary(char *buffer, size_t size) {
    int rate = modem_link_rate();

    if(link_kind == LINK_MODEM && rate > 0)
        snprintf(buffer, size, "MODEM %d BPS", rate);
    else if(link_kind == LINK_MODEM)
        snprintf(buffer, size, "MODEM");
    else if(net_default_dev && !strcmp(net_default_dev->name, "bba"))
        snprintf(buffer, size, "BBA");
    else if(net_default_dev && !strcmp(net_default_dev->name, "la"))
        snprintf(buffer, size, "LAN ADAPTER");
    else if(net_default_dev)
        snprintf(buffer, size, "%.12s", net_default_dev->name);
    else
        snprintf(buffer, size, "NO LINK");
}

static void update_console_texture(void) {
    char line[64];
    char summary[32];
    uint32_t completed = received_count + timeout_count;
    uint32_t loss_percent = completed ? (timeout_count * 100) / completed : 0;
    uint32_t average_tenths = received_count
        ? (uint32_t)(total_reply_us / received_count / 100)
        : 0;
    int i;

    memcpy(console_pixels, console_background, CONSOLE_BYTES);

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

    console_link_summary(summary, sizeof(summary));
    snprintf(line, sizeof(line), "VIA %s  SPIN x%u.%02u",
             summary, spin_hundredths / 100, spin_hundredths % 100);
    draw_console_text(94, line, rgb565(35, 104, 118));

    for(i = 0; i < CONSOLE_LOG_LINES; ++i)
        draw_console_text(122 + i * 26, ping_log[i], rgb565(28, 79, 69));

    pvr_txr_load(console_pixels, console_texture, CONSOLE_BYTES);
    console_dirty = 0;
}

static int make_console_texture(void) {
    pvr_poly_cxt_t context;

    console_pixels = aligned_alloc(32, CONSOLE_BYTES);
    console_background = aligned_alloc(32, CONSOLE_BYTES);
    if(!console_pixels || !console_background)
        return -1;

    fill_console_background(console_background);

    console_texture = pvr_mem_malloc(CONSOLE_BYTES);
    if(!console_texture)
        return -1;

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

/* ------------------------------------------------------------------ */
/* Rendering                                                            */
/* ------------------------------------------------------------------ */

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

    for(face = 0; face < FACE_COUNT; ++face) {
        const uint32_t color = PVR_PACK_COLOR(1.0f,
                                              face_brightness[face] * tint.r,
                                              face_brightness[face] * tint.g,
                                              face_brightness[face] * tint.b);

        pvr_prim(&face_headers[face], sizeof(face_headers[face]));

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

/* ------------------------------------------------------------------ */
/* ICMP                                                                 */
/* ------------------------------------------------------------------ */

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
    if(!minimum_reply_us || delta_us < minimum_reply_us)
        minimum_reply_us = delta_us;
    if(delta_us > maximum_reply_us)
        maximum_reply_us = delta_us;
    latest_result = PING_RESULT_REPLY;
    face_dirty |= (1u << FACE_TOP);
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
            face_dirty |= (1u << FACE_TOP);
            add_ping_log("#%04u  timeout after %u ms",
                         pending->sequence, PING_TIMEOUT_MS);
            printf("Ping Cube: timeout from 8.8.8.8 seq=%u after %u ms\n",
                   pending->sequence, PING_TIMEOUT_MS);
        }
    }
}

static void send_ping(uint64_t now_ms) {
    netif_t *netif = active_netif();
    pending_ping_t *pending;
    uint16_t sequence = next_ping_sequence++;
    int result;

    if(!netif) {
        latest_result = PING_RESULT_ERROR;
        face_dirty |= (1u << FACE_TOP);
        add_ping_log("#%04u  no network device", sequence);
        return;
    }

    pending = &pending_pings[sequence % PING_PENDING_SLOTS];
    pending->sequence = sequence;
    pending->sent_ms = now_ms;
    pending->active = 1;

    result = net_icmp_send_echo(netif, ping_target, PING_IDENTIFIER, sequence,
                                ping_payload, sizeof(ping_payload) - 1);
    if(result < 0) {
        pending->active = 0;
        latest_result = PING_RESULT_ERROR;
        face_dirty |= (1u << FACE_TOP);
        add_ping_log("#%04u  send failed (%d)", sequence, result);
        printf("Ping Cube: ICMP send failed for seq=%u (%d)\n",
               sequence, result);
        return;
    }

    transmitted_count++;
    face_dirty |= (1u << FACE_TOP);
    console_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

typedef struct controls {
    int start;
    int left_trigger;
    int right_trigger;
} controls_t;

static controls_t read_controls(void) {
    controls_t controls = {0, 0, 0};

    MAPLE_FOREACH_BEGIN(MAPLE_FUNC_CONTROLLER, cont_state_t, state)
        if(state->buttons & CONT_START)
            controls.start = 1;
        if(state->ltrig > controls.left_trigger)
            controls.left_trigger = state->ltrig;
        if(state->rtrig > controls.right_trigger)
            controls.right_trigger = state->rtrig;
    MAPLE_FOREACH_END()

    return controls;
}

static float trigger_amount(int value) {
    if(value <= TRIGGER_DEADZONE)
        return 0.0f;
    return (float)(value - TRIGGER_DEADZONE) / (float)(255 - TRIGGER_DEADZONE);
}

/* R accelerates the spin, L slows it and can bring the cube to a stop so a
   face can be read. */
static void update_spin_speed(const controls_t *controls, float delta_seconds) {
    const float change = trigger_amount(controls->right_trigger) -
                         trigger_amount(controls->left_trigger);
    unsigned hundredths;

    if(change != 0.0f) {
        spin_speed += change * SPIN_ACCELERATION * delta_seconds;

        if(spin_speed < 0.0f)
            spin_speed = 0.0f;
        else if(spin_speed > SPIN_MAXIMUM)
            spin_speed = SPIN_MAXIMUM;
    }

    hundredths = (unsigned)(spin_speed * 100.0f + 0.5f);
    if(hundredths != spin_hundredths) {
        spin_hundredths = hundredths;
        face_dirty |= (1u << FACE_BOTTOM);
        console_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Link setup                                                           */
/* ------------------------------------------------------------------ */

static void report_interface(void) {
    netif_t *netif = net_default_dev;

    if(!netif)
        return;

    printf("Ping Cube netif: %s, ip %u.%u.%u.%u, netmask %u.%u.%u.%u, "
           "gateway %u.%u.%u.%u, dns %u.%u.%u.%u, mtu %d\n",
           netif->name,
           netif->ip_addr[0], netif->ip_addr[1],
           netif->ip_addr[2], netif->ip_addr[3],
           netif->netmask[0], netif->netmask[1],
           netif->netmask[2], netif->netmask[3],
           netif->gateway[0], netif->gateway[1],
           netif->gateway[2], netif->gateway[3],
           netif->dns[0], netif->dns[1], netif->dns[2], netif->dns[3],
           netif->mtu);
}

static void refresh_source_ip(void) {
    netif_t *netif = active_netif();

    if(netif)
        format_address(console_source_ip, sizeof(console_source_ip),
                       netif->ip_addr);
    else
        snprintf(console_source_ip, sizeof(console_source_ip), "%s",
                 link_kind == LINK_MODEM ? "MODEM" : "NO LINK");
    console_dirty = 1;
}

/* KOS's INIT_NET already chose Ethernet when one was present. The BBA and the
   modem share the expansion port, so the modem is only probed when no default
   device exists. */
static void start_link(void) {
    if(net_default_dev) {
        link_kind = LINK_ETHERNET;
        printf("Ping Cube: using %s\n", adapter_name());
        report_interface();
        printf("Ping Cube: KOS ICMP echo replies are active\n");
        add_ping_log("link %s ready", net_default_dev->name);
        return;
    }

    link_kind = LINK_MODEM;
    add_ping_log("no ethernet adapter; trying modem");
    printf("Ping Cube: no Ethernet adapter, trying the modem\n");

    if(modem_link_start() < 0)
        add_ping_log("could not start the modem worker");
}

static void poll_modem_stage(void) {
    modem_stage_t stage;

    if(link_kind != LINK_MODEM)
        return;

    stage = modem_link_stage();
    if(stage == modem_stage)
        return;

    modem_stage = stage;
    face_dirty = FACE_ALL_DIRTY;

    switch(stage) {
        case MODEM_STAGE_DETECTING:
            add_ping_log("looking for a dreamcast modem");
            break;
        case MODEM_STAGE_DIALING:
            add_ping_log("dialing dreampi (up to 65 s)");
            break;
        case MODEM_STAGE_NEGOTIATING:
            add_ping_log("carrier %d bps; ppp handshake",
                         modem_link_rate());
            break;
        case MODEM_STAGE_READY:
            refresh_source_ip();
            report_interface();
            add_ping_log("ppp up as %s", console_source_ip);
            break;
        case MODEM_STAGE_FAILED:
            latest_result = PING_RESULT_ERROR;
            add_ping_log("modem failed: %s", modem_link_failure());
            break;
        default:
            break;
    }
}

int main(int argc, char **argv) {
    const uint64_t start_time = timer_ms_gettime64();
    uint64_t last_frame_ms = start_time;
    uint64_t next_ping_ms = start_time + PING_INTERVAL_MS;
    uint32_t handled_reply_event = reply_event;
    net_echo_cb previous_echo_callback;
    float yaw = 0.55f;
    float pitch_phase = -0.16f;

    (void)argc;
    (void)argv;

    add_ping_log("icmp console initialized");
    add_ping_log("target 8.8.8.8 every 250 ms");

    pvr_init_defaults();
    pvr_set_bg_color(0.015f, 0.035f, 0.065f);

    start_link();
    refresh_source_ip();

    if(make_face_textures() < 0) {
        printf("Ping Cube: unable to allocate the cube textures\n");
        free_face_textures();
        return 1;
    }

    if(make_console_texture() < 0) {
        printf("Ping Cube: unable to allocate the console texture\n");
        free_face_textures();
        free(console_pixels);
        free(console_background);
        return 1;
    }

    previous_echo_callback = net_icmp_echo_cb;
    net_icmp_echo_cb = ping_reply_callback;

    for(;;) {
        const uint64_t now_ms = timer_ms_gettime64();
        float delta_seconds = (float)(now_ms - last_frame_ms) * 0.001f;
        const uint64_t elapsed_seconds = (now_ms - start_time) / 1000;
        const controls_t controls = read_controls();
        float angle_x;

        last_frame_ms = now_ms;
        if(delta_seconds > 0.1f)
            delta_seconds = 0.1f;

        if(controls.start && !exiting) {
            exiting = 1;
            face_dirty = FACE_ALL_DIRTY;
            if(modem_link_busy()) {
                modem_link_cancel();
                add_ping_log("exiting after the current dial step");
                printf("Ping Cube: exit requested; waiting for the modem\n");
            }
        }

        /* KOS's dial and PPP calls cannot be interrupted, so keep drawing
           until the worker returns rather than tearing down underneath it. */
        if(exiting && !modem_link_busy())
            break;

        poll_modem_stage();
        process_ping_reply(&handled_reply_event);
        process_ping_timeouts(now_ms);

        if(elapsed_seconds != uptime_seconds) {
            uptime_seconds = elapsed_seconds;
            face_dirty |= (1u << FACE_BOTTOM);
        }

        if(!exiting && link_ready() && now_ms >= next_ping_ms) {
            send_ping(now_ms);
            next_ping_ms = now_ms + PING_INTERVAL_MS;
        }
        else if(!link_ready()) {
            next_ping_ms = now_ms + PING_INTERVAL_MS;
        }

        update_spin_speed(&controls, delta_seconds);
        yaw += 0.28f * spin_speed * delta_seconds;
        pitch_phase += 0.16f * spin_speed * delta_seconds;
        if(yaw > 6.2831853f)
            yaw -= 6.2831853f;
        if(pitch_phase > 6.2831853f)
            pitch_phase -= 6.2831853f;
        /* Tumbling on both axes brings the top and bottom panels into view. */
        angle_x = fsin(pitch_phase) * 1.05f - 0.15f;

        if(face_dirty)
            update_dirty_faces();
        if(console_dirty)
            update_console_texture();

        draw_scene(angle_x, yaw, ping_color());
    }

    net_icmp_echo_cb = previous_echo_callback;

    if(link_kind == LINK_MODEM)
        modem_link_shutdown();

    free_face_textures();
    pvr_mem_free(console_texture);
    free(console_pixels);
    free(console_background);
    return 0;
}
