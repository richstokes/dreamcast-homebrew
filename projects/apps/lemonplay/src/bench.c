/* Decode benchmark (build with VARIANT=decodebench): downloads short clips
   of one video at several sizes and bitrates, then times the MPEG-1 decoder
   and the YUV converter upload on each picture with the network idle. The
   download also measures TCP throughput from the server. The results set
   ZeroConfigDLNA's --dreamcast-mode budget. */
#include "bench.h"

#include "http.h"
#include "mpeg.h"

#include <dc/pvr.h>
#include <dc/sq.h>
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIP_BYTES (9u << 20)
#define CLIP_SECONDS 10
#define CLIP_START 300

typedef struct {
    int width;      /* box width; the height follows the source's shape */
    float bpp;      /* bits per pixel */
} config_t;

static const config_t configs[] = {
    {320, 0.5f}, {368, 0.5f}, {448, 0.5f}, {512, 0.5f}, {576, 0.5f}, {640, 0.5f},
    {448, 0.25f}, {448, 1.0f}, {448, 1.5f},
};

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

#define MAX_PICTURES 1024

/* Find the AVI's video chunks: their offsets and sizes go into pos/size. */
static int find_video(const uint8_t *buf, uint32_t len, uint32_t *pos, uint32_t *size_out,
                      int *w, int *h, double *fps) {
    uint32_t at = 12;
    int n = 0;
    while(at + 8 <= len) {
        const uint8_t *c = buf + at;
        uint32_t size = le32(c + 4);
        if(!memcmp(c, "LIST", 4) || !memcmp(c, "RIFF", 4)) {
            at += 12;
            continue;
        }
        if(!memcmp(c, "strh", 4) && !memcmp(c + 8, "vids", 4)) {
            uint32_t scale = le32(c + 8 + 20), rate = le32(c + 8 + 24);
            *fps = scale ? (double)rate / scale : 0;
        }
        if(!memcmp(c, "strf", 4) && *w == 0 && size >= 40 && le32(c + 8) == 40) {
            *w = (int)le32(c + 12);
            *h = (int)le32(c + 16);
        }
        if(c[0] == '0' && c[1] == '0' && c[2] == 'd' && at + 8 + size <= len && n < MAX_PICTURES) {
            pos[n] = at + 8;
            size_out[n] = size;
            n++;
        }
        at += 8 + size + (size & 1);
    }
    return n;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void upload(const plm_frame_t *frame, pvr_ptr_t tex, int tw, int th) {
    PVR_SET(PVR_YUV_ADDR, ((uint32_t)tex) & 0xffffff);
    PVR_SET(PVR_YUV_CFG, (((th >> 4) - 1) << 8) | ((tw >> 4) - 1));
    PVR_GET(PVR_YUV_CFG);
    int mb_w = frame->y.width >> 4, mb_h = frame->y.height >> 4;
    int pad = (tw >> 4) - mb_w;
    uint32_t *src = frame->display;
    uint32_t *d = SQ_MASK_DEST((void *)PVR_TA_YUV_CONV);
    sq_lock((void *)PVR_TA_YUV_CONV);
    for(int y = 0; y < mb_h; y++) {
        sq_fast_cpy(d, src, mb_w * 12);
        src += 96 * mb_w;
        for(int k = 0; k < pad * 12; k++) sq_flush(d);
    }
    sq_unlock();
}

void bench_run(const char *media_url) {
    uint8_t *buf = memalign(32, CLIP_BYTES);
    uint32_t *times = malloc(MAX_PICTURES * sizeof(uint32_t));
    uint32_t *pos = malloc(MAX_PICTURES * sizeof(uint32_t));
    uint32_t *sizes = malloc(MAX_PICTURES * sizeof(uint32_t));
    pvr_ptr_t tex = pvr_mem_malloc(1024 * 512 * 2);
    if(!buf || !times || !pos || !sizes || !tex) {
        printf("LP DECODEBENCH: out of memory\n");
        return;
    }
    printf("LP DECODEBENCH: %d s clips from %d s; w x h fps kbps | net KB/s | decode avg p95 max ms | upload ms | MB/s\n",
           CLIP_SECONDS, CLIP_START);
    for(size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        const config_t *cfg = &configs[i];
        char url[600];
        snprintf(url, sizeof(url), "%s%ct=%d&d=%d&a=pcm&w=%d&h=480&mbs=40000&bpp=%.2f", media_url,
                 strchr(media_url, '?') ? '&' : '?', CLIP_START, CLIP_SECONDS, cfg->width, cfg->bpp);

        uint64_t t0 = timer_us_gettime64();
        http_t h;
        uint32_t len = 0;
        if(http_open(&h, url, "GET", NULL, NULL, 0, 30000) == 0) {
            for(;;) {
                int n = http_read(&h, buf + len, CLIP_BYTES - len, 20000, NULL);
                if(n <= 0) break;
                len += n;
                if(len == CLIP_BYTES) break;
            }
            http_close(&h);
        }
        uint64_t t1 = timer_us_gettime64();
        if(len < 1024) {
            printf("LP DECODEBENCH: download failed for width %d\n", cfg->width);
            continue;
        }
        float net = len / 1024.0f / ((t1 - t0) / 1e6f);

        int w = 0, hgt = 0;
        double fps = 0;
        int pictures = find_video(buf, len, pos, sizes, &w, &hgt, &fps);
        uint32_t es = 0;
        for(int k = 0; k < pictures; k++) es += sizes[k];
        /* pl_mpeg's SH-4 buffer is a power-of-two ring: feed it the way the
           player does, a few pictures ahead of the decoder. */
        plm_buffer_t *pb = plm_buffer_create_with_capacity(512 * 1024);
        plm_video_t *v = plm_video_create_with_buffer(pb, 1);
        plm_video_set_no_delay(v, 1);
        int fed = 0;

        int tw = 16, th = 16;
        while(tw < w) tw <<= 1;
        while(th < hgt) th <<= 1;
        int n = 0;
        uint64_t total = 0, upload_us = 0;
        for(;;) {
            while(fed < pictures && fed < n + 3) {
                plm_buffer_write(pb, buf + pos[fed], sizes[fed]);
                fed++;
            }
            if(fed == pictures) plm_buffer_signal_end(pb);
            uint64_t a = timer_us_gettime64();
            plm_frame_t *f = plm_video_decode(v);
            uint64_t b = timer_us_gettime64();
            if(!f) break;
            upload(f, tex, tw, th);
            uint64_t c = timer_us_gettime64();
            if(n < MAX_PICTURES) times[n] = (uint32_t)(b - a);
            n++;
            total += b - a;
            upload_us += c - b;
        }
        plm_video_destroy(v);
        if(!n) {
            printf("LP DECODEBENCH: no pictures decoded at width %d\n", cfg->width);
            continue;
        }
        float avg = total / 1000.0f / n;
        int kept = n < MAX_PICTURES ? n : MAX_PICTURES;
        qsort(times, kept, sizeof(uint32_t), cmp_u32);
        float p95 = times[kept * 95 / 100] / 1000.0f, mx = times[kept - 1] / 1000.0f;
        int mbs = (w / 16) * (hgt / 16);
        float kbps = es * 8.0f / 1000.0f / (pictures / (fps > 0 ? fps : 24));
        printf("LP DECODEBENCH: %dx%d %.2ffps %.0fkbps bpp %.2f | net %.0f KB/s | decode %.2f %.2f %.2f ms | upload %.2f ms | %.0f MB/s\n",
               w, hgt, fps, kbps, cfg->bpp, net, avg, p95, mx, upload_us / 1000.0f / n, mbs / (avg / 1000.0f));
    }
    pvr_mem_free(tex);
    free(times);
    free(pos);
    free(sizes);
    free(buf);
    printf("LP DECODEBENCH: done\n");
}
