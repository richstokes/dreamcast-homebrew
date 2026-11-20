/* Film builds (film.h). */
#include "film.h"
#ifdef KL_FILM
#include "city.h"
#include "kaiju.h"
#include "life.h"
#include "fx.h"
#include "score.h"
#include "engine/qa.h"

enum { SH_GAME, SH_STATIC, SH_ORBIT, SH_HEAD };

typedef struct {
    const char *name;
    float pre, dur;             /* seconds run before "go", then filmed */
    V3 start;                   /* the kaiju */
    float heading;              /* degrees */
    int shot;
    V3 eye;                     /* STATIC: the camera. ORBIT: radius, height, turn rate. HEAD: right, up, forward of the head */
    V3 look;                    /* STATIC/ORBIT: offset from the kaiju's chest, or the point itself (fixed) */
    bool fixed;
    float fov, orbit0;
    void (*setup)(void);
    void (*ctl)(float t, Ctl *c);
    float slow_at, slow_len, slow;   /* a stretch of the scene slowed down */
    int watch;                  /* 1: slow down while a tower is about to land */
} Scene;

static inline float R(int i) { return city_road(i); }
static float rad(float deg) { return deg * (ENG_PI / 180.0f); }
static void go(Ctl *c, float deg, float k) {
    c->mx = sinf(rad(deg)) * k;
    c->mz = cosf(rad(deg)) * k;
}
/* A press every period seconds, from t0. */
static bool every(float t, float t0, float period) {
    static float last = -1;
    if(t < t0) {
        last = -1;
        return false;
    }
    int n = (int)((t - t0) / period);
    if((float)n == last) return false;
    last = (float)n;
    return true;
}
static void weaken(float x, float z, float hp) {
    int b = city_at(x, z);
    if(b >= 0 && city_b[b].hp > hp) city_b[b].hp = hp;
}

/* ---- What the kaiju does in each ---- */
static void c_walk(float t, Ctl *c) { (void)t; go(c, kj.heading * (180.0f / ENG_PI), 1); }
static void c_claws(float t, Ctl *c) { c->claw = every(t, 0.3f, 0.2f); }
static void c_timber(float t, Ctl *c) { c->claw = every(t, 0.6f, 0.2f); }
static void c_tanks(float t, Ctl *c) {
    go(c, -135, 1);
    c->jump = t > 0.5f && t < 0.6f;
}
static void c_breath(float t, Ctl *c) {
    /* Hold the breath and swing it across the blocks. */
    c->breath = t > 0.3f;
    go(c, -150.0f + 28.0f * t, 0.2f);
}
static void c_tail(float t, Ctl *c) {
    c->tail = every(t, 0.7f, 1.0f);
    c->roar = false;
}
static void c_flop(float t, Ctl *c) {
    int b = city_nearest(kj.pos, 30);
    if(b >= 0) {
        float dx = city_b[b].x - kj.pos.x, dz = city_b[b].z - kj.pos.z, d = sqrtf(dx * dx + dz * dz) + 0.01f;
        c->mx = dx / d;
        c->mz = dz / d;
        c->jump = d < 52.0f && every(t, 0.2f, 1.3f);
    }
}
static void c_chomp(float t, Ctl *c) {
    if(t < 0.9f) life_film_heli(vadd(kj.pos, v3(0, 33, 30)));
    c->chomp = t > 0.9f && t < 1.0f;
    c->roar = t > 2.0f && t < 2.1f;
}
static void c_tv(float t, Ctl *c) { c->tail = every(t, 0.5f, 0.8f); }
static void c_roar(float t, Ctl *c) { c->roar = t > 0.6f && t < 0.7f; }
static void c_bot(float t, Ctl *c) {
    extern void bot_ctl(Ctl *c, float dt);
    (void)t;
    bot_ctl(c, 1.0f / 60.0f);
}
static void c_rush(float t, Ctl *c) {
    /* Through the low streets at a run, swiping and spinning as it goes. */
    go(c, 45, 1);
    c->claw = every(t, 0.2f, 0.9f);
    c->tail = fmodf(t, 2.4f) > 2.3f;
}

static void s_twins(void) { weaken(-99, -96, 520); }
static void s_mega(void) { weaken(0, 0, 200); }
static void s_tv(void) { weaken(168, -168, 150); }
static void s_jam(void) { life_film_jam(kj.pos, 26); }
static void s_war(void) { score.damage = 9000000; }   /* (what brings the tanks, helicopters and jets) */

static const Scene scenes[] = {
    /* Up the avenue and over the camera in the road. */
    {"arrive", 1.5f, 6.0f, {-42, 0, -330}, 0, SH_STATIC, {-36, 2.2f, -215}, {0, 4, 0}, false, 1.25f, 0, NULL, c_walk, 0, 0, 1, 0},
    /* Chunks out of a twin tower, from close in. */
    {"claws", 0.2f, 7.0f, {-99, 0, -128}, 0, SH_STATIC, {-172, 9, -124}, {-99, 36, -108}, true, 1.15f, 0, s_twins, c_claws, 1.0f, 1.6f, 0.3f, 0},
    {"claws-game", 0.2f, 7.0f, {-99, 0, -128}, 0, SH_GAME, {0}, {0}, false, 0, 0, s_twins, c_claws, 0, 0, 1, 0},
    /* The tallest tower in town, felled across two blocks. */
    {"timber", 0.2f, 9.0f, {0, 0, -40}, 0, SH_STATIC, {42, 120, 330}, {0, 80, 80}, true, 1.1f, 0, s_mega, c_timber, 0, 0, 0.3f, 1},
    {"timber-low", 0.2f, 9.0f, {0, 0, -40}, 0, SH_STATIC, {44, 3, 250}, {0, 90, 60}, true, 1.3f, 0, s_mega, c_timber, 0, 0, 0.3f, 1},
    /* Into the tank farm: three blocks of it go up one after another. */
    {"tanks", 0.2f, 8.0f, {-372, 0, -372}, -135, SH_STATIC, {-290, 46, -290}, {-400, 6, -400}, true, 1.15f, 0, NULL, c_tanks, 1.3f, 1.2f, 0.35f, 0},
    {"tanks-orbit", 0.2f, 8.0f, {-372, 0, -372}, -135, SH_ORBIT, {120, 60, 0.5f}, {-400, 10, -400}, true, 1.0f, 0.8f, NULL, c_tanks, 0, 0, 1, 0},
    /* The breath across the mid-rise blocks, on the game's own camera. */
    {"breath-game", 0.3f, 7.0f, {-210, 0, -126}, -150, SH_GAME, {0}, {0}, false, 0, 0, NULL, c_breath, 0, 0, 1, 0},
    {"breath", 0.3f, 7.0f, {-210, 0, -126}, -150, SH_STATIC, {-296, 16, -150}, {0, 12, 0}, false, 1.2f, 0, NULL, c_breath, 0, 0, 1, 0},
    /* A traffic jam, and a tail. */
    {"tail", 0.6f, 5.0f, {-42, 0, -210}, 0, SH_STATIC, {-30, 5, -150}, {0, 0, 0}, false, 1.25f, 0, s_jam, c_tail, 0.9f, 1.2f, 0.3f, 0},
    {"tail-game", 0.6f, 5.0f, {-42, 0, -210}, 0, SH_GAME, {0}, {0}, false, 0, 0, s_jam, c_tail, 0, 0, 1, 0},
    /* Belly flops through a mid-rise block. */
    {"flop", 0.2f, 8.0f, {-210, 0, -42}, -90, SH_ORBIT, {90, 60, 0.3f}, {0, 0, 0}, false, 1.0f, 3.6f, NULL, c_flop, 0, 0, 1, 0},
    /* The news helicopter, close up. */
    {"chomp", 0.3f, 4.0f, {-42, 0, -294}, 0, SH_HEAD, {7, -6, 50}, {0, -1, 8}, false, 1.0f, 0, NULL, c_chomp, 0.95f, 0.8f, 0.3f, 0},
    /* The TV mast over. */
    {"tv", 0.2f, 8.0f, {168, 0, -190}, 0, SH_STATIC, {126, 150, 70}, {168, 60, -90}, true, 1.1f, 0, s_tv, c_tv, 0, 0, 0.3f, 1},
    /* The game as played: the bot on a tear, HUD and all. */
    {"rampage-game", 2.0f, 16.0f, {-252, 0, -210}, 45, SH_GAME, {0}, {0}, false, 0, 0, NULL, c_rush, 0, 0, 1, 0},
    {"bot-game", 2.0f, 14.0f, {130, 0, -126}, 0, SH_GAME, {0}, {0}, false, 0, 0, NULL, c_bot, 0, 0, 1, 0},
    /* A roar into the lens to end on. */
    /* The army turns up to all of it at once. */
    {"war-game", 9.0f, 14.0f, {-42, 0, -126}, 0, SH_GAME, {0}, {0}, false, 0, 0, s_war, c_bot, 0, 0, 1, 0},
    {"war", 9.0f, 14.0f, {-42, 0, -126}, 0, SH_ORBIT, {95, 44, 0.35f}, {0, 6, 0}, false, 1.1f, 0, s_war, c_bot, 0, 0, 1, 0},
    {"roar", 0.3f, 3.5f, {-42, 0, -60}, 0, SH_STATIC, {-30, 4, -12}, {0, 10, 4}, false, 1.1f, 0, NULL, c_roar, 0, 0, 1, 0},
};
#define SCENES ((int)(sizeof(scenes) / sizeof(scenes[0])))

static int cur = -1;
static float st;            /* seconds into the scene, pre-roll included */
static bool rolling, hud;
static float shot_at;

#ifdef KL_FILM_PICK
static const int pick[] = {KL_FILM_PICK};
#define PICKS ((int)(sizeof(pick) / sizeof(pick[0])))
#else
#define PICKS SCENES
#endif
static int which(int n) {
#ifdef KL_FILM_PICK
    return pick[n];
#else
    return n;
#endif
}

static void begin(int n) {
    cur = n;
    if(n >= PICKS) {
        printf("KL FILM done\n");
#ifdef KL_FILM_SHOTS
        printf("KL AUTOTEST: PASS\n");
#endif
        return;
    }
    const Scene *s = &scenes[which(n)];
    eng_seed(0x2026dcu + (uint32_t)which(n));
    game_world_reset(3);
    game.state = ST_PLAY;
    game.intro = game.outro = 0;
    game.clock = 100.0f;
    kj.pos = s->start;
    kj.heading = rad(s->heading);
    for(int i = 0; i < 2; i++) kj.foot[i].p = vadd(kj.pos, vscale(heading_right(kj.heading), i ? 6.2f : -6.2f));
    kj.energy = 1;
    game_camera_face(kj.heading);
    if(s->setup) s->setup();
    st = 0;
    rolling = false;
    hud = s->shot == SH_GAME;
    shot_at = 0;
}

void film_start(void) {
    begin(0);
}

bool film_hud(void) { return hud; }

static void camera(const Scene *s, float dt) {
    V3 chest = vadd(kj.pos, v3(0, 24 + kj.y * 0.6f, 0)), eye, look;
    Xf yaw = xf_rot_y(kj.heading);
    switch(s->shot) {
        case SH_GAME:
            game_camera(dt, 0);
            return;
        case SH_ORBIT: {
            V3 centre = s->fixed ? s->look : vadd(chest, xf_dir(&yaw, s->look));
            eye = orbit_point(v3(centre.x, 0, centre.z), s->orbit0 + (st - s->pre) * s->eye.z, s->eye.x, s->eye.y);
            look = centre;
            break;
        }
        case SH_HEAD: {
            /* (worked out here: the kaiju only knows where its head is once drawn) */
            V3 head = vadd(kj.pos, xf_dir(&yaw, v3(0, 33, 7)));
            eye = vadd(head, xf_dir(&yaw, s->eye));
            look = vadd(head, xf_dir(&yaw, s->look));
            break;
        }
        case 99:
            break;
        default:
            eye = s->eye;
            look = s->fixed ? s->look : vadd(chest, s->look);
            break;
    }
    float sh = fx_shake_now() * 1.2f;
    if(sh > 0) eye = vadd(eye, v3(frand_s() * sh, frand_s() * sh, frand_s() * sh));
    cam.fov = s->fov;
    cam_look_at(&cam, eye, look);
}

void film_update(float dt) {
    if(cur < 0 || cur >= PICKS) return;
    const Scene *s = &scenes[which(cur)];
    float t = st - s->pre;      /* scene time: 0 at "go" */
    if(!rolling && t >= 0) {
        rolling = true;
        printf("KL FILM go %s\n", s->name);
    }
    /* Slow motion: a set stretch, or while a falling tower nears the ground. */
    float k = 1;
    if(s->slow_len > 0 && t >= s->slow_at && t < s->slow_at + s->slow_len) k = s->slow;
    if(s->watch)
        for(int i = 0; i < city_nb; i++)
            if(city_b[i].state == B_TOPPLE && city_b[i].len > 100 && city_b[i].ang > 1.05f) k = s->slow;
    float step = dt * k;
    Ctl c;
    memset(&c, 0, sizeof(c));
    if(s->ctl) s->ctl(t, &c);
    kj.energy = 1;
    game.time += step;
    kaiju_update(&c, step);
    city_update(step);
    life_update(step);
    fx_update(step);
    score_update(step);
    camera(s, dt);
    st += dt;
#ifdef KL_FILM_SHOTS
    if(rolling && t >= shot_at) {
        qa_shot("%s-%02d", s->name, (int)(shot_at * 2.0f + 0.5f));
        shot_at += 0.5f;
    }
#endif
    if(t >= s->dur) {
        printf("KL FILM cut %s\n", s->name);
        begin(cur + 1);
    }
}
#endif
