/* The player has two threads:

   - the stream thread reads the HTTP response and demuxes the AVI into a
     video queue (one compressed picture per entry) and an audio ring, and
     blocks when either is full, which lets TCP throttle the server;
   - the render thread (the caller's) decodes pictures just before they are
     due, pushes each one through the PowerVR's YUV converter into one of
     three textures (shown, waiting to be shown, being written), and feeds
     the AICA from the audio ring through snd_stream.

   The clock is the content time of the audio the AICA is playing: it starts
   with the AICA and stops with it (pause, rebuffering). Pictures are shown
   by that clock; if decoding falls more than SKIP_BEHIND behind, pictures
   are dropped up to the next intra frame. A seek asks the server for a new
   stream starting at the target time. */
#include "player.h"

#include "gfx.h"
#include "http.h"
#include "input.h"
#include "mpeg.h"

#include <dc/pvr.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/sq.h>
#include <kos.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VQ_BYTES (2u << 20)         /* compressed video queue */
#define AQ_BYTES (1u << 20)         /* audio ring: ~6 s of 44.1 kHz stereo PCM */
#define SND_BUF_BYTES (32 * 1024)   /* AICA buffer per channel: ~0.37 s */
#define START_SECS 1.5              /* buffered before playback starts */
#define DECODE_LEAD 0.030           /* decode a picture this early */
#define SKIP_BEHIND 0.5             /* drop to the next I-frame when this late */
#define SEEK_COMMIT_MS 700
#define OSD_MS 3500

enum { STREAM_CONNECTING, STREAM_RUNNING, STREAM_ENDED, STREAM_FAILED };
enum { AUDIO_NONE, AUDIO_PCM16 };

typedef struct {
    uint8_t *data;
    uint32_t size;          /* a power of two */
    volatile uint32_t head; /* bytes ever written */
    volatile uint32_t tail; /* bytes the reader no longer needs */
} ring_t;

typedef struct {
    char url[480];
    volatile int abort;
    volatile int state;
    char error[64];
    kthread_t *thread;
    http_t http;

    /* From the AVI headers; valid once header_ready is set. */
    volatile int header_ready;
    int video_index, audio_index;
    int width, height;
    double fps;
    int audio_format, audio_rate, audio_channels;

    ring_t vq;                    /* entries: u32 size, data padded to 4 */
    volatile uint32_t frames_in;  /* pictures queued, ever */
    ring_t aq;
    volatile uint32_t bytes_in;   /* network bytes, for the stats line */

    uint8_t buf[16384];
    uint32_t buf_pos, buf_len;
} stream_t;

static struct {
    const entry_t *entry;
    stream_t *s;
    uint8_t *vq_mem, *aq_mem;

    /* Session (one stream request) */
    double start_time;      /* content time the stream starts at */
    int buffering;
    int paused;
    int started;            /* the clock has run at least once this session */

    /* Clock */
    int running;
    double clock_base;
    uint64_t anchor_us;

    /* Video */
    plm_video_t *video;
    plm_buffer_t *vbuf;
    uint32_t frames_out;    /* pictures taken from the queue */
    uint32_t decoded;       /* picture index the decoder is on */
    uint8_t fed_sizes[16];  /* nonzero if picture i (mod 16) had data */
    uint32_t fed;           /* pictures copied into vbuf */
    int vbuf_ended;
    int skipping;
    pvr_ptr_t tex[3];
    pvr_poly_hdr_t hdr[3];
    int tex_w, tex_h, vid_w, vid_h;
    int front, pending;
    double pending_time;
    int have_picture;

    /* Audio */
    snd_stream_hnd_t snd;
    uint8_t *snd_staging;
    uint32_t a_pos;         /* next audio byte to hand the AICA */
    uint32_t a_start;       /* a_pos when the AICA last started */
    volatile int audio_starved;
    int bytes_per_sec;
    int volume;

    /* UI */
    uint64_t osd_until;
    int show_stats;
    double seek_target;
    uint64_t seek_commit_at;
    int redraw;

    /* Stats */
    float decode_ms, decode_max_ms;
    uint32_t late_frames, skips;
    uint64_t stats_at;
    uint32_t stats_bytes;
    float net_kbps;
#ifdef LP_QA
    int qa_stage;
    double qa_mark;
    uint32_t qa_pictures;
#endif
#ifdef LP_BENCH
    uint64_t bench_decode_us;
    uint32_t bench_frames;
    uint64_t bench_log_at;
#endif
} P;

/* ---- Rings ---- */

static uint32_t ring_free(const ring_t *r) {
    return r->size - (r->head - r->tail);
}

static void ring_put(ring_t *r, const void *src, uint32_t n) {
    uint32_t pos = r->head & (r->size - 1);
    uint32_t first = n < r->size - pos ? n : r->size - pos;
    memcpy(r->data + pos, src, first);
    memcpy(r->data, (const uint8_t *)src + first, n - first);
    __sync_synchronize();
    r->head += n;
}

static void ring_copy(const ring_t *r, uint32_t at, void *dst, uint32_t n) {
    uint32_t pos = at & (r->size - 1);
    uint32_t first = n < r->size - pos ? n : r->size - pos;
    memcpy(dst, r->data + pos, first);
    memcpy((uint8_t *)dst + first, r->data, n - first);
}

/* ---- Stream thread: HTTP and AVI ---- */

static int s_fill(stream_t *s) {
    if(s->buf_pos < s->buf_len) return 1;
    int n = http_read(&s->http, s->buf, sizeof(s->buf), 20000, &s->abort);
    if(n <= 0) return n == 0 ? 0 : -1;
    s->buf_pos = 0;
    s->buf_len = n;
    s->bytes_in += n;
    return 1;
}

/* Copy n bytes of the response to dst (or drop them if dst is NULL). */
static int s_read(stream_t *s, void *dst, uint32_t n) {
    uint8_t *d = dst;
    while(n) {
        if(s_fill(s) <= 0) return -1;
        uint32_t take = s->buf_len - s->buf_pos;
        if(take > n) take = n;
        if(d) {
            memcpy(d, s->buf + s->buf_pos, take);
            d += take;
        }
        s->buf_pos += take;
        n -= take;
    }
    return 0;
}

static int wait_space(stream_t *s, ring_t *r, uint32_t n) {
    while(ring_free(r) < n) {
        if(s->abort) return -1;
        thd_sleep(5);
    }
    return 0;
}

/* Move n bytes of the response into a ring as they arrive. */
static int s_read_to_ring(stream_t *s, ring_t *r, uint32_t n) {
    while(n) {
        if(s_fill(s) <= 0) return -1;
        uint32_t take = s->buf_len - s->buf_pos;
        if(take > n) take = n;
        if(take > r->size / 4) take = r->size / 4;
        if(wait_space(s, r, take) < 0) return -1;
        ring_put(r, s->buf + s->buf_pos, take);
        s->buf_pos += take;
        n -= take;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

static void *stream_thread(void *arg) {
    stream_t *s = arg;
    char why[64] = "";
    if(http_open(&s->http, s->url, "GET", NULL, NULL, 0, 15000) < 0) {
        snprintf(s->error, sizeof(s->error), "Could not reach the server");
        s->state = STREAM_FAILED;
        return NULL;
    }
    if(s->http.status != 200) {
        snprintf(s->error, sizeof(s->error), "The server said HTTP %d", s->http.status);
        s->state = STREAM_FAILED;
        http_close(&s->http);
        return NULL;
    }
    s->state = STREAM_RUNNING;

    uint8_t hdr[64];
    char type[5] = "";
    int streams = 0;
    if(s_read(s, hdr, 12) < 0 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "AVI ", 4)) {
        snprintf(why, sizeof(why), "Not a Dreamcast stream");
        goto done;
    }
    for(;;) {
        if(s->abort) break;
        if(s_read(s, hdr, 8) < 0) break;   /* end of the response */
        uint32_t size = le32(hdr + 4);
        uint32_t padded = size + (size & 1);

        if(!memcmp(hdr, "LIST", 4) || !memcmp(hdr, "RIFF", 4)) {
            /* Descend into every list; a pipe can't fill in their sizes. */
            if(s_read(s, hdr, 4) < 0) break;
            continue;
        }
        if(!memcmp(hdr, "strh", 4) && size >= 32) {
            if(s_read(s, hdr, 32) < 0 || s_read(s, NULL, padded - 32) < 0) break;
            memcpy(type, hdr, 4);
            if(!memcmp(type, "vids", 4) && s->video_index < 0) {
                s->video_index = streams;
                uint32_t scale = le32(hdr + 20), rate = le32(hdr + 24);
                s->fps = scale ? (double)rate / scale : 0;
            }
            else if(!memcmp(type, "auds", 4) && s->audio_index < 0) {
                s->audio_index = streams;
            }
            streams++;
            continue;
        }
        if(!memcmp(hdr, "strf", 4) && size >= 16) {
            uint32_t want = size < sizeof(hdr) ? size : sizeof(hdr);
            if(s_read(s, hdr, want) < 0 || s_read(s, NULL, padded - want) < 0) break;
            if(!memcmp(type, "vids", 4) && s->video_index == streams - 1) {
                s->width = (int)le32(hdr + 4);
                s->height = abs((int32_t)le32(hdr + 8));
            }
            else if(!memcmp(type, "auds", 4) && s->audio_index == streams - 1) {
                int tag = le16(hdr), bits = le16(hdr + 14);
                s->audio_channels = le16(hdr + 2);
                s->audio_rate = (int)le32(hdr + 4);
                if(tag == 1 && bits == 16 && s->audio_channels <= 2) s->audio_format = AUDIO_PCM16;
                else s->audio_index = -1;   /* not one we can play */
            }
            continue;
        }

        /* Stream chunks are named "NNdc", "NNdb", "NNwb". */
        int index = -1;
        if(hdr[0] >= '0' && hdr[0] <= '9' && hdr[1] >= '0' && hdr[1] <= '9') index = (hdr[0] - '0') * 10 + hdr[1] - '0';
        if(index >= 0 && !s->header_ready) {
            if(s->video_index < 0 && s->audio_index < 0) {
                snprintf(why, sizeof(why), "No video or audio we can play");
                goto done;
            }
            __sync_synchronize();
            s->header_ready = 1;
        }
        if(index >= 0 && index == s->video_index && hdr[2] == 'd') {
            uint32_t entry = 4 + ((size + 3) & ~3u);
            if(entry > s->vq.size / 2) {
                if(s_read(s, NULL, padded) < 0) break;
                continue;
            }
            uint8_t len[4] = {size, size >> 8, size >> 16, size >> 24};
            if(wait_space(s, &s->vq, entry) < 0) break;
            ring_put(&s->vq, len, 4);
            if(s_read_to_ring(s, &s->vq, size) < 0) break;
            static const uint8_t zero[4];
            if(entry - 4 - size) ring_put(&s->vq, zero, entry - 4 - size);
            if(size & 1) s_read(s, NULL, 1);
            __sync_synchronize();
            s->frames_in++;
        }
        else if(index >= 0 && index == s->audio_index && hdr[2] == 'w') {
            if(s_read_to_ring(s, &s->aq, size) < 0) break;
            if(size & 1) s_read(s, NULL, 1);
        }
        else if(s_read(s, NULL, padded) < 0) {
            break;
        }
    }

done:
    if(why[0]) {
        snprintf(s->error, sizeof(s->error), "%s", why);
        s->state = STREAM_FAILED;
    }
    else {
        if(!s->header_ready && !s->abort) {
            snprintf(s->error, sizeof(s->error), "The stream ended before it began");
            s->state = STREAM_FAILED;
        }
        else {
            s->state = STREAM_ENDED;
        }
    }
    http_close(&s->http);
    return NULL;
}

/* ---- Clock and audio ---- */

static double clock_now(void) {
    if(!P.running) return P.clock_base;
    return P.clock_base + (timer_us_gettime64() - P.anchor_us) * 1e-6;
}

static uint32_t audio_bytes_at(double t) {
    double secs = t - P.start_time;
    if(secs < 0) secs = 0;
    uint32_t frame = P.s->audio_channels * 2;
    return (uint32_t)(secs * P.s->audio_rate) * frame;
}

static void *audio_callback(snd_stream_hnd_t hnd, int request, int *got) {
    (void)hnd;
    stream_t *s = P.s;
    uint32_t avail = s->aq.head - P.a_pos;
    uint32_t n = (uint32_t)request < avail ? (uint32_t)request : avail;
    n &= ~3u;
    if(n) ring_copy(&s->aq, P.a_pos, P.snd_staging, n);
    if(n < (uint32_t)request) {
        memset(P.snd_staging + n, 0, request - n);
        if(s->state == STREAM_RUNNING || s->state == STREAM_CONNECTING) P.audio_starved = 1;
    }
    P.a_pos += n;
    *got = request;
    return P.snd_staging;
}

static void clock_start(void) {
    if(P.running) return;
    P.audio_starved = 0;
    if(P.s->audio_index >= 0) {
        P.a_pos = audio_bytes_at(P.clock_base);
        if(P.a_pos < P.s->aq.tail) P.a_pos = P.s->aq.tail;
        if(P.a_pos > P.s->aq.head) P.a_pos = P.s->aq.head & ~3u;
        P.a_start = P.a_pos;
        snd_stream_start(P.snd, P.s->audio_rate, P.s->audio_channels == 2);
        snd_stream_volume(P.snd, P.volume);
    }
    P.anchor_us = timer_us_gettime64();
    P.running = 1;
    P.started = 1;
}

static void clock_stop(void) {
    if(!P.running) return;
    P.clock_base = clock_now();
    P.running = 0;
    if(P.s->audio_index >= 0) snd_stream_stop(P.snd);
}

/* ---- Video ---- */

static uint32_t next_pow2(uint32_t n) {
    uint32_t p = 16;
    while(p < n) p <<= 1;
    return p;
}

static void video_free(void) {
    if(P.video) plm_video_destroy(P.video);   /* also destroys vbuf */
    P.video = NULL;
    P.vbuf = NULL;
}

static void video_reset_decoder(void) {
    video_free();
    P.vbuf = plm_buffer_create_with_capacity(256 * 1024);
    P.video = plm_video_create_with_buffer(P.vbuf, 1);
    plm_video_set_no_delay(P.video, 1);
    P.fed = P.decoded;
    P.vbuf_ended = 0;
}

static int textures_setup(int w, int h) {
    int tw = next_pow2(w), th = next_pow2(h);
    if(tw > 1024 || th > 1024) return -1;
    if(tw != P.tex_w || th != P.tex_h) {
        for(int i = 0; i < 3; i++) {
            if(P.tex[i]) pvr_mem_free(P.tex[i]);
            P.tex[i] = pvr_mem_malloc(tw * th * 2);
            if(!P.tex[i]) return -1;
            sq_set(P.tex[i], 0, tw * th * 2);
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED,
                             tw, th, P.tex[i], PVR_FILTER_BILINEAR);
            pvr_poly_compile(&P.hdr[i], &cxt);
        }
        P.tex_w = tw;
        P.tex_h = th;
    }
    P.vid_w = w;
    P.vid_h = h;
    return 0;
}

static void textures_free(void) {
    for(int i = 0; i < 3; i++) {
        if(P.tex[i]) pvr_mem_free(P.tex[i]);
        P.tex[i] = NULL;
    }
    P.tex_w = P.tex_h = 0;
}

/* Send a decoded picture (already in YUV converter macroblock order) to
   texture i. Each macroblock row is padded out to the texture's width. */
static void upload(const plm_frame_t *frame, int i) {
    PVR_SET(PVR_YUV_ADDR, ((uint32_t)P.tex[i]) & 0xffffff);
    PVR_SET(PVR_YUV_CFG, (((P.tex_h >> 4) - 1) << 8) | ((P.tex_w >> 4) - 1));
    PVR_GET(PVR_YUV_CFG);
    int mb_w = frame->y.width >> 4, mb_h = frame->y.height >> 4;
    int pad = (P.tex_w >> 4) - mb_w;
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

static int picture_type(const uint8_t *p, uint32_t n) {
    for(uint32_t i = 0; i + 5 < n; i++) {
        if(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0) return (p[i + 5] >> 3) & 7;
    }
    return 0;
}

static uint32_t frames_queued(void) {
    return P.s->frames_in - P.frames_out;
}

/* Take the next picture off the queue. Returns its size; data goes into the
   decoder's buffer unless drop is set, and its first bytes into head. */
static uint32_t pop_picture(int drop, uint8_t *head, uint32_t head_size) {
    stream_t *s = P.s;
    uint8_t len[4];
    ring_copy(&s->vq, s->vq.tail, len, 4);
    uint32_t size = le32(len);
    uint32_t at = s->vq.tail + 4;
    if(head) ring_copy(&s->vq, at, head, size < head_size ? size : head_size);
    if(!drop && size) {
        uint32_t pos = at & (s->vq.size - 1);
        uint32_t first = size < s->vq.size - pos ? size : s->vq.size - pos;
        plm_buffer_write(P.vbuf, s->vq.data + pos, first);
        if(size > first) plm_buffer_write(P.vbuf, s->vq.data, size - first);
    }
    __sync_synchronize();
    s->vq.tail += 4 + ((size + 3) & ~3u);
    P.frames_out++;
    return size;
}

static double picture_time(uint32_t index) {
    return P.start_time + index / P.s->fps;
}

/* Drop queued pictures up to an intra frame that is not yet late. Returns
   1 once the decoder restarts there. */
static int skip_to_intra(double now) {
    while(frames_queued()) {
        uint8_t head[256];
        uint32_t peek;
        ring_copy(&P.s->vq, P.s->vq.tail, head, 4);
        peek = le32(head);
        ring_copy(&P.s->vq, P.s->vq.tail + 4, head, peek < sizeof(head) ? peek : sizeof(head));
        uint32_t index = P.frames_out;
        if(peek && picture_type(head, peek < sizeof(head) ? peek : sizeof(head)) == 1 &&
           picture_time(index) >= now - 0.1) {
            P.decoded = index;
            video_reset_decoder();
            P.skips++;
            return 1;
        }
        pop_picture(1, NULL, 0);
    }
    return 0;
}

/* Copy pictures into the decoder's buffer so it holds the one to decode
   and the start of the next (pl_mpeg needs the next start code). */
static void feed(void) {
    while(P.fed < P.decoded + 2 && frames_queued()) {
        uint32_t size = pop_picture(0, NULL, 0);
        P.fed_sizes[P.fed & 15] = size != 0;
        P.fed++;
    }
    if(!frames_queued() && P.s->state == STREAM_ENDED && !P.vbuf_ended) {
        plm_buffer_signal_end(P.vbuf);
        P.vbuf_ended = 1;
    }
}

static void video_step(double now) {
    if(!P.video) return;
    if(P.pending >= 0) {
        if(now + 0.004 >= P.pending_time) {
            P.front = P.pending;
            P.pending = -1;
            P.redraw = 1;
        }
        return;
    }
    double due = picture_time(P.decoded);
    if(P.running && due < now - SKIP_BEHIND) P.skipping = 1;
    if(P.skipping) {
        if(!skip_to_intra(now)) return;
        P.skipping = 0;
        due = picture_time(P.decoded);
    }
    if(due > now + DECODE_LEAD && P.started) return;

    feed();
    if(P.fed <= P.decoded) return;   /* nothing queued yet */
    if(!P.fed_sizes[P.decoded & 15]) {   /* an empty AVI chunk repeats the picture */
        P.decoded++;
        return;
    }
    if(P.fed < P.decoded + 2 && !P.vbuf_ended) return;

    uint64_t t0 = timer_us_gettime64();
    plm_frame_t *frame = plm_video_decode(P.video);
    uint64_t t1 = timer_us_gettime64();
    if(!frame) {
        if(P.vbuf_ended) P.decoded = P.fed;
        return;
    }
    float ms = (t1 - t0) / 1000.0f;
    P.decode_ms = P.decode_ms * 0.9f + ms * 0.1f;
    if(ms > P.decode_max_ms) P.decode_max_ms = ms;
#ifdef LP_BENCH
    P.bench_decode_us += t1 - t0;
    P.bench_frames++;
#endif
    if(P.running && due < now - 1.0 / P.s->fps) P.late_frames++;

    int slot = 0;
    while(slot == P.front || slot == P.pending) slot++;
    if(frame->y.width >> 4 <= P.tex_w >> 4 && frame->y.height <= P.tex_h) upload(frame, slot);
    P.pending = slot;
    P.pending_time = due;
    P.have_picture = 1;
    P.decoded++;
}

/* ---- Sessions: one stream request each ---- */

static void session_close(void) {
    stream_t *s = P.s;
    if(!s) return;
    clock_stop();
    s->abort = 1;
    if(s->thread) thd_join(s->thread, NULL);
    video_free();
    free(s);
    P.s = NULL;
}

static int session_open(double t) {
    stream_t *s = calloc(1, sizeof(*s));
    if(!s) return -1;
    const char *url = P.entry->url;
    snprintf(s->url, sizeof(s->url), "%s%ct=%.2f&a=pcm", url, strchr(url, '?') ? '&' : '?', t);
    s->video_index = s->audio_index = -1;
    s->vq.data = P.vq_mem;
    s->vq.size = VQ_BYTES;
    s->aq.data = P.aq_mem;
    s->aq.size = AQ_BYTES;
    P.s = s;

    P.start_time = t;
    P.clock_base = t;
    P.running = 0;
    P.started = 0;
    P.buffering = 1;
    P.frames_out = P.decoded = P.fed = 0;
    P.skipping = 0;
    P.pending = -1;
    P.front = -1;
    P.have_picture = 0;
    P.a_pos = 0;
    P.redraw = 1;
    P.decode_max_ms = 0;

    s->thread = thd_create(0, stream_thread, s);
    return s->thread ? 0 : -1;
}

/* Buffered seconds of video and audio ahead of the clock. */
static double video_ahead(double now) {
    if(!P.s->header_ready || P.s->video_index < 0 || P.s->fps <= 0) return 99;
    return picture_time(P.s->frames_in) - now;
}

static double audio_ahead(double now) {
    if(!P.s->header_ready || P.s->audio_index < 0) return 99;
    uint32_t at = audio_bytes_at(now);
    if(P.s->aq.head <= at) return 0;
    return (double)(P.s->aq.head - at) / P.bytes_per_sec;
}

static int buffers_ready(double now) {
    stream_t *s = P.s;
    if(s->state == STREAM_ENDED) return 1;
    int vfull = ring_free(&s->vq) < s->vq.size / 4;
    int afull = ring_free(&s->aq) < s->aq.size / 4;
    if(vfull || afull) return 1;
    return video_ahead(now) >= START_SECS && audio_ahead(now) >= START_SECS;
}

/* Called once the AVI headers are known. */
static int session_prepare(char *error, size_t error_size) {
    stream_t *s = P.s;
    if(s->video_index >= 0) {
        if(s->fps <= 0 || s->width <= 0 || s->height <= 0 || (s->width & 15) || (s->height & 15)) {
            snprintf(error, error_size, "Unsupported picture size %dx%d", s->width, s->height);
            return -1;
        }
        if(textures_setup(s->width, s->height) < 0) {
            snprintf(error, error_size, "Picture too large: %dx%d", s->width, s->height);
            return -1;
        }
        video_reset_decoder();
    }
    if(s->audio_index >= 0) {
        P.bytes_per_sec = s->audio_rate * s->audio_channels * 2;
    }
    return 0;
}

/* ---- Drawing ---- */

static void format_time(char *out, size_t size, double secs) {
    int t = secs < 0 ? 0 : (int)secs;
    if(t >= 3600) snprintf(out, size, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, size, "%d:%02d", t / 60, t % 60);
}

static void draw(double now) {
    stream_t *s = P.s;
    int video = s && s->header_ready && s->video_index >= 0;
    char text[96], a[16], b[16];

    gfx_begin();
    if(video && P.front >= 0) {
        float scale = fminf((float)SCREEN_W / P.vid_w, (float)SCREEN_H / P.vid_h);
        float w = P.vid_w * scale, h = P.vid_h * scale;
        /* Half a texel in from each edge: bilinear filtering would otherwise
           blend in the texture's padding, which is green in YUV. */
        gfx_textured(&P.hdr[P.front], (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h,
                     0.5f / P.tex_w, 0.5f / P.tex_h, (P.vid_w - 0.5f) / P.tex_w, (P.vid_h - 0.5f) / P.tex_h);
    }
    else if(!video) {
        /* Audio only: a simple now-playing card. */
        gfx_rect(80, 150, 480, 150, COL_PANEL);
        gfx_rect(80, 150, 6, 150, COL_LEMON);
    }

    if(!video) {
        gfx_text(104, 166, COL_LEMON, 1, "NOW PLAYING");
        gfx_text_clip(104, 200, COL_TEXT, 1, P.entry->title, 440);
    }

    uint64_t ms = timer_ms_gettime64();
    int osd = !video || P.paused || P.buffering || ms < P.osd_until || P.seek_commit_at;
    if(osd) {
        double shown = P.seek_commit_at ? P.seek_target : now;
        double duration = P.entry->duration;
        gfx_rect_alpha(0, 392, SCREEN_W, 88, 0xc0000000);
        gfx_text_clip(24, 400, COL_TEXT, 1, P.entry->title, 592);
        format_time(a, sizeof(a), shown);
        if(duration > 0) {
            format_time(b, sizeof(b), duration);
            snprintf(text, sizeof(text), "%s / %s", a, b);
        }
        else {
            snprintf(text, sizeof(text), "%s", a);
        }
        gfx_text(24, 440, P.seek_commit_at ? COL_LEMON : COL_MUTED, 1, text);
        float bar_x = 24 + gfx_text_width(text, 1) + 20, bar_w = SCREEN_W - 24 - bar_x;
        if(duration > 0 && bar_w > 40) {
            float f = shown / duration;
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            gfx_rect_alpha(bar_x, 450, bar_w, 6, 0xff4a4535);
            gfx_rect_alpha(bar_x, 450, bar_w * f, 6, COL_LEMON);
        }
        const char *state = P.paused ? "PAUSED" : NULL;
        if(P.buffering && s) {
            if(s->state == STREAM_CONNECTING) state = "CONNECTING";
            else {
                double v = video_ahead(P.clock_base), au = audio_ahead(P.clock_base);
                double have = v < au ? v : au;
                int pct = (int)(100 * have / START_SECS);
                snprintf(text, sizeof(text), "BUFFERING %d%%", pct < 0 ? 0 : pct > 99 ? 99 : pct);
                state = text;
            }
        }
        if(state) {
            float w = gfx_text_width(state, 1);
            gfx_rect_alpha(SCREEN_W - w - 40, 24, w + 24, 36, 0xc0000000);
            gfx_text(SCREEN_W - w - 28, 30, COL_LEMON, 1, state);
        }
    }
    if(P.show_stats && s) {
        snprintf(text, sizeof(text), "%dx%d %.2ffps dec %.1f/%.1fms late %u skip %u",
                 P.vid_w, P.vid_h, s->fps, P.decode_ms, P.decode_max_ms, (unsigned)P.late_frames, (unsigned)P.skips);
        gfx_rect_alpha(0, 0, SCREEN_W, 56, 0xb0000000);
        gfx_text(8, 4, COL_TEXT, 0.75f, text);
        snprintf(text, sizeof(text), "buffer v %.1fs a %.1fs  net %.0f KB/s  vol %d",
                 video_ahead(now), audio_ahead(now), P.net_kbps, P.volume);
        gfx_text(8, 26, COL_TEXT, 0.75f, text);
    }
    gfx_end();
    P.redraw = 0;
}

/* ---- Main loop ---- */

static void seek_by(double delta) {
    double now = clock_now();
    if(!P.seek_commit_at) P.seek_target = now;
    P.seek_target += delta;
    if(P.seek_target < 0) P.seek_target = 0;
    if(P.entry->duration > 0 && P.seek_target > P.entry->duration - 5) P.seek_target = P.entry->duration - 5;
    if(P.seek_target < 0) P.seek_target = 0;
    P.seek_commit_at = timer_ms_gettime64() + SEEK_COMMIT_MS;
    P.redraw = 1;
}

play_result_t player_play(const entry_t *entry, double start_seconds, char *error, size_t error_size) {
    play_result_t result = PLAY_STOPPED;
    memset(&P, 0, sizeof(P));
    P.entry = entry;
    P.front = P.pending = -1;
    P.volume = 240;
    P.snd = SND_STREAM_INVALID;
    error[0] = '\0';

    P.vq_mem = memalign(32, VQ_BYTES);
    P.aq_mem = memalign(32, AQ_BYTES);
    P.snd_staging = memalign(32, SND_BUF_BYTES * 2);
    if(!P.vq_mem || !P.aq_mem || !P.snd_staging) {
        snprintf(error, error_size, "Out of memory");
        result = PLAY_FAILED;
        goto out;
    }
    P.snd = snd_stream_alloc(audio_callback, SND_BUF_BYTES);
    if(P.snd == SND_STREAM_INVALID) {
        snprintf(error, error_size, "No sound channel");
        result = PLAY_FAILED;
        goto out;
    }
    pvr_set_bg_color(0, 0, 0);

    if(session_open(start_seconds) < 0) {
        snprintf(error, error_size, "Could not start the stream");
        result = PLAY_FAILED;
        goto out;
    }
    int prepared = 0;
    P.osd_until = timer_ms_gettime64() + OSD_MS;
    P.stats_at = timer_ms_gettime64();
#ifdef LP_BENCH
    P.show_stats = 1;
    P.bench_log_at = timer_ms_gettime64() + 10000;
#endif

    for(;;) {
        input_t in;
        input_poll(&in);
        uint64_t ms = timer_ms_gettime64();
        stream_t *s = P.s;

        if(s->state == STREAM_FAILED) {
            snprintf(error, error_size, "%s", s->error);
            result = PLAY_FAILED;
            break;
        }
        if(!prepared && s->header_ready) {
            if(session_prepare(error, error_size) < 0) {
                result = PLAY_FAILED;
                break;
            }
            prepared = 1;
        }

        /* Controls */
        if(in.pressed & CONT_B) {
            result = PLAY_STOPPED;
            break;
        }
        if(in.pressed & (CONT_A | CONT_START)) {
            P.paused = !P.paused;
            if(P.paused) clock_stop();
            P.osd_until = ms + OSD_MS;
            P.redraw = 1;
        }
        if(in.pressed & CONT_DPAD_LEFT) seek_by(-30);
        if(in.pressed & CONT_DPAD_RIGHT) seek_by(30);
        if(in.pressed & BTN_LTRIG) seek_by(-300);
        if(in.pressed & BTN_RTRIG) seek_by(300);
        if(in.pressed & CONT_DPAD_UP) {
            P.volume = P.volume + 16 > 255 ? 255 : P.volume + 16;
            if(P.running) snd_stream_volume(P.snd, P.volume);
            P.osd_until = ms + OSD_MS;
            P.redraw = 1;
        }
        if(in.pressed & CONT_DPAD_DOWN) {
            P.volume = P.volume < 16 ? 0 : P.volume - 16;
            if(P.running) snd_stream_volume(P.snd, P.volume);
            P.osd_until = ms + OSD_MS;
            P.redraw = 1;
        }
        if(in.pressed & CONT_Y) {
            P.osd_until = ms < P.osd_until ? 0 : ms + OSD_MS;
            P.redraw = 1;
        }
        if(in.pressed & CONT_X) {
            P.show_stats = !P.show_stats;
            P.redraw = 1;
        }
        if(P.seek_commit_at && ms >= P.seek_commit_at) {
            double target = P.seek_target;
            P.seek_commit_at = 0;
            session_close();
            if(session_open(target) < 0) {
                snprintf(error, error_size, "Could not restart the stream");
                result = PLAY_FAILED;
                break;
            }
            prepared = 0;
            P.osd_until = ms + OSD_MS;
            continue;
        }

        double now = clock_now();

        /* Playback state */
        if(P.running && P.audio_starved) {
            clock_stop();
            P.buffering = 1;
            P.redraw = 1;
        }
        if(prepared && P.running && s->video_index >= 0 && s->state != STREAM_ENDED && !frames_queued() &&
           picture_time(P.s->frames_in) < now - 0.1) {
            clock_stop();   /* video ran dry */
            P.buffering = 1;
            P.redraw = 1;
        }
        if(prepared && P.buffering && buffers_ready(P.clock_base)) {
            P.buffering = 0;
            P.redraw = 1;
        }
        if(prepared && !P.buffering && !P.paused && !P.running && !P.seek_commit_at) {
            /* Show the first picture before the clock starts. */
            if(s->video_index < 0 || P.have_picture) clock_start();
        }
        now = clock_now();

        if(prepared && s->audio_index >= 0) {
            if(P.running) snd_stream_poll(P.snd);
            uint32_t keep = audio_bytes_at(now - 0.1);
            if(keep > P.a_pos) keep = P.a_pos;
            if(keep > s->aq.tail && keep <= s->aq.head) s->aq.tail = keep;
            if(!P.running) {
                /* While stopped, keep everything from the clock on. */
                uint32_t at = audio_bytes_at(P.clock_base);
                if(at > s->aq.tail && at <= s->aq.head) s->aq.tail = at;
            }
        }
        if(prepared && s->video_index >= 0 && !P.paused) video_step(now);

        /* The end */
        if(prepared && s->state == STREAM_ENDED && !frames_queued() && P.pending < 0 &&
           (s->audio_index < 0 || P.a_pos >= s->aq.head) && !P.seek_commit_at) {
            /* Let the AICA play out what it already holds, and the last
               picture stay up for its frame. */
            double end = P.start_time;
            if(s->audio_index >= 0) end = P.start_time + (double)s->aq.head / P.bytes_per_sec;
            if(s->video_index >= 0 && picture_time(P.decoded) > end) end = picture_time(P.decoded);
            if((s->video_index < 0 || P.fed <= P.decoded || P.vbuf_ended) && (now >= end || !P.running)) {
#ifdef LP_QA
                printf("LP QA: finished at %.2f, %u pictures, late %u, skips %u\n", now, (unsigned)P.decoded,
                       (unsigned)P.late_frames, (unsigned)P.skips);
#endif
                result = PLAY_FINISHED;
                break;
            }
        }

#if defined(LP_QA) && !defined(LP_QA_END)
        /* Scripted check: play, pause two seconds, play, seek forward a
           minute, play, stop. */
        if(P.running && P.qa_stage == 0 && now > P.start_time + 6) {
            P.paused = 1;
            clock_stop();
            P.qa_mark = ms;
            P.qa_stage = 1;
            printf("LP QA: paused at %.2f\n", now);
        }
        else if(P.qa_stage == 1 && ms > P.qa_mark + 2000) {
            P.paused = 0;
            P.qa_stage = 2;
            printf("LP QA: resumed at %.2f\n", clock_now());
        }
        else if(P.running && P.qa_stage == 2 && now > P.start_time + 12) {
            printf("LP QA: %.1fs played, %u pictures, decode %.1f ms (max %.1f), late %u, skips %u\n",
                   now - P.start_time, (unsigned)P.decoded, P.decode_ms, P.decode_max_ms,
                   (unsigned)P.late_frames, (unsigned)P.skips);
            P.qa_pictures += P.decoded;
            P.qa_mark = now + 60;
            P.qa_stage = 3;
            seek_by(60);
        }
        else if(P.running && P.qa_stage == 3 && now > P.qa_mark + 10) {
            P.qa_pictures += P.decoded;
            printf("LP QA: after seek %.1fs from %.1f, %u pictures, late %u, skips %u\n",
                   now - P.start_time, P.start_time, (unsigned)P.decoded, (unsigned)P.late_frames, (unsigned)P.skips);
            printf("LP QA: %s\n", P.qa_pictures > 400 && P.start_time >= P.qa_mark - 1 && !P.skips ? "PASS" : "FAIL");
            result = PLAY_STOPPED;
            break;
        }
#endif

        /* Stats once a second */
        if(ms - P.stats_at >= 1000) {
            uint32_t bytes = s->bytes_in;
            P.net_kbps = (bytes - P.stats_bytes) / 1024.0f * 1000.0f / (ms - P.stats_at);
            P.stats_bytes = bytes;
            P.stats_at = ms;
            if(P.show_stats || P.buffering) P.redraw = 1;
            if(ms < P.osd_until + 1000) P.redraw = 1;
        }
#ifdef LP_BENCH
        if(ms >= P.bench_log_at) {
            printf("LP BENCH %dx%d %.2ffps decode avg %.2f ms max %.2f ms late %u skips %u net %.0f KB/s\n",
                   P.vid_w, P.vid_h, s->fps,
                   P.bench_frames ? P.bench_decode_us / 1000.0 / P.bench_frames : 0.0,
                   P.decode_max_ms, (unsigned)P.late_frames, (unsigned)P.skips, P.net_kbps);
            P.bench_log_at = ms + 10000;
        }
#endif

        if((P.redraw || !P.have_picture) && gfx_ready()) draw(now);
        else if(P.pending < 0 || !P.running) thd_sleep(2);
        else thd_pass();
    }

out:
    session_close();
    if(P.snd != SND_STREAM_INVALID) snd_stream_destroy(P.snd);
    textures_free();
    free(P.vq_mem);
    free(P.aq_mem);
    free(P.snd_staging);
    pvr_set_bg_color(0.08f, 0.075f, 0.047f);
    return result;
}
