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
#define CLIP_SECONDS 20
#define CLIP_START 300

/* Server profiles to compare (query parameters on top of the defaults). */
static const char *const configs[] = {
#ifdef LP_REPEAT_BENCH
    /* The same clips again and again: hashes must not change. */
    "&bpp=0.25", "&bpp=0.25", "", "&bpp=0.25", "", "&bpp=0.25", "", "&bpp=0.25",
#else
    "", "&bpp=0.25", "&bpp=0.5",
#endif
};

#ifdef LP_PROFILE
uint64_t lp_prof[LP_PROF_COUNT];
uint32_t lp_motion[5];
#endif

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
    printf("LP DECODEBENCH: %d s clips from %d s; profile | w x h fps kbps I-frames | net KB/s | decode avg p95 max ms | upload ms | MB/s\n",
           CLIP_SECONDS, CLIP_START);
    for(size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        const char *cfg = configs[i];
        char url[600];
        snprintf(url, sizeof(url), "%s%ct=%d&d=%d&a=none%s", media_url,
                 strchr(media_url, '?') ? '&' : '?', CLIP_START, CLIP_SECONDS, cfg);
#ifdef LP_PROFILE
        memset(lp_prof, 0, sizeof(lp_prof));
        memset(lp_motion, 0, sizeof(lp_motion));
#endif

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
            printf("LP DECODEBENCH: download failed for '%s'\n", cfg);
            continue;
        }
        float net = len / 1024.0f / ((t1 - t0) / 1e6f);

        int w = 0, hgt = 0;
        double fps = 0;
        int pictures = find_video(buf, len, pos, sizes, &w, &hgt, &fps);
        uint32_t es = 0, es_hash = 2166136261u;
        int intra = 0;
        for(int k = 0; k < pictures; k++) {
            es += sizes[k];
            const uint8_t *p = buf + pos[k];
            for(uint32_t j = 0; j < sizes[k]; j++) es_hash = (es_hash ^ p[j]) * 16777619u;
            for(uint32_t j = 0; j + 5 < sizes[k] && j < 256; j++) {
                if(!p[j] && !p[j + 1] && p[j + 2] == 1 && !p[j + 3]) {
                    intra += ((p[j + 5] >> 3) & 7) == 1;
                    break;
                }
            }
        }
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
        /* FNV-1a over every decoded picture: decoding is integer-only, so
           Flycast and the console must agree bit for bit. */
        uint32_t hash = 2166136261u;
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
            const uint8_t *px = (const uint8_t *)f->display;
            for(int k = 0, len = (f->y.width >> 4) * (f->y.height >> 4) * 384; k < len; k++)
                hash = (hash ^ px[k]) * 16777619u;
            if(n < MAX_PICTURES) times[n] = (uint32_t)(b - a);
            n++;
            total += b - a;
            upload_us += c - b;
        }
        plm_video_destroy(v);
        if(!n) {
            printf("LP DECODEBENCH: no pictures decoded for '%s'\n", cfg);
            continue;
        }
        float avg = total / 1000.0f / n;
        int kept = n < MAX_PICTURES ? n : MAX_PICTURES;
        qsort(times, kept, sizeof(uint32_t), cmp_u32);
        float p95 = times[kept * 95 / 100] / 1000.0f, mx = times[kept - 1] / 1000.0f;
        int mbs = (w / 16) * (hgt / 16);
        float kbps = es * 8.0f / 1000.0f / (pictures / (fps > 0 ? fps : 24));
        printf("LP DECODEBENCH: '%s' | %dx%d %.2ffps %.0fkbps %d I | net %.0f KB/s | decode %.2f %.2f %.2f ms | upload %.2f ms | %.0f MB/s | %d pictures hash %08lx stream %08lx\n",
               cfg[0] ? cfg : "default", w, hgt, fps, kbps, intra, net, avg, p95, mx, upload_us / 1000.0f / n,
               mbs / (avg / 1000.0f), n, (unsigned long)hash, (unsigned long)es_hash);
#ifdef LP_PROFILE
        /* Cycles per macroblock by stage; "other" is header and motion
           vector parsing plus loop overhead. */
        double per = (double)n * mbs;
        uint64_t parts = lp_prof[LP_PROF_VLC] + lp_prof[LP_PROF_IDCT] + lp_prof[LP_PROF_PUT] +
                         lp_prof[LP_PROF_PREDICT] + lp_prof[LP_PROF_SCATTER];
        printf("LP PROFILE: '%s' cycles/MB total %.0f | vlc %.0f idct %.0f put %.0f predict %.0f scatter %.0f other %.0f\n",
               cfg[0] ? cfg : "default", lp_prof[LP_PROF_PICTURE] / per, lp_prof[LP_PROF_VLC] / per,
               lp_prof[LP_PROF_IDCT] / per, lp_prof[LP_PROF_PUT] / per, lp_prof[LP_PROF_PREDICT] / per,
               lp_prof[LP_PROF_SCATTER] / per, (lp_prof[LP_PROF_PICTURE] - parts) / per);
        printf("LP PROFILE: '%s' predicted MBs %% of all: full-pel %.0f (unaligned %.0f) half-h %.0f half-v %.0f half-hv %.0f\n",
               cfg[0] ? cfg : "default", 100.0 * (lp_motion[0] + lp_motion[1]) / per, 100.0 * lp_motion[1] / per,
               100.0 * lp_motion[2] / per, 100.0 * lp_motion[3] / per, 100.0 * lp_motion[4] / per);
#endif
    }
    pvr_mem_free(tex);
    free(times);
    free(pos);
    free(sizes);
    free(buf);
    printf("LP DECODEBENCH: done\n");
}
