/* Ridgeline film builds (film.h). */
#include "film.h"
#ifdef RL_FILM
#include "truck.h"
#include "ai.h"
#include "camera.h"
#include "audio.h"
#include "fx.h"
#include "terrain.h"
#include "tod.h"
#include "world.h"
#include "deer.h"
#include "birds.h"
#include "engine/frame.h"
#include "engine/qa.h"
/* (GCC 15's SH-4 back end dies in film_update at -O2: "in gen_reg_rtx". Nothing here needs the speed.) */
#pragma GCC optimize("O1")
#include "damage.h"

/* Camera shots. Their numbers, a[]:
   SH_HELI    radius, height, turn rate (rad/s, signed), start angle from
              the truck's heading
   SH_POST    a tripod by the trail: metres up the route from where the
              truck is placed, metres to the side, height
   SH_STATIC  eye x, height above the ground, z; look x, height, z (all 0:
              follow the truck); dolly to x, height, z over the shot
   SH_LEAD    ahead of the truck looking back: metres ahead, to the side,
              height
   SH_SIDE    beside the truck, keeping pace: metres to the side (- left),
              height, metres ahead
   SH_ORBIT   round a place: its x, z, the height looked at; radius, eye
              height (both heights above the ground at the place), turn
              rate, start bearing
   Heights above the ground are above the water where there is water. */
enum { SH_CHASE, SH_FAR, SH_BUMPER, SH_HELI, SH_POST, SH_STATIC, SH_LEAD, SH_SIDE, SH_ORBIT, SH_GAG };

/* Stunts: a scene's gag has the trucks do what no driver would (gag_begin,
   gag_update below), and SH_GAG is a camera of its own for one. */
enum { G_NONE, G_RAIN, G_TUMBLE, G_LEMMINGS, G_HORNS, G_ROCKET, G_MOON, G_LOWG, G_DONUTS, G_LANDING, G_BREAK, G_BALLOON, G_FLIP,
       G_HOT,  /* its route trucks take their trail at the scene's path speed, far faster than a driver with any
                  sense; one with a delay under zero comes down the trail the other way */
       G_SLOPE, /* truck 0 (PARKED: x, z) is taken to the steepest long slope within 130 m of there and driven
                   across it at the scene's path speed: a sidehill no driver should try at that pace */
       G_TREE  /* truck 0 (PARKED: x, z, heading) is put 45 m short of the tree nearest the point 45 m ahead of
                  it and drives at it; the camera (SH_GAG) stands by the tree: a[0] m to the side, a[1] up, a[2]
                  back toward the truck */ };
float film_g = 1.0f;

/* A truck's route, or one of these. */
#define ON_PATH (-1)    /* the scene's path: x metres to its right, back metres behind its start (or from its point z) */
#define PARKED (-2)     /* standing at x, z, facing back degrees (0 north, 90 east) */

typedef struct {
    int8_t model, livery;
    /* The route it is on (level.h order: 0 the way in, 1-3 the ways up,
       4 the summit trail, 5 the extreme trail; 11-13 the way in, bound
       for that way up), ON_PATH or PARKED. */
    int8_t route;
    int8_t lights;      /* LIGHTS_*, or -1: the CPU driver's call */
    float x, z;         /* placed at the route's point nearest here... */
    float back;         /* ...this many metres back down it */
    float delay;        /* seconds before it sets off */
} FilmTruck;

typedef struct {
    const char *name;
    int8_t area;        /* AREA_* */
    float hour, rate;   /* the start hour; hours a second for a time-lapse */
    float pre, secs;    /* run-up before "go", then the shot */
    int shot, watch;    /* the camera, and the truck it is on (and we hear) */
    float fov;          /* 0: 1.2 */
    float a[9];
    int n;
    FilmTruck t[MAX_TRUCKS];
    /* A path off the routes for trucks with route -1 (x, z pairs), its
       speed (m/s) and drive mode. */
    const float (*path)[2];
    int npath;
    float path_speed;
    int path_mode;
    int gag;            /* G_* */
    /* Slow motion: the world runs at this rate (0.22, say) for a moment
       each time a truck in the shot loses something, the camera carrying
       on at its own pace. 0: none. */
    float slow;
} Scene;

/* ---- Paths off the routes ---- */
static const float p_ford[][2] = {{-30, -216}, {-48, -216.5f}, {-61, -217}, {-80, -219}, {-95, -221}, {-115, -223}, {-140, -226}};
static const float p_boulders[][2] = {{-100, -266}, {-114, -260}, {-126, -255}, {-138, -247}, {-150, -238}, {-170, -226}};
static const float p_deer[][2] = {{100, -232}, {112, -224}, {130, -210}, {150, -198}, {170, -192}, {200, -188}, {230, -184}};
static const float p_slide[][2] = {{-20, -262}, {0, -252}, {22, -244}, {34, -232}, {32, -216}, {18, -208}, {0, -212}, {-12, -226}, {-6, -244}, {12, -250}};
static const float p_lemmings[][2] = {{-138.7f, 205.2f}, {-135.7f, 207.9f}, {-132, 209.1f}, {-128, 209.7f}, {-124, 209.9f}, {-120, 210}, {-116, 210}, {-112, 210}, {-108, 210}, {-104, 210}, {-100, 210}, {-96, 210}, {-92, 210}, {-88, 210}, {-84, 209.8f}, {-80, 209.4f}, {-76.1f, 208.7f}, {-72.4f, 207.3f}, {-68.8f, 205.4f}, {-65.5f, 203.2f}, {-61.9f, 201.4f}, {-58.2f, 200}, {-54.3f, 198.9f}, {-50.4f, 198.3f}, {-46.4f, 198}, {-42.4f, 198}, {-38.4f, 198.1f}, {-34.4f, 198.3f}, {-30.4f, 198.7f}, {-26.4f, 199.1f}, {-22.4f, 199.5f}, {-18.5f, 200.1f}, {-14.5f, 200.7f}, {-10.6f, 201.4f}, {-6.7f, 202.1f}, {-2.7f, 202.8f}, {1.2f, 203.4f}, {5.2f, 204}, {9.2f, 204.6f}, {13.1f, 205}, {17.1f, 205.4f}, {20, 213}, {22, 272}};
static const float p_tumble[][2] = {{96.8f, -182.8f}, {164.4f, -263.1f}};
static const float p_jetty[][2] = {{30, -296}, {30, -318}, {30, -340}, {30, -359}, {30, -392}};
static const float p_sheep[][2] = {{48, -205}, {54.489f, -206.028f}, {60.343f, -209.011f}, {64.989f, -213.657f}, {67.972f, -219.511f}, {69, -226}, {67.972f, -232.489f}, {64.989f, -238.343f}, {60.343f, -242.989f}, {54.489f, -245.972f}, {48, -247}, {41.511f, -245.972f}, {35.657f, -242.989f}, {31.011f, -238.343f}, {28.028f, -232.489f}, {27, -226}, {28.028f, -219.511f}, {31.011f, -213.657f}, {35.657f, -209.011f}, {41.511f, -206.028f}, {48, -205}, {54.489f, -206.028f}, {60.343f, -209.011f}, {64.989f, -213.657f}, {67.972f, -219.511f}, {69, -226}, {67.972f, -232.489f}, {64.989f, -238.343f}, {60.343f, -242.989f}, {54.489f, -245.972f}, {48, -247}, {41.511f, -245.972f}, {35.657f, -242.989f}, {31.011f, -238.343f}, {28.028f, -232.489f}, {27, -226}, {28.028f, -219.511f}, {31.011f, -213.657f}, {35.657f, -209.011f}, {41.511f, -206.028f}, {48, -205}, {54.489f, -206.028f}, {60.343f, -209.011f}, {64.989f, -213.657f}, {67.972f, -219.511f}, {69, -226}, {67.972f, -232.489f}, {64.989f, -238.343f}, {60.343f, -242.989f}, {54.489f, -245.972f}, {48, -247}, {41.511f, -245.972f}, {35.657f, -242.989f}, {31.011f, -238.343f}, {28.028f, -232.489f}, {27, -226}, {28.028f, -219.511f}, {31.011f, -213.657f}, {35.657f, -209.011f}, {41.511f, -206.028f}, {48, -205}, {54.489f, -206.028f}, {60.343f, -209.011f}, {64.989f, -213.657f}};
static const float p_tbone[][2] = {{-34, -250.8f}, {12, -241.6f}, {60, -232}};
static const float p_tbone2[][2] = {{-34, -250.8f}, {12, -241.6f}, {60, -232}, {110, -236}, {160, -230}};
#define PATH(p) p, (int)(sizeof(p) / sizeof(p[0]))

enum { T_CASCADE, T_KESTREL, T_BOBCAT, T_FFR, T_TACO, T_GOOSE, T_ONETEN, T_EAGLE };

/* name, area, hour, time-lapse rate, run-up, seconds, shot, watched truck,
   fov, the shot's numbers, trucks, {model, livery, route, lights, x, z,
   back, delay} each, then a path, its speed and drive mode. */
static const Scene scenes[] = {
    /* Dawn mist over the meadow, the clock running fast. */
    {"c-dawn-lapse", AREA_CASCADE, 5, 0.22f, 2, 12, SH_STATIC, 0, 1.05f,
     {10, 6, -300, 20, 40, 60},
     0, {{0}},
     NULL, 0, 0, 0},
    /* Sunrise mist on the forest road. */
    {"c-forest-mist", AREA_CASCADE, 6.5f, 0, 4, 12, SH_HELI, 0, 1.15f,
     {18, 7, 0.15f, 2.4f},
     4,
     {{T_BOBCAT, 3, 0, LIGHTS_DIPPED, 8, -160, 0, 0},
      {T_CASCADE, 0, 0, LIGHTS_DIPPED, 8, -160, 12, 0},
      {T_KESTREL, 1, 0, LIGHTS_DIPPED, 8, -160, 24, 0},
      {T_FFR, 2, 0, LIGHTS_DIPPED, 8, -160, 36, 0}},
     NULL, 0, 0, 0},
    /* Through the deep ford and under the stone arch. */
    {"c-ford", AREA_CASCADE, 9.6f, 0, 3, 14, SH_STATIC, 0, 1,
     {-50, 2.2f, -209, 0, 0, 0},
     2,
     {{T_KESTREL, 1, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 0, 16, 0}},
     PATH(p_ford), 6, DRIVE_4WD},
    {"c-arch", AREA_CASCADE, 9.6f, 0, 8, 9, SH_STATIC, 0, 1,
     {-114, 1.5f, -226, 0, 0, 0},
     2,
     {{T_KESTREL, 1, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 0, 16, 0}},
     PATH(p_ford), 6, DRIVE_4WD},
    /* Crawling the boulder field. */
    {"c-boulders", AREA_CASCADE, 11, 0, 3, 14, SH_HELI, 0, 1.15f,
     {13, 4.5f, 0.22f, 2.2f},
     1,
     {{T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_boulders), 2.6f, DRIVE_LOCK},
    /* Deer in the east meadow. */
    {"c-deer", AREA_CASCADE, 8, 0, 3, 12, SH_STATIC, 0, 0.95f,
     {164, 1.4f, -185, 0, 0, 0},
     1,
     {{T_TACO, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_deer), 9, DRIVE_4WD},
    /* The washout on the logging road. */
    {"c-washout", AREA_CASCADE, 15, 0, 4, 12, SH_POST, 0, 0,
     {40, 6, 3},
     3,
     {{T_KESTREL, 1, 1, -1, 238, 31, 40, 0},
      {T_CASCADE, 0, 1, -1, 238, 31, 54, 0},
      {T_FFR, 2, 1, -1, 238, 31, 68, 0}},
     NULL, 0, 0, 0},
    /* The log bridge and the ford below it. */
    {"c-logbridge", AREA_CASCADE, 10, 0, 4, 12, SH_STATIC, 0, 1,
     {197, 3.5f, 84, 0, 0, 0},
     2,
     {{T_ONETEN, 0, 1, -1, 192, 101, 34, 0},
      {T_GOOSE, 2, 1, -1, 192, 101, 48, 0}},
     NULL, 0, 0, 0},
    /* Every vehicle up the logging road's east flank. */
    {"c-logging-8", AREA_CASCADE, 9, 0, 5, 14, SH_HELI, 3, 1.15f,
     {36, 14, -0.1f, -2.2f},
     8,
     {{T_EAGLE, 3, 1, -1, 160, -70, 0, 0},
      {T_ONETEN, 0, 1, -1, 160, -70, 11, 0},
      {T_GOOSE, 2, 1, -1, 160, -70, 22, 0},
      {T_TACO, 0, 1, -1, 160, -70, 33, 0},
      {T_KESTREL, 1, 1, -1, 160, -70, 44, 0},
      {T_BOBCAT, 3, 1, -1, 160, -70, 55, 0},
      {T_FFR, 2, 1, -1, 160, -70, 66, 0},
      {T_CASCADE, 0, 1, -1, 160, -70, 77, 0}},
     NULL, 0, 0, 0},
    /* Widowmaker's hill. */
    {"c-widow", AREA_CASCADE, 11, 0, 4, 14, SH_POST, 0, 0,
     {34, 7, 3},
     2,
     {{T_TACO, 0, 5, -1, 50.2f, 225.7f, 0, 0},
      {T_EAGLE, 3, 5, -1, 50.2f, 225.7f, 14, 0}},
     NULL, 0, 0, 0},
    {"c-widow-heli", AREA_CASCADE, 11, 0, 4, 14, SH_HELI, 0, 1.15f,
     {20, 8, 0.14f, 2.2f},
     2,
     {{T_TACO, 0, 5, -1, 35.1f, 220.3f, 0, 0},
      {T_EAGLE, 3, 5, -1, 35.1f, 220.3f, 14, 0}},
     NULL, 0, 0, 0},
    /* The ridge line's knife edge. */
    {"c-knife", AREA_CASCADE, 11.5f, 0, 3, 14, SH_HELI, 0, 1.15f,
     {18, 7, 0.16f, 2.6f},
     1,
     {{T_FFR, 2, 2, -1, 72, 12, 18, 0}},
     NULL, 0, 0, 0},
    /* The summit trail past the lake. */
    {"c-lake", AREA_CASCADE, 10.5f, 0, 3, 12, SH_STATIC, 0, 1,
     {22, 1.4f, 62, 0, 0, 0},
     2,
     {{T_ONETEN, 0, 4, -1, 2, 44, 0, 0},
      {T_KESTREL, 1, 4, -1, 2, 44, 0, 4}},
     NULL, 0, 0, 0},
    /* The lookout over the cloud. */
    {"c-summit-day", AREA_CASCADE, 12, 0, 3, 15, SH_HELI, 0, 1.2f,
     {22, 9, 0.12f, 2.4f},
     3,
     {{T_BOBCAT, 3, 4, -1, 30, 140, 40, 0},
      {T_TACO, 0, 4, -1, 30, 140, 53, 0},
      {T_ONETEN, 0, 4, -1, 30, 140, 66, 0}},
     NULL, 0, 0, 0},
    {"c-summit-sunset", AREA_CASCADE, 18.4f, 0, 3, 15, SH_HELI, 0, 1.2f,
     {22, 9, 0.12f, 2.4f},
     3,
     {{T_BOBCAT, 3, 4, -1, 30, 140, 40, 0},
      {T_TACO, 0, 4, -1, 30, 140, 53, 0},
      {T_ONETEN, 0, 4, -1, 30, 140, 66, 0}},
     NULL, 0, 0, 0},
    /* The old pickup in 2WD on the meadow grass. */
    {"c-slide", AREA_CASCADE, 9, 0, 3, 12, SH_HELI, 0, 1.15f,
     {15, 5, 0.2f, 1.8f},
     1,
     {{T_CASCADE, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_slide), 13, DRIVE_2WD},
    /* Every vehicle out along the rim road under the first cliffs. */
    {"m-flats-8", AREA_MOAB, 15, 0, 5, 14, SH_HELI, 3, 1.15f,
     {36, 14, 0.1f, 2.3f},
     8,
     {{T_EAGLE, 3, 1, -1, 150, -190, 0, 0},
      {T_ONETEN, 0, 1, -1, 150, -190, 11, 0},
      {T_GOOSE, 2, 1, -1, 150, -190, 22, 0},
      {T_TACO, 0, 1, -1, 150, -190, 33, 0},
      {T_KESTREL, 1, 1, -1, 150, -190, 44, 0},
      {T_BOBCAT, 3, 1, -1, 150, -190, 55, 0},
      {T_FFR, 2, 1, -1, 150, -190, 66, 0},
      {T_CASCADE, 0, 1, -1, 150, -190, 77, 0}},
     NULL, 0, 0, 0},
    /* The dugway's switchbacks. */
    {"m-dugway", AREA_MOAB, 15.6f, 0, 4, 12, SH_POST, 0, 0,
     {44, 7, 3.5f},
     3,
     {{T_TACO, 0, 1, -1, 250, -171, 34, 0},
      {T_EAGLE, 3, 1, -1, 250, -171, 48, 0},
      {T_KESTREL, 1, 1, -1, 250, -171, 62, 0}},
     NULL, 0, 0, 0},
    {"m-rim-heli", AREA_MOAB, 16.2f, 0, 4, 14, SH_HELI, 1, 1.15f,
     {26, 11, -0.12f, -2.2f},
     4,
     {{T_EAGLE, 3, 1, -1, 215, -64, 20, 0},
      {T_TACO, 0, 1, -1, 215, -64, 32, 0},
      {T_FFR, 2, 1, -1, 215, -64, 44, 0},
      {T_BOBCAT, 3, 1, -1, 215, -64, 56, 0}},
     NULL, 0, 0, 0},
    /* Up the spine of a fin. */
    {"m-fins-post", AREA_MOAB, 16.6f, 0, 4, 14, SH_POST, 0, 0,
     {48, 8, 3},
     2,
     {{T_BOBCAT, 3, 3, -1, 29, -91.5f, 44, 0},
      {T_TACO, 0, 3, -1, 29, -91.5f, 58, 0}},
     NULL, 0, 0, 0},
    {"m-fins-heli", AREA_MOAB, 16.6f, 0, 4, 14, SH_HELI, 0, 1.15f,
     {22, 9, 0.15f, 2.5f},
     2,
     {{T_BOBCAT, 3, 3, -1, 29, -91.5f, 30, 0},
      {T_TACO, 0, 3, -1, 29, -91.5f, 44, 0}},
     NULL, 0, 0, 0},
    /* Bighorn Canyon. */
    {"m-canyon", AREA_MOAB, 16, 0, 4, 12, SH_LEAD, 0, 1.15f,
     {13, -2, 2.4f},
     3,
     {{T_KESTREL, 1, 2, -1, -184, -46, 24, 0},
      {T_CASCADE, 0, 2, -1, -184, -46, 36, 0},
      {T_FFR, 2, 2, -1, -184, -46, 48, 0}},
     NULL, 0, 0, 0},
    {"m-canyon-far", AREA_MOAB, 16, 0, 4, 12, SH_FAR, 2, 0,
     {0},
     3,
     {{T_KESTREL, 1, 2, -1, -188, -55, 60, 0},
      {T_CASCADE, 0, 2, -1, -188, -55, 72, 0},
      {T_FFR, 2, 2, -1, -188, -55, 84, 0}},
     NULL, 0, 0, 0},
    /* Out through the gap by Window Arch. */
    {"m-arch", AREA_MOAB, 17, 0, 4, 14, SH_POST, 0, 0,
     {46, 6, 2.6f},
     3,
     {{T_ONETEN, 3, 1, -1, 10, 69, 44, 0},
      {T_GOOSE, 2, 1, -1, 10, 69, 57, 0},
      {T_EAGLE, 3, 1, -1, 10, 69, 70, 0}},
     NULL, 0, 0, 0},
    /* The shelf road across the mesa's cliff. */
    {"m-shelf", AREA_MOAB, 17.5f, 0, 4, 14, SH_HELI, 0, 1.15f,
     {26, 8, -0.1f, -1.8f},
     2,
     {{T_TACO, 3, 4, -1, 93, 96, 36, 0},
      {T_ONETEN, 3, 4, -1, 93, 96, 50, 0}},
     NULL, 0, 0, 0},
    {"m-shelf-far", AREA_MOAB, 17.5f, 0, 4, 12, SH_FAR, 1, 0,
     {0},
     2,
     {{T_TACO, 3, 4, -1, 93, 96, 20, 0},
      {T_ONETEN, 3, 4, -1, 93, 96, 34, 0}},
     NULL, 0, 0, 0},
    /* The High Wire: the Dorsal, a fin 6 m wide. */
    {"m-dorsal", AREA_MOAB, 17.8f, 0, 4, 15, SH_HELI, 0, 1.15f,
     {28, 10, 0.12f, 2},
     2,
     {{T_EAGLE, 3, 5, -1, -213.2f, 125.4f, 6, 0},
      {T_TACO, 0, 5, -1, -213.2f, 125.4f, 22, 0}},
     NULL, 0, 0, 0},
    {"m-dorsal-post", AREA_MOAB, 17.8f, 0, 6, 15, SH_POST, 0, 0,
     {64, 2.4f, 1.7f},
     2,
     {{T_EAGLE, 3, 5, -1, -213.2f, 125.4f, 6, 0},
      {T_TACO, 0, 5, -1, -213.2f, 125.4f, 22, 0}},
     NULL, 0, 0, 0},
    /* The Catwalk, between the cliff and the crack. */
    {"m-catwalk", AREA_MOAB, 18, 0, 3, 13, SH_FAR, 0, 0,
     {0},
     1,
     {{T_BOBCAT, 3, 5, -1, -138.7f, 205.2f, 0, 0}},
     NULL, 0, 0, 0},
    {"m-catwalk-heli", AREA_MOAB, 18, 0, 3, 13, SH_HELI, 0, 1.15f,
     {24, 9, -0.12f, 1.9f},
     1,
     {{T_BOBCAT, 3, 5, -1, -138.7f, 205.2f, 0, 0}},
     NULL, 0, 0, 0},
    /* The Diving Board as the sun goes. */
    {"m-summit-arrive", AREA_MOAB, 18.3f, 0, 4, 13, SH_POST, 0, 0,
     {44, 6, 2.6f},
     3,
     {{T_KESTREL, 1, 4, -1, 25, 204, 54, 0},
      {T_FFR, 2, 4, -1, 25, 204, 67, 0},
      {T_TACO, 0, 4, -1, 25, 204, 80, 0}},
     NULL, 0, 0, 0},
    {"m-summit-orbit", AREA_MOAB, 18.45f, 0, 3, 13, SH_ORBIT, 0, 1.15f,
     {22, 203, 1.5f, 20, 6, 0.12f, 2.6f},
     5,
     {{T_EAGLE, 3, PARKED, LIGHTS_OFF, 10, 203, 0, 0},
      {T_TACO, 0, PARKED, LIGHTS_OFF, 16, 204, 0, 0},
      {T_ONETEN, 3, PARKED, LIGHTS_OFF, 22, 204, 0, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_OFF, 28, 204, 0, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_OFF, 34, 203, 0, 0}},
     NULL, 0, 0, 0},
    /* The day going over the mesa. */
    {"m-lapse", AREA_MOAB, 17.2f, 0.2f, 2, 12, SH_STATIC, 0, 1.1f,
     {40, 14, 190, -160, 40, -40},
     0, {{0}},
     NULL, 0, 0, 0},
    /* The corrie from the loch's shore. */
    {"l-loch", AREA_LOWLAND, 9.6f, 0, 2, 11, SH_STATIC, 0, 1.1f,
     {18, 3, -322, -20, 60, 60, 44, 5, -318},
     0, {{0}},
     NULL, 0, 0, 0},
    /* A shower coming up the loch. */
    {"l-rain-lapse", AREA_LOWLAND, 12.05f, 0.1f, 2, 13, SH_STATIC, 0, 1.1f,
     {18, 3, -322, -20, 60, 60},
     0, {{0}},
     NULL, 0, 0, 0},
    /* Over the stone bridge in the rain. */
    {"l-bridge", AREA_LOWLAND, 13.2f, 0, 6, 13, SH_STATIC, 0, 1,
     {38, 2, -203, 0, 0, 0},
     4,
     {{T_ONETEN, 0, 11, LIGHTS_DIPPED, -3, -225, 0, 0},
      {T_CASCADE, 0, 11, LIGHTS_DIPPED, -3, -225, 11, 0},
      {T_FFR, 2, 11, LIGHTS_DIPPED, -3, -225, 22, 0},
      {T_KESTREL, 1, 11, LIGHTS_DIPPED, -3, -225, 33, 0}},
     NULL, 0, 0, 0},
    /* The drove road's shelf under the crag. */
    {"l-shelf-rain", AREA_LOWLAND, 13.4f, 0, 4, 13, SH_POST, 0, 0,
     {44, 6, 3},
     3,
     {{T_ONETEN, 0, 1, LIGHTS_DIPPED, 90, -66, 24, 0},
      {T_FFR, 2, 1, LIGHTS_DIPPED, 90, -66, 37, 0},
      {T_BOBCAT, 3, 1, LIGHTS_DIPPED, 90, -66, 50, 0}},
     NULL, 0, 0, 0},
    {"l-shelf-heli", AREA_LOWLAND, 13.6f, 0, 4, 14, SH_HELI, 0, 1.15f,
     {24, 8, 0.12f, 2.2f},
     3,
     {{T_ONETEN, 0, 1, LIGHTS_DIPPED, 128, -26, 30, 0},
      {T_FFR, 2, 1, LIGHTS_DIPPED, 128, -26, 43, 0},
      {T_BOBCAT, 3, 1, LIGHTS_DIPPED, 128, -26, 56, 0}},
     NULL, 0, 0, 0},
    /* The Devil's Staircase by the burn. */
    {"l-stair", AREA_LOWLAND, 10.6f, 0, 4, 13, SH_POST, 0, 0,
     {34, 5, 1.6f},
     3,
     {{T_BOBCAT, 3, 2, -1, 30, -69, 34, 0},
      {T_TACO, 0, 2, -1, 30, -69, 47, 0},
      {T_KESTREL, 1, 2, -1, 30, -69, 60, 0}},
     NULL, 0, 0, 0},
    /* Round Lochan Dubh on the peat road. */
    {"l-lochan", AREA_LOWLAND, 11, 0, 4, 14, SH_HELI, 0, 1.15f,
     {24, 8, 0.14f, 2.2f},
     2,
     {{T_CASCADE, 0, 3, -1, -242, -82, 34, 0},
      {T_ONETEN, 0, 3, -1, -242, -82, 48, 0}},
     NULL, 0, 0, 0},
    /* Every vehicle up the hairpins at the corrie's head. */
    {"l-hairpins-8", AREA_LOWLAND, 15.3f, 0, 5, 14, SH_HELI, 3, 1.15f,
     {38, 18, 0.08f, 2.4f},
     8,
     {{T_EAGLE, 3, 1, -1, 85.7f, 186.5f, 40, 0},
      {T_ONETEN, 0, 1, -1, 85.7f, 186.5f, 51, 0},
      {T_GOOSE, 2, 1, -1, 85.7f, 186.5f, 62, 0},
      {T_TACO, 0, 1, -1, 85.7f, 186.5f, 73, 0},
      {T_KESTREL, 1, 1, -1, 85.7f, 186.5f, 84, 0},
      {T_BOBCAT, 3, 1, -1, 85.7f, 186.5f, 95, 0},
      {T_FFR, 2, 1, -1, 85.7f, 186.5f, 106, 0},
      {T_CASCADE, 0, 1, -1, 85.7f, 186.5f, 117, 0}},
     NULL, 0, 0, 0},
    /* The Black Rake. */
    {"l-rake", AREA_LOWLAND, 16.5f, 0, 4, 15, SH_HELI, 0, 1.15f,
     {28, 6, 0, 1.9f},
     2,
     {{T_ONETEN, 0, 5, -1, 148, 57.3f, 0, 0},
      {T_BOBCAT, 3, 5, -1, 148, 57.3f, 16, 0}},
     NULL, 0, 0, 0},
    {"l-rake-far", AREA_LOWLAND, 16.5f, 0, 4, 13, SH_FAR, 1, 0,
     {0},
     2,
     {{T_ONETEN, 0, 5, -1, 153.7f, 42.4f, 0, 0},
      {T_BOBCAT, 3, 5, -1, 153.7f, 42.4f, 16, 0}},
     NULL, 0, 0, 0},
    /* The Causeway. */
    {"l-causeway", AREA_LOWLAND, 17, 0, 4, 15, SH_HELI, 0, 1.15f,
     {20, 5, 0.16f, 2.4f},
     3,
     {{T_ONETEN, 0, 5, -1, 209.8f, 66.3f, 0, 0},
      {T_EAGLE, 3, 5, -1, 209.8f, 66.3f, 14, 0},
      {T_TACO, 0, 5, -1, 209.8f, 66.3f, 28, 0}},
     NULL, 0, 0, 0},
    /* The mast at dusk. */
    {"l-mast", AREA_LOWLAND, 18.6f, 0, 4, 13, SH_POST, 0, 0,
     {44, 6, 2.6f},
     3,
     {{T_ONETEN, 0, 4, -1, 170, 276, 54, 0},
      {T_KESTREL, 1, 4, -1, 170, 276, 67, 0},
      {T_GOOSE, 2, 4, -1, 170, 276, 80, 0}},
     NULL, 0, 0, 0},
    {"l-summit-orbit", AREA_LOWLAND, 18.8f, 0, 3, 13, SH_ORBIT, 0, 1.15f,
     {176, 284, 2, 24, 7, 0.11f, 0.5f},
     6,
     {{T_ONETEN, 0, PARKED, LIGHTS_DIPPED, 182.5f, 272.7f, 330, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_DIPPED, 174.6f, 271.1f, 6, 0},
      {T_FFR, 2, PARKED, LIGHTS_DIPPED, 167.3f, 274.3f, 42, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_DIPPED, 163.3f, 281.3f, 78, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_DIPPED, 164.1f, 289.3f, 114, 0},
      {T_TACO, 0, PARKED, LIGHTS_DIPPED, 169.5f, 295.3f, 150, 0}},
     NULL, 0, 0, 0},
    /* Rain in the beams. */
    {"l-night-rain", AREA_LOWLAND, 21.3f, 0, 4, 12, SH_LEAD, 0, 1.1f,
     {14, 3, 2.2f},
     2,
     {{T_ONETEN, 0, 1, LIGHTS_FULL, 90, -66, 10, 0},
      {T_FFR, 2, 1, LIGHTS_FULL, 90, -66, 24, 0}},
     NULL, 0, 0, 0},
    /* The dune field from the razorback. */
    {"d-establish", AREA_DUNES, 16.8f, 0, 2, 11, SH_STATIC, 0, 1.1f,
     {-233, 6, -31, 60, 40, 200, -210, 10, -6},
     0, {{0}},
     NULL, 0, 0, 0},
    /* Every vehicle along the shore road. */
    {"d-shore-8", AREA_DUNES, 17.3f, 0, 5, 14, SH_HELI, 3, 1.15f,
     {36, 13, 0.1f, 2.3f},
     8,
     {{T_GOOSE, 2, 1, -1, 194, -209, 30, 0},
      {T_EAGLE, 3, 1, -1, 194, -209, 41, 0},
      {T_TACO, 0, 1, -1, 194, -209, 52, 0},
      {T_ONETEN, 0, 1, -1, 194, -209, 63, 0},
      {T_KESTREL, 1, 1, -1, 194, -209, 74, 0},
      {T_BOBCAT, 3, 1, -1, 194, -209, 85, 0},
      {T_FFR, 2, 1, -1, 194, -209, 96, 0},
      {T_CASCADE, 0, 1, -1, 194, -209, 107, 0}},
     NULL, 0, 0, 0},
    /* The powerline's whoops. */
    {"d-whoops", AREA_DUNES, 17.6f, 0, 4, 13, SH_POST, 0, 0,
     {42, 6, 1.4f},
     4,
     {{T_GOOSE, 2, 3, -1, 25, -110, 40, 0},
      {T_EAGLE, 3, 3, -1, 25, -110, 52, 0},
      {T_TACO, 0, 3, -1, 25, -110, 64, 0},
      {T_BOBCAT, 3, 3, -1, 25, -110, 76, 0}},
     NULL, 0, 0, 0},
    {"d-whoops-side", AREA_DUNES, 17.6f, 0, 4, 12, SH_SIDE, 0, 1.15f,
     {-9, 1.3f, 2},
     2,
     {{T_GOOSE, 2, 3, -1, 25, -110, 30, 0},
      {T_EAGLE, 3, 3, -1, 25, -110, 42, 0}},
     NULL, 0, 0, 0},
    /* Along the razorback's crest. */
    {"d-razor", AREA_DUNES, 17.9f, 0, 4, 15, SH_HELI, 0, 1.15f,
     {26, 8, 0.12f, 2},
     2,
     {{T_EAGLE, 3, 2, -1, -233, -31.5f, 44, 0},
      {T_GOOSE, 2, 2, -1, -233, -31.5f, 60, 0}},
     NULL, 0, 0, 0},
    {"d-razor-far", AREA_DUNES, 17.9f, 0, 4, 13, SH_FAR, 1, 0,
     {0},
     2,
     {{T_EAGLE, 3, 2, -1, -215, -52, 40, 0},
      {T_GOOSE, 2, 2, -1, -215, -52, 56, 0}},
     NULL, 0, 0, 0},
    /* Witch's Eye: the first drop and the charge. */
    {"d-drop", AREA_DUNES, 18.1f, 0, 4, 15, SH_POST, 0, 0,
     {54, -12, 2},
     2,
     {{T_GOOSE, 2, 5, -1, -297.4f, 46.4f, 0, 0},
      {T_EAGLE, 3, 5, -1, -297.4f, 46.4f, 16, 0}},
     NULL, 0, 0, 0},
    /* Comp Hill. */
    {"d-comp", AREA_DUNES, 18.25f, 0, 5, 16, SH_POST, 0, 0,
     {76, 10, 3},
     3,
     {{T_EAGLE, 3, 5, -1, -128.2f, 204.1f, 0, 0},
      {T_GOOSE, 2, 5, -1, -128.2f, 204.1f, 22, 0},
      {T_TACO, 0, 5, -1, -128.2f, 204.1f, 44, 0}},
     NULL, 0, 0, 0},
    {"d-comp-heli", AREA_DUNES, 18.25f, 0, 4, 15, SH_HELI, 0, 1.15f,
     {26, 9, 0.12f, 2.2f},
     2,
     {{T_EAGLE, 3, 5, -1, -115.3f, 213.5f, 0, 0},
      {T_GOOSE, 2, 5, -1, -115.3f, 213.5f, 22, 0}},
     NULL, 0, 0, 0},
    /* The Blade, the king dune's crest, at sunset. */
    {"d-blade", AREA_DUNES, 18.45f, 0, 4, 15, SH_HELI, 0, 1.15f,
     {28, 7, 0.1f, 1.9f},
     2,
     {{T_EAGLE, 3, 5, -1, -2.5f, 303, 0, 0},
      {T_GOOSE, 2, 5, -1, -2.5f, 303, 16, 0}},
     NULL, 0, 0, 0},
    /* All eight on the shore as the sun goes. */
    {"d-lineup", AREA_DUNES, 18.2f, 0, 3, 13, SH_ORBIT, 0, 1.05f,
     {7, -306, 1.2f, 25, 2.2f, 0.07f, 0.75f},
     8,
     {{T_CASCADE, 0, PARKED, LIGHTS_OFF, -14, -306, 0, 0},
      {T_FFR, 2, PARKED, LIGHTS_OFF, -8, -306, 0, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_OFF, -2, -306, 0, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_OFF, 4, -306, 0, 0},
      {T_TACO, 0, PARKED, LIGHTS_OFF, 10, -306, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 16, -306, 0, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_OFF, 22, -306, 0, 0},
      {T_EAGLE, 3, PARKED, LIGHTS_OFF, 28, -306, 0, 0}},
     NULL, 0, 0, 0},
    /* Sunset into night from the king dune. */
    {"d-king-lapse", AREA_DUNES, 17.9f, 0.22f, 2, 12, SH_STATIC, 0, 1.1f,
     {70, 8, 214, -200, 10, -100},
     0, {{0}},
     NULL, 0, 0, 0},
    /* Dusk, whips lit. */
    {"d-dusk-8", AREA_DUNES, 19.2f, 0, 5, 14, SH_HELI, 3, 1.15f,
     {34, 11, 0.1f, 2.3f},
     8,
     {{T_GOOSE, 2, 3, LIGHTS_DIPPED, 25, -110, 20, 0},
      {T_EAGLE, 3, 3, LIGHTS_DIPPED, 25, -110, 31, 0},
      {T_TACO, 0, 3, LIGHTS_DIPPED, 25, -110, 42, 0},
      {T_ONETEN, 0, 3, LIGHTS_DIPPED, 25, -110, 53, 0},
      {T_KESTREL, 1, 3, LIGHTS_DIPPED, 25, -110, 64, 0},
      {T_BOBCAT, 3, 3, LIGHTS_DIPPED, 25, -110, 75, 0},
      {T_FFR, 2, 3, LIGHTS_DIPPED, 25, -110, 86, 0},
      {T_CASCADE, 0, 3, LIGHTS_DIPPED, 25, -110, 97, 0}},
     NULL, 0, 0, 0},
    /* The camp after dark. */
    {"d-camp-night", AREA_DUNES, 21.5f, 0, 3, 14, SH_ORBIT, 0, 1.15f,
     {54, -283, 1.2f, 35, 6, 0.09f, 3.5f},
     8,
     {{T_EAGLE, 3, PARKED, LIGHTS_DIPPED, 50.5f, -302.7f, 10, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_DIPPED, 45.5f, -301.1f, 25, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_DIPPED, 41.1f, -298.3f, 40, 0},
      {T_TACO, 0, PARKED, LIGHTS_DIPPED, 37.6f, -294.5f, 55, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_DIPPED, 35.2f, -289.8f, 70, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_DIPPED, 34.1f, -284.7f, 85, 0},
      {T_FFR, 2, PARKED, LIGHTS_DIPPED, 34.3f, -279.5f, 100, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_DIPPED, 35.9f, -274.5f, 115, 0}},
     NULL, 0, 0, 0},
    /* A night run under the wires. */
    {"d-night-run", AREA_DUNES, 22, 0, 4, 12, SH_FAR, 1, 0,
     {0},
     3,
     {{T_GOOSE, 2, 3, LIGHTS_FULL, 22, -116, 20, 0},
      {T_EAGLE, 3, 3, LIGHTS_FULL, 22, -116, 33, 0},
      {T_TACO, 0, 3, LIGHTS_FULL, 22, -116, 46, 0}},
     NULL, 0, 0, 0},
    /* A quiet dawn in the meadow, and then the trucks arrive. */
    {"x-rain", AREA_CASCADE, 6.4f, 0, 1, 13, SH_STATIC, 0, 1.15f,
     {12, 3.2f, -302, 12, 3, -262},
     8,
     {{T_EAGLE, 3, PARKED, LIGHTS_OFF, 0, -280, 20, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 14, -276, 300, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_OFF, 26, -282, 110, 0},
      {T_TACO, 0, PARKED, LIGHTS_OFF, 6, -268, 200, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_OFF, 20, -264, 60, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_OFF, -6, -272, 250, 0},
      {T_FFR, 2, PARKED, LIGHTS_OFF, 32, -270, 330, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_OFF, 12, -258, 160, 0}},
     NULL, 0, 0, 0, G_RAIN},
    /* Deer, and a truck going by head over heels. */
    {"x-tumble", AREA_CASCADE, 8, 0, 2, 12, SH_STATIC, 0, 0.95f,
     {164, 1.4f, -185, 125.8f, 2.2f, -217.2f},
     1,
     {{T_FFR, 2, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_tumble), 8, DRIVE_4WD, G_TUMBLE},
    /* One on its roof, spinning, and an audience. */
    {"x-break", AREA_MOAB, 14, 0, 3, 12, SH_ORBIT, 0, 1.15f,
     {18, -266, 1, 18, 3.4f, 0.14f, 0.4f, 0, 3.6f},
     8,
     {{T_KESTREL, 1, PARKED, LIGHTS_OFF, 18, -266, 0, 0},
      {T_EAGLE, 3, PARKED, LIGHTS_OFF, 18, -255, 180, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_OFF, 26.6f, -259.1f, 231.4f, 0},
      {T_TACO, 0, PARKED, LIGHTS_OFF, 28.7f, -268.4f, 282.9f, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 22.8f, -275.9f, 334.3f, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_OFF, 13.2f, -275.9f, 25.7f, 0},
      {T_FFR, 2, PARKED, LIGHTS_OFF, 7.3f, -268.4f, 77.2f, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_OFF, 9.4f, -259.1f, 128.6f, 0}},
     NULL, 0, 0, 0, G_BREAK},
    /* The whoops with most of the gravity turned off. */
    {"x-lowg", AREA_DUNES, 17.6f, 0, 4, 13, SH_POST, 0, 0,
     {42, 6, 1.4f},
     4,
     {{T_GOOSE, 2, 3, -1, 25, -110, 40, 0},
      {T_EAGLE, 3, 3, -1, 25, -110, 52, 0},
      {T_TACO, 0, 3, -1, 25, -110, 64, 0},
      {T_BOBCAT, 3, 3, -1, 25, -110, 76, 0}},
     NULL, 0, 0, 0, G_LOWG},
    /* A queue behind the cattle. */
    {"x-cows", AREA_LOWLAND, 15, 0, 3, 13, SH_ORBIT, 0, 1.1f,
     {-56, -262, 1.4f, 22, 2.6f, 0.03f, 4.71f},
     5,
     {{T_ONETEN, 0, PARKED, LIGHTS_OFF, -44, -262, 270, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_OFF, -37, -262, 270, 0},
      {T_FFR, 2, PARKED, LIGHTS_OFF, -30, -262, 270, 0},
      {T_EAGLE, 3, PARKED, LIGHTS_OFF, -23, -262, 270, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_OFF, -16, -262, 270, 0}},
     NULL, 0, 0, 0, G_HORNS},
    /* Rounding up the sheep. */
    {"x-sheep", AREA_LOWLAND, 15.2f, 0, 2, 14, SH_ORBIT, 0, 1.1f,
     {48, -226, 1, 38, 13, 0.05f, 2},
     1,
     {{T_GOOSE, 2, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_sheep), 10, DRIVE_4WD, G_HORNS},
    /* The jetty has an end. */
    {"x-jetty", AREA_LOWLAND, 13.3f, 0, 1, 19, SH_STATIC, 0, 1.1f,
     {46, 1.4f, -358, 30, 1, -362},
     4,
     {{T_ONETEN, 0, ON_PATH, LIGHTS_DIPPED, 0, 0, 0, 0},
      {T_BOBCAT, 3, ON_PATH, LIGHTS_DIPPED, 0, 0, 12, 0},
      {T_FFR, 2, ON_PATH, LIGHTS_DIPPED, 0, 0, 24, 0},
      {T_CASCADE, 0, ON_PATH, LIGHTS_DIPPED, 0, 0, 36, 0}},
     PATH(p_jetty), 10, DRIVE_4WD, G_LEMMINGS},
    /* Three on the horn, and up. */
    {"x-rocket", AREA_DUNES, 17.2f, 0, 2, 10, SH_STATIC, 0, 1.15f,
     {3, 1.4f, -311, 0, 0, 0},
     8,
     {{T_GOOSE, 2, PARKED, LIGHTS_OFF, 20, -298, 0, 0},
      {T_EAGLE, 3, PARKED, LIGHTS_OFF, 8.7f, -293.9f, 110, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 11.3f, -289.8f, 133.3f, 0},
      {T_TACO, 0, PARKED, LIGHTS_OFF, 15.2f, -287, 156.7f, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_OFF, 20, -286, 180, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_OFF, 24.8f, -287, 203.3f, 0},
      {T_FFR, 2, PARKED, LIGHTS_OFF, 28.7f, -289.8f, 226.7f, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_OFF, 31.3f, -293.9f, 250, 0}},
     NULL, 0, 0, 0, G_ROCKET},
    /* A backflip off the razorback into the sunset. */
    {"x-flip", AREA_DUNES, 18.1f, 0, 4, 13, SH_HELI, 0, 1.15f,
     {26, 8, 0.12f, 2, 4, 9.5f, 2.7f},
     2,
     {{T_EAGLE, 3, 2, -1, -233, -31.5f, 44, 0},
      {T_GOOSE, 2, 2, -1, -233, -31.5f, 60, 0}},
     NULL, 0, 0, 0, G_FLIP},
    /* Follow the leader off the Diving Board. The last one thinks about it. */
    {"x-lemmings", AREA_MOAB, 17.6f, 0, 1, 24, SH_STATIC, 0, 1,
     {47, 97.623f, 229, 18, 0.074f, 210},
     8,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 21, 0, 0},
      {T_GOOSE, 2, ON_PATH, LIGHTS_OFF, 0, 18, 0, 0},
      {T_TACO, 0, ON_PATH, LIGHTS_OFF, 0, 15, 0, 0},
      {T_KESTREL, 1, ON_PATH, LIGHTS_OFF, 0, 12, 0, 0},
      {T_ONETEN, 0, ON_PATH, LIGHTS_OFF, 0, 9, 0, 0},
      {T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 6, 0, 0},
      {T_FFR, 2, ON_PATH, LIGHTS_OFF, 0, 3, 0, 0},
      {T_CASCADE, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_lemmings), 7.5f, DRIVE_4WD, G_LEMMINGS},
    {"x-lemmings-low", AREA_MOAB, 17.6f, 0, 1, 22, SH_STATIC, 2, 1.2f,
     {70, 3, 264, 0, 0, 0},
     8,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 21, 0, 0},
      {T_GOOSE, 2, ON_PATH, LIGHTS_OFF, 0, 18, 0, 0},
      {T_TACO, 0, ON_PATH, LIGHTS_OFF, 0, 15, 0, 0},
      {T_KESTREL, 1, ON_PATH, LIGHTS_OFF, 0, 12, 0, 0},
      {T_ONETEN, 0, ON_PATH, LIGHTS_OFF, 0, 9, 0, 0},
      {T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 6, 0, 0},
      {T_FFR, 2, ON_PATH, LIGHTS_OFF, 0, 3, 0, 0},
      {T_CASCADE, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_lemmings), 7.5f, DRIVE_4WD, G_LEMMINGS},
    {"x-lemmings-last", AREA_MOAB, 17.6f, 0, 8, 22, SH_FAR, 7, 0,
     {0},
     8,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 21, 0, 0},
      {T_GOOSE, 2, ON_PATH, LIGHTS_OFF, 0, 18, 0, 0},
      {T_TACO, 0, ON_PATH, LIGHTS_OFF, 0, 15, 0, 0},
      {T_KESTREL, 1, ON_PATH, LIGHTS_OFF, 0, 12, 0, 0},
      {T_ONETEN, 0, ON_PATH, LIGHTS_OFF, 0, 9, 0, 0},
      {T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 6, 0, 0},
      {T_FFR, 2, ON_PATH, LIGHTS_OFF, 0, 3, 0, 0},
      {T_CASCADE, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}},
     PATH(p_lemmings), 7.5f, DRIVE_4WD, G_LEMMINGS},
    /* Sunset on the Diving Board, and up they go. */
    {"x-balloon", AREA_MOAB, 18.4f, 0, 3, 17, SH_GAG, 0, 1.2f,
     {22, 203, 1.5f, 24, 6, 0.05f, 2.75f, 0.8f, 4},
     8,
     {{T_CASCADE, 0, PARKED, LIGHTS_DIPPED, 2, 203.5f, 0, 0},
      {T_EAGLE, 3, PARKED, LIGHTS_DIPPED, 8, 203.5f, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_DIPPED, 14, 203.5f, 0, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_DIPPED, 20, 203.5f, 0, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_DIPPED, 26, 203.5f, 0, 0},
      {T_TACO, 0, PARKED, LIGHTS_DIPPED, 32, 203.5f, 0, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_DIPPED, 38, 203.5f, 0, 0},
      {T_FFR, 2, PARKED, LIGHTS_DIPPED, 44, 203.5f, 0, 0}},
     NULL, 0, 0, 0, G_BALLOON},
    /* Eight at once, by the camp after dark. */
    {"x-donuts", AREA_DUNES, 21.5f, 0, 2, 14, SH_ORBIT, 0, 1.1f,
     {12, -286, 1, 42, 13, 0.08f, 2.4f},
     8,
     {{T_EAGLE, 3, PARKED, LIGHTS_DIPPED, 12, -266, 180, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_DIPPED, 26.1f, -271.9f, 225, 0},
      {T_GOOSE, 2, PARKED, LIGHTS_DIPPED, 32, -286, 270, 0},
      {T_TACO, 0, PARKED, LIGHTS_DIPPED, 26.1f, -300.1f, 315, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_DIPPED, 12, -306, 0, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_DIPPED, -2.1f, -300.1f, 45, 0},
      {T_FFR, 2, PARKED, LIGHTS_DIPPED, -8, -286, 90, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_DIPPED, -2.1f, -271.9f, 135, 0}},
     NULL, 0, 0, 0, G_DONUTS},
    /* Across the moon. */
    {"x-moon", AREA_DUNES, 20.3f, 0, 1.5f, 9, SH_GAG, 0, 0.42f,
     {110, 16, 2},
     1,
     {{T_GOOSE, 2, PARKED, LIGHTS_FULL, 0, -279, 0, 0}},
     NULL, 0, 0, 0, G_MOON},
    /* And back down by the fire. */
    {"x-landing", AREA_DUNES, 22, 0, 2, 12, SH_ORBIT, 0, 1.15f,
     {54, -283, 1.2f, 35, 6, 0.03f, 4},
     8,
     {{T_GOOSE, 2, PARKED, LIGHTS_DIPPED, 45, -285, 30, 0},
      {T_EAGLE, 3, PARKED, LIGHTS_DIPPED, 50.5f, -302.7f, 10, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_DIPPED, 44.8f, -300.7f, 27.5f, 0},
      {T_TACO, 0, PARKED, LIGHTS_DIPPED, 39.9f, -297.1f, 45, 0},
      {T_KESTREL, 1, PARKED, LIGHTS_DIPPED, 36.3f, -292.2f, 62.5f, 0},
      {T_BOBCAT, 3, PARKED, LIGHTS_DIPPED, 34.3f, -286.5f, 80, 0},
      {T_FFR, 2, PARKED, LIGHTS_DIPPED, 34.2f, -280.4f, 97.5f, 0},
      {T_CASCADE, 0, PARKED, LIGHTS_DIPPED, 35.9f, -274.5f, 115, 0}},
     NULL, 0, 0, 0, G_LANDING},

    /* ---- Damage: drivers in far too much of a hurry ---- */
    {"k-forest", AREA_CASCADE, 9, 0, 1, 12, SH_HELI, 0, 1.15f, {16, 6, -0.18f, -2.4f}, 1,
     {{T_CASCADE, 0, 1, LIGHTS_OFF, 160, -70, 0, 0}}, NULL, 0, 17, DRIVE_4WD, G_HOT},
    {"k-widow", AREA_CASCADE, 11, 0, 1, 12, SH_POST, 0, 0, {34, 7, 3}, 1,
     {{T_TACO, 0, 5, LIGHTS_OFF, 50.2f, 225.7f, 0, 0}}, NULL, 0, 13, DRIVE_4WD, G_HOT},
    {"k-knife", AREA_CASCADE, 11.5f, 0, 1, 12, SH_HELI, 0, 1.15f, {18, 7, 0.16f, 2.6f}, 1,
     {{T_FFR, 2, 2, LIGHTS_OFF, 72, 12, 18, 0}}, NULL, 0, 14, DRIVE_4WD, G_HOT},
    {"k-boulders", AREA_CASCADE, 11, 0, 1, 12, SH_HELI, 0, 1.15f, {13, 4.5f, 0.22f, 2.2f}, 1,
     {{T_BOBCAT, 3, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_boulders), 12, DRIVE_4WD, 0},
    {"k-tbone", AREA_CASCADE, 16, 0, 1, 10, SH_STATIC, 0, 1.0f, {22, 1.3f, -252, 0, 0, 0}, 2,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 12, -241.6f, 0, 0}}, PATH(p_tbone), 20, DRIVE_4WD, 0},
    {"k-pileup", AREA_CASCADE, 16, 0, 1, 12, SH_STATIC, 0, 1.05f, {24, 2.2f, -250, 0, 0, 0}, 4,
     {{T_KESTREL, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 12, -241.6f, 0, 0},
      {T_TACO, 0, ON_PATH, LIGHTS_OFF, 0, 0, 16, 0},
      {T_FFR, 2, ON_PATH, LIGHTS_OFF, 0, 0, 32, 0}}, PATH(p_tbone), 17, DRIVE_4WD, 0},
    {"k-fins", AREA_MOAB, 16.6f, 0, 1, 12, SH_POST, 0, 0, {48, 8, 3}, 1,
     {{T_BOBCAT, 3, 3, LIGHTS_OFF, 29, -91.5f, 44, 0}}, NULL, 0, 14, DRIVE_4WD, G_HOT},
    {"k-dorsal", AREA_MOAB, 17.8f, 0, 1, 12, SH_HELI, 0, 1.15f, {28, 10, 0.12f, 2}, 1,
     {{T_EAGLE, 3, 5, LIGHTS_OFF, -213.2f, 125.4f, 6, 0}}, NULL, 0, 13, DRIVE_4WD, G_HOT},
    {"k-catwalk", AREA_MOAB, 18, 0, 1, 12, SH_FAR, 0, 0, {0}, 1,
     {{T_BOBCAT, 3, 5, LIGHTS_OFF, -138.7f, 205.2f, 0, 0}}, NULL, 0, 14, DRIVE_4WD, G_HOT},
    {"k-whoops", AREA_DUNES, 17.6f, 0, 1, 12, SH_POST, 0, 0, {42, 6, 1.4f}, 1,
     {{T_GOOSE, 2, 3, LIGHTS_OFF, 25, -110, 40, 0}}, NULL, 0, 20, DRIVE_4WD, G_HOT},
    {"k-razor", AREA_DUNES, 17.9f, 0, 1, 12, SH_HELI, 0, 1.15f, {26, 8, 0.12f, 2}, 1,
     {{T_EAGLE, 3, 2, LIGHTS_OFF, -233, -31.5f, 44, 0}}, NULL, 0, 16, DRIVE_4WD, G_HOT},
    {"k-drop", AREA_DUNES, 18.1f, 0, 1, 12, SH_POST, 0, 0, {54, -12, 2}, 1,
     {{T_GOOSE, 2, 5, LIGHTS_OFF, -297.4f, 46.4f, 0, 0}}, NULL, 0, 16, DRIVE_4WD, G_HOT},
    {"k-stair", AREA_LOWLAND, 10.6f, 0, 1, 12, SH_POST, 0, 0, {34, 5, 1.6f}, 1,
     {{T_BOBCAT, 3, 2, LIGHTS_OFF, 30, -69, 34, 0}}, NULL, 0, 13, DRIVE_4WD, G_HOT},
    {"k-rake", AREA_LOWLAND, 16.5f, 0, 1, 12, SH_HELI, 0, 1.15f, {28, 6, 0, 1.9f}, 1,
     {{T_ONETEN, 0, 5, LIGHTS_OFF, 148, 57.3f, 0, 0}}, NULL, 0, 13, DRIVE_4WD, G_HOT},
    {"k-night", AREA_LOWLAND, 21.3f, 0, 1, 12, SH_FAR, 0, 0, {0}, 1,
     {{T_ONETEN, 0, 1, LIGHTS_DIPPED, 90, -66, 10, 0}}, NULL, 0, 15, DRIVE_4WD, G_HOT},
    {"k-headon", AREA_CASCADE, 9, 0, 1, 10, SH_HELI, 0, 1.15f, {15, 5, -0.2f, -2.4f}, 2,
     {{T_CASCADE, 0, 1, LIGHTS_OFF, 160, -70, 0, 0},
      {T_KESTREL, 0, 1, LIGHTS_OFF, 160, -70, -150, -1}}, NULL, 0, 16, DRIVE_4WD, G_HOT},
    {"k-headon-rain", AREA_LOWLAND, 13.4f, 0, 1, 10, SH_HELI, 0, 1.15f, {14, 4.5f, 0.2f, 2.4f}, 2,
     {{T_ONETEN, 0, 1, LIGHTS_DIPPED, 90, -66, 10, 0},
      {T_FFR, 2, 1, LIGHTS_DIPPED, 90, -66, -75, -1}}, NULL, 0, 14, DRIVE_4WD, G_HOT},
    {"k-headon-mesa", AREA_MOAB, 18, 0, 1, 10, SH_HELI, 0, 1.15f, {16, 6, 0.2f, 2.2f}, 2,
     {{T_BOBCAT, 3, 5, LIGHTS_OFF, -138.7f, 205.2f, 0, 0},
      {T_TACO, 0, 5, LIGHTS_OFF, -138.7f, 205.2f, -110, -1}}, NULL, 0, 14, DRIVE_4WD, G_HOT},
    {"k-tree-night", AREA_CASCADE, 21.5f, 0, 1, 9, SH_GAG, 0, 1.05f, {9, 1.5f, -9}, 1,
     {{T_TACO, 0, PARKED, LIGHTS_DIPPED, 100, -210, 60, 0}}, NULL, 0, 18, DRIVE_4WD, G_TREE},
    {"k-tree-day", AREA_CASCADE, 10, 0, 1, 9, SH_GAG, 0, 1.05f, {-10, 1.6f, 6}, 1,
     {{T_FFR, 2, PARKED, LIGHTS_OFF, 100, -210, 60, 0}}, NULL, 0, 18, DRIVE_4WD, G_TREE},
    {"k-cliff", AREA_MOAB, 17.6f, 0, 1, 16, SH_STATIC, 0, 1.2f, {70, 3, 264, 0, 0, 0}, 1,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 21, 0, 0}}, PATH(p_lemmings), 11, DRIVE_4WD, 0},
    {"k-cliff-top", AREA_MOAB, 17.6f, 0, 1, 16, SH_STATIC, 0, 1, {47, 97.623f, 229, 0, 0, 0}, 1,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 21, 0, 0}}, PATH(p_lemmings), 11, DRIVE_4WD, 0},
    {"k-limp", AREA_CASCADE, 16, 0, 5.5f, 8, SH_LEAD, 0, 1.1f, {7, -3.5f, 1.3f}, 2,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 12, -241.6f, 0, 0}}, PATH(p_tbone2), 20, DRIVE_4WD, 0},
    {"k-limp-side", AREA_CASCADE, 16, 0, 5.5f, 8, SH_SIDE, 0, 1.1f, {-5, 1.0f, 1}, 2,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 12, -241.6f, 0, 0}}, PATH(p_tbone2), 20, DRIVE_4WD, 0},
    {"k-tbone-close", AREA_CASCADE, 16, 0, 1, 8, SH_STATIC, 0, 1.0f, {17, 1.1f, -248, 12, 1.0f, -241.6f}, 2,
     {{T_EAGLE, 3, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 12, -241.6f, 0, 0}}, PATH(p_tbone), 20, DRIVE_4WD, 0},
    {"k-pileup-orbit", AREA_CASCADE, 17.5f, 0, 1, 14, SH_ORBIT, 0, 1.05f, {14, -240, 1.0f, 11, 2.2f, 0.25f, 2.6f}, 4,
     {{T_KESTREL, 0, ON_PATH, LIGHTS_OFF, 0, 0, 0, 0},
      {T_ONETEN, 0, PARKED, LIGHTS_OFF, 12, -241.6f, 0, 0},
      {T_TACO, 0, ON_PATH, LIGHTS_OFF, 0, 0, 16, 0},
      {T_FFR, 2, ON_PATH, LIGHTS_OFF, 0, 0, 32, 0}}, PATH(p_tbone), 17, DRIVE_4WD, 0},
    {"k-whoops-side", AREA_DUNES, 17.6f, 0, 1, 10, SH_HELI, 0, 1.15f, {11, 3.5f, 0.3f, 2.0f}, 1,
     {{T_GOOSE, 2, 3, LIGHTS_OFF, 25, -110, 40, 0}}, NULL, 0, 20, DRIVE_4WD, G_HOT},
    /* ---- Damage, second go: closer, and slowed as things come off ---- */
    {"q-sidehill-c", AREA_CASCADE, 10.5f, 0, 1, 14, SH_HELI, 0, 1.1f, {10, 3.5f, 0.35f, 2.2f}, 1,
     {{T_KESTREL, 0, PARKED, LIGHTS_OFF, 60, 40, 0, 0}}, NULL, 0, 14, DRIVE_4WD, G_SLOPE, 0.22f},
    {"q-sidehill-c2", AREA_CASCADE, 16.5f, 0, 1, 14, SH_HELI, 0, 1.1f, {9, 2.5f, -0.3f, -2.0f}, 1,
     {{T_ONETEN, 0, PARKED, LIGHTS_OFF, -120, -80, 0, 0}}, NULL, 0, 14, DRIVE_4WD, G_SLOPE, 0.22f},
    {"q-sidehill-l", AREA_LOWLAND, 13.4f, 0, 1, 14, SH_HELI, 0, 1.1f, {10, 3.5f, 0.35f, 2.2f}, 1,
     {{T_BOBCAT, 3, PARKED, LIGHTS_OFF, 60, 40, 0, 0}}, NULL, 0, 14, DRIVE_4WD, G_SLOPE, 0.22f},
    {"q-sidehill-m", AREA_MOAB, 17.2f, 0, 1, 14, SH_HELI, 0, 1.1f, {10, 3.5f, -0.3f, -2.2f}, 1,
     {{T_TACO, 0, PARKED, LIGHTS_OFF, -60, 60, 0, 0}}, NULL, 0, 14, DRIVE_4WD, G_SLOPE, 0.22f},
    {"q-sidehill-d", AREA_DUNES, 17.9f, 0, 1, 14, SH_HELI, 0, 1.1f, {10, 3.5f, 0.3f, 2.4f}, 1,
     {{T_EAGLE, 3, PARKED, LIGHTS_OFF, -100, 60, 0, 0}}, NULL, 0, 15, DRIVE_4WD, G_SLOPE, 0.22f},
    {"q-whoops", AREA_DUNES, 17.6f, 0, 1, 9, SH_HELI, 0, 1.1f, {9, 2.5f, 0.3f, 2.0f}, 1,
     {{T_GOOSE, 2, 3, LIGHTS_OFF, 25, -110, 40, 0}}, NULL, 0, 20, DRIVE_4WD, G_HOT, 0.22f},
    {"q-convoy", AREA_CASCADE, 9, 0, 1, 12, SH_HELI, 4, 1.1f, {11, 3.5f, 0.3f, 2.6f}, 5,
     {{T_ONETEN, 0, 1, -1, 160, -70, 0, 0},
      {T_GOOSE, 2, 1, -1, 160, -70, 12, 0},
      {T_BOBCAT, 3, 1, -1, 160, -70, 24, 0},
      {T_FFR, 2, 1, -1, 160, -70, 36, 0},
      {T_EAGLE, 3, 1, LIGHTS_OFF, 160, -70, -120, -1}}, NULL, 0, 16, DRIVE_4WD, G_HOT, 0.22f},
    {"q-convoy-lead", AREA_CASCADE, 9, 0, 1, 12, SH_LEAD, 0, 1.1f, {-9, 3, 2.2f}, 5,
     {{T_ONETEN, 0, 1, -1, 160, -70, 0, 0},
      {T_GOOSE, 2, 1, -1, 160, -70, 12, 0},
      {T_BOBCAT, 3, 1, -1, 160, -70, 24, 0},
      {T_FFR, 2, 1, -1, 160, -70, 36, 0},
      {T_EAGLE, 3, 1, LIGHTS_OFF, 160, -70, -120, -1}}, NULL, 0, 16, DRIVE_4WD, G_HOT, 0.22f},
    {"q-rearend", AREA_LOWLAND, 13.4f, 0, 1, 17, SH_HELI, 0, 1.1f, {10, 3, 0.3f, 1.2f}, 4,
     {{T_CASCADE, 0, 1, -1, 90, -66, 0, 0},
      {T_KESTREL, 0, 1, -1, 90, -66, -14, 0},
      {T_TACO, 0, 1, -1, 90, -66, -28, 0},
      {T_EAGLE, 3, 1, LIGHTS_DIPPED, 90, -66, 70, 0}}, NULL, 0, 15, DRIVE_4WD, G_HOT, 0.22f},
    {"q-tree-night", AREA_CASCADE, 21.5f, 0, 1, 8, SH_GAG, 0, 1.05f, {3.5f, 1.2f, -11}, 1,
     {{T_TACO, 0, PARKED, LIGHTS_DIPPED, 100, -210, 60, 0}}, NULL, 0, 18, DRIVE_4WD, G_TREE, 0.22f},
    {"q-tree-day", AREA_CASCADE, 16.5f, 0, 1, 8, SH_GAG, 0, 1.05f, {-3.5f, 1.2f, -11}, 1,
     {{T_ONETEN, 0, PARKED, LIGHTS_OFF, 100, -210, 60, 0}}, NULL, 0, 18, DRIVE_4WD, G_TREE, 0.22f},
    {"q-headon", AREA_CASCADE, 9, 0, 1, 9, SH_HELI, 0, 1.1f, {9, 2.8f, -0.35f, -2.2f}, 2,
     {{T_CASCADE, 0, 1, LIGHTS_OFF, 160, -70, 0, 0},
      {T_KESTREL, 0, 1, LIGHTS_OFF, 160, -70, -150, -1}}, NULL, 0, 16, DRIVE_4WD, G_HOT, 0.22f},
};
#define NSCENES (int)(sizeof(scenes) / sizeof(scenes[0]))

static struct {
    int scene;
    float t;            /* seconds since the scene started (go at its pre) */
    bool go;
    int late;           /* frames that ran long during the shot */
    const Scene *s;
    int pk[MAX_TRUCKS]; /* path trucks: the point each is heading for */
    const lvl_route_t *hot[MAX_TRUCKS];   /* G_HOT: the route each is tearing along, */
    int hot_dir[MAX_TRUCKS];              /* ...and which way */
    float scale, slow_t, slow_cool;       /* slow motion: the world's rate now, what is left of this one, and before the next */
    int lost;                             /* pieces and lamps gone, all trucks: one more is a moment to slow for */
    V3 eye, post;
    bool eye_set;
    float yaw, heli_a;
} fm;

/* ---- Placing trucks ---- */

/* The ground, or the water over it. */
static float ground(float x, float z) {
    return fmaxf(terrain_height(x, z), water_at(x, z));
}

static int nearest_pt(const lvl_route_t *r, float x, float z) {
    int best = 0;
    float bd = 1e18f;
    for(int i = 0; i < r->n; i++) {
        float dx = r->pts[i].x - x, dz = r->pts[i].z - z, d = dx * dx + dz * dz;
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* The point d metres along the route from point i (back down it for a
   negative d), and which point that is. */
static V3 route_walk(const lvl_route_t *r, int i, float d, int *at) {
    int step = d < 0 ? -1 : 1;
    d = fabsf(d);
    V3 p = v3(r->pts[i].x, r->pts[i].y, r->pts[i].z);
    while(i + step >= 0 && i + step < r->n) {
        V3 q = v3(r->pts[i + step].x, r->pts[i + step].y, r->pts[i + step].z);
        float seg = vlen(vsub(q, p));
        if(seg >= d) break;
        d -= seg;
        p = q;
        i += step;
    }
    if(at) *at = i;
    return p;
}

static float route_heading(const lvl_route_t *r, int i) {
    int j = i + 1 < r->n ? i + 1 : i, k = j == i ? i - 1 : i;
    return atan2f(r->pts[j].x - r->pts[k].x, r->pts[j].z - r->pts[k].z);
}

/* ---- Stunts ---- */

static struct {
    float cue[MAX_TRUCKS];
    bool done[MAX_TRUCKS];
    V3 eye, dir;
    float hold;
} gg;

static void gag_begin(const Scene *s) {
    memset(&gg, 0, sizeof(gg));
    switch(s->gag) {
        case G_RAIN:
            /* Out of sight until each one's moment to fall. */
            for(int i = 0; i < s->n; i++) {
                trucks[i].absent = true;
                gg.cue[i] = s->pre + 2.2f + 0.55f * i;
            }
            break;
        case G_LOWG:
            film_g = 0.24f;
            break;
        case G_DONUTS:
            for(int i = 0; i < s->n; i++) trucks[i].mode = DRIVE_2WD;
            break;
        case G_LANDING:
            /* The first truck is still on its way down from somewhere. */
            trucks[0].absent = true;
            break;
        case G_BREAK: {
            /* On its roof. */
            Truck *t = &trucks[0];
            t->ax[0] = vscale(t->ax[0], -1.0f);
            t->ax[1] = vscale(t->ax[1], -1.0f);
            t->pos.y += 1.5f;
            break;
        }
        case G_BALLOON:
            for(int i = 0; i < s->n; i++) gg.cue[i] = s->pre + 2.5f + 0.9f * ((i * 5) % s->n);
            break;
        case G_MOON: {
            /* A camera on the ground looking at the moon, and the truck
               carried across its face. */
            const Truck *t = &trucks[0];
            gg.eye = v3(t->pos.x, ground(t->pos.x, t->pos.z) + 1.6f, t->pos.z);
            gg.dir = tod.moon;
            film_g = 0;
            printf("RL FILM moon %.0f degrees up\n", (double)(tod.moon_el * 57.3f));
            break;
        }
        case G_HOT:
            for(int i = 0; i < s->n; i++) {
                const FilmTruck *f = &s->t[i];
                fm.hot[i] = NULL;
                if(f->route < 0 || f->lights < 0) continue;   /* (lights -1: a CPU driver, minding the trail) */
                const lvl_route_t *r = &lvl_routes[f->route >= 10 ? 0 : f->route];
                int at;
                route_walk(r, nearest_pt(r, f->x, f->z), -f->back, &at);
                fm.hot[i] = r;
                fm.hot_dir[i] = f->delay < 0 ? -1 : 1;
                fm.pk[i] = at + fm.hot_dir[i];
                if(f->delay < 0) {
                    uint8_t lights = trucks[i].lights;
                    truck_place(&trucks[i], v3(r->pts[at].x, 0, r->pts[at].z), route_heading(r, at) + 3.14159265f);
                    trucks[i].lights = lights;
                }
                trucks[i].mode = s->path_mode;
                trucks[i].cpu = false;
            }
            break;
        case G_SLOPE: {
            Truck *t = &trucks[0];
            const FilmTruck *f = &s->t[0];
            /* The spot whose fall line drops furthest over 40 m without
               being a cliff or ending in water. */
            V3 best = v3(f->x, 0, f->z), bdn = v3(1, 0, 0);
            float bdrop = 0;
            for(float x = f->x - 130; x <= f->x + 130; x += 6)
                for(float z = f->z - 130; z <= f->z + 130; z += 6) {
                    if(fabsf(x) > 0.46f * LEVEL_SIZE || fabsf(z) > 0.46f * LEVEL_SIZE) continue;
                    float h = terrain_height(x, z);
                    V3 g = v3(terrain_height(x + 3, z) - terrain_height(x - 3, z), 0, terrain_height(x, z + 3) - terrain_height(x, z - 3));
                    float gl = vlen(g);
                    if(gl < 2.4f || gl > 4.8f) continue;       /* 0.4 to 0.8 of a metre down a metre */
                    V3 dn = vscale(g, -1.0f / gl);
                    float drop = 0, last = h;
                    bool ok = true;
                    for(int k = 1; k <= 4 && ok; k++) {
                        V3 q = vmad(v3(x, 0, z), dn, 10.0f * k);
                        float hq = terrain_height(q.x, q.z);
                        if(hq > last - 2.5f || hq < last - 9.5f || water_at(q.x, q.z) > hq) ok = false;
                        last = hq;
                    }
                    /* (and as steep for the run-in, 30 m back along the hill) */
                    V3 c = v3(dn.z, 0, -dn.x), back = vmad(v3(x, 0, z), c, -30.0f);
                    if(terrain_height(back.x, back.z) - terrain_height(back.x + dn.x * 10, back.z + dn.z * 10) < 3.0f) ok = false;
                    if(water_at(back.x, back.z) > terrain_height(back.x, back.z)) ok = false;
                    drop = h - last;
                    if(ok && drop > bdrop) bdrop = drop, best = v3(x, 0, z), bdn = dn;
                }
            V3 c = v3(bdn.z, 0, -bdn.x);
            uint8_t lights = t->lights;
            truck_place(t, vmad(best, c, -30.0f), atan2f(c.x, c.z));
            t->lights = lights;
            t->mode = s->path_mode;
            t->cpu = false;
            gg.dir = vmad(best, c, 300.0f);
            printf("RL FILM slope at %.0f, %.0f drops %.0f m in 40\n", (double)best.x, (double)best.z, (double)bdrop);
            break;
        }
        case G_TREE: {
            Truck *t = &trucks[0];
            const FilmTruck *f = &s->t[0];
            float h = f->back * (3.14159265f / 180.0f);
            V3 fw = heading_fwd(h), aim = vmad(v3(f->x, 0, f->z), fw, 45.0f);
            int best = 0;
            float bd = 1e18f;
            for(int k = 0; k < LVL_TREES; k++) {
                float dx = lvl_trees[k].x - aim.x, dz = lvl_trees[k].z - aim.z;
                if(dx * dx + dz * dz < bd) bd = dx * dx + dz * dz, best = k;
            }
            V3 tree = v3(lvl_trees[best].x, 0, lvl_trees[best].z);
            uint8_t lights = t->lights;
            truck_place(t, vmad(tree, fw, -45.0f), h);
            t->lights = lights;
            t->mode = s->path_mode;
            t->cpu = false;
            V3 side = v3(fw.z, 0, -fw.x);
            gg.dir = tree;
            gg.eye = vadd(vmad(vmad(tree, side, s->a[0]), fw, -s->a[2]), v3(0, 0, 0));
            gg.eye.y = ground(gg.eye.x, gg.eye.z) + s->a[1];
            printf("RL FILM tree %d at %.1f, %.1f\n", best, (double)tree.x, (double)tree.z);
            break;
        }
        default:
            break;
    }
}

/* After the drivers have had their say and before the physics. */
static void gag_update(const Scene *s, float dt) {
    float since = fm.t - s->pre;
    switch(s->gag) {
        case G_RAIN:
            for(int i = 0; i < s->n; i++) {
                Truck *t = &trucks[i];
                if(!gg.done[i] && fm.t >= gg.cue[i]) {
                    gg.done[i] = true;
                    t->absent = false;
                    t->pos.y += 36.0f + 8.0f * (i % 3);
                    t->vel = v3(0, -4.0f, 0);
                    t->w = v3(i & 1 ? 0.7f : -0.5f, 0, 0.35f * (float)(i % 3 - 1));
                }
                t->horn = !t->absent && t->airborne_t > 0.25f;
            }
            break;
        case G_TUMBLE: {
            /* Head over heels along the scene's path, for ever. */
            Truck *t = &trucks[0];
            V3 d = vnorm(v3(s->path[1][0] - s->path[0][0], 0, s->path[1][1] - s->path[0][1]));
            t->vel.x = d.x * s->path_speed;
            t->vel.z = d.z * s->path_speed;
            t->w = vscale(vcross(v3(0, 1, 0), d), 5.5f);
            if(t->pos.y - ground(t->pos.x, t->pos.z) < 1.5f && t->vel.y < 2.5f) t->vel.y = 4.2f;
            if(t->vel.y > 4.5f) t->vel.y = 4.5f;   /* (the ground throws a spinning truck a long way up) */
            t->in.throttle = t->in.brake = 0;
            t->in.handbrake = false;
            break;
        }
        case G_LEMMINGS: {
            /* Every one off the edge with its horn going, but the last,
               which stops at the brink and thinks about it. */
            for(int i = 0; i < s->n; i++) trucks[i].horn = trucks[i].airborne_t > 0.35f;
            Truck *t = &trucks[s->n - 1];
            V3 brink = v3(s->path[s->npath - 2][0], 0, s->path[s->npath - 2][1]);
            float dx = brink.x - t->pos.x, dz = brink.z - t->pos.z;
            if(!gg.done[0] && dx * dx + dz * dz < 8.0f * 8.0f) {
                gg.done[0] = true;
                gg.hold = 0;
            }
            if(gg.done[0] && gg.hold < 5.0f) {
                gg.hold += dt;
                t->in.throttle = 0;
                t->in.brake = t->speed > 0.8f ? 1.0f : 0;
                t->in.handbrake = t->speed <= 0.8f;
                t->horn = (gg.hold > 2.2f && gg.hold < 2.45f) || (gg.hold > 2.75f && gg.hold < 3.0f);
            }
            break;
        }
        case G_HORNS:
            /* Leaning on their horns, each in its own time. */
            for(int i = 0; i < s->n; i++) trucks[i].horn = since > 0.5f && fmodf(fm.t + 0.71f * i, 2.3f + 0.2f * i) < 0.3f;
            break;
        case G_ROCKET: {
            /* Three on the horn and straight up, turning as it goes. */
            Truck *t = &trucks[0];
            t->horn = since > 1.0f && since < 2.9f && fmodf(since - 1.0f, 0.7f) < 0.28f;
            if(since > 3.3f) {
                t->vel = v3(0, fminf(4.0f + 26.0f * (since - 3.3f), 70.0f), 0);
                t->w = v3(0, 2.2f, 0);
                t->in.throttle = 1.0f;
                t->in.handbrake = false;
            }
            for(int i = 1; i < s->n; i++) trucks[i].horn = since > 4.6f && fmodf(since + 0.4f * i, 1.6f) < 0.25f;
            break;
        }
        case G_LANDING: {
            /* It comes back, and the others say so. */
            Truck *t = &trucks[0];
            if(!gg.done[0] && since >= 2.5f) {
                gg.done[0] = true;
                t->absent = false;
                t->pos.y += 70.0f;
                t->vel = v3(0, -12.0f, 0);
                t->w = v3(0.4f, 1.0f, 0.2f);
            }
            if(gg.done[0]) {
                t->horn = t->airborne_t > 0.25f;
                if(since > 4.0f && t->airborne_t < 0.05f) gg.hold += dt;
            }
            for(int i = 1; i < s->n; i++) trucks[i].horn = gg.hold > 1.2f && fmodf(gg.hold + 0.37f * i, 1.3f) < 0.3f;
            break;
        }
        case G_MOON: {
            Truck *t = &trucks[0];
            V3 r = vnorm(vcross(v3(0, 1, 0), gg.dir));
            float u = since / s->secs * 2.0f - 1.0f;
            float span = s->a[1], far = s->a[0];
            V3 up = vnorm(vcross(gg.dir, r));
            V3 p = vadd(vmad(gg.eye, gg.dir, far), vadd(vscale(r, u * span), vscale(up, (1.0f - u * u) * s->a[2])));
            t->pos = p;
            t->vel = vscale(r, 2.0f * span / s->secs);
            t->w = v3(0, 0, 0);
            /* Nose a little up, as if it had just left a ramp. */
            V3 f = vnorm(vadd(r, vscale(up, 0.22f - 0.3f * u)));
            t->ax[2] = f;
            t->ax[0] = vnorm(vcross(up, f));
            t->ax[1] = vcross(f, t->ax[0]);
            t->in.throttle = 1.0f;
            t->in.brake = 0;
            t->in.handbrake = false;
            break;
        }
        case G_DONUTS:
            for(int i = 0; i < s->n; i++) {
                Truck *t = &trucks[i];
                t->mode = DRIVE_2WD;
                if(since < 1.0f) continue;
                t->in.steer = 1.0f;
                t->in.throttle = 0.9f;
                t->in.brake = 0;
                t->in.handbrake = false;
                t->horn = fmodf(since + 0.3f * i, 4.0f) > 3.7f;
            }
            break;
        case G_BREAK: {
            /* Spinning on its roof, and the others keeping time. */
            Truck *t = &trucks[0];
            t->w = v3(0, s->a[8], 0);
            t->vel.x *= 0.9f;
            t->vel.z *= 0.9f;
            t->in.throttle = 0.5f;
            t->in.handbrake = false;
            float beat = fm.t * 2.4f;
            for(int i = 1; i < s->n; i++) trucks[i].horn = since > 0.5f && ((int)beat + i) % 2 == 0 && beat - floorf(beat) < 0.4f;
            break;
        }
        case G_BALLOON:
            for(int i = 0; i < s->n; i++) {
                Truck *t = &trucks[i];
                float up = fm.t - gg.cue[i];
                if(up < 0) continue;
                t->vel = v3(0.25f * sinf(i * 2.1f), fminf(0.4f + 0.8f * up, 3.4f), 0.2f * cosf(i * 1.3f));
                t->w = v3(0.1f * sinf(i * 1.7f), i & 1 ? 0.45f : -0.45f, 0.08f * cosf(i * 0.9f));
                t->horn = up < 0.3f;
            }
            break;
        case G_FLIP:
            for(int i = 0; i < s->n; i++) {
                Truck *t = &trucks[i];
                if(!gg.done[i] && since >= s->a[4] + 1.3f * i) {
                    gg.done[i] = true;
                    t->vel.y = s->a[5];
                    t->w = vscale(t->ax[0], -s->a[6]);
                    film_g = 0.8f;
                }
            }
            break;
        default:
            break;
    }
}

static void gag_camera(const Scene *s, float fov) {
    if(s->gag == G_MOON) camera_set(gg.eye, vmad(gg.eye, gg.dir, 100.0f), fov);
    else if(s->gag == G_TREE) {
        if(!fm.eye_set) fm.post = trucks[0].pos, fm.eye_set = true;
        fm.post = vlerp(fm.post, vadd(trucks[0].pos, v3(0, 0.8f, 0)), 0.12f);
        camera_set(gg.eye, fm.post, fov);
    }
    else {
        /* An orbit (SH_ORBIT's numbers) whose gaze climbs, a[7] metres a
           second, once the shot has run a[8] seconds. */
        const float *a = s->a;
        float since = fmaxf(fm.t - s->pre, 0);
        float b = a[6] + a[5] * since;
        float g = ground(a[0], a[1]);
        V3 eye = v3(a[0] + sinf(b) * a[3], g + a[4], a[1] + cosf(b) * a[3]);
        float eg = ground(eye.x, eye.z) + 1.0f;
        if(eye.y < eg) eye.y = eg;
        camera_set(eye, v3(a[0], g + a[2] + a[7] * fmaxf(since - a[8], 0), a[1]), fov);
    }
}

static void scene_begin(int n) {
    const Scene *s = &scenes[n];
    if(s->area != game.area) game_load_area(s->area);
    fm.scene = n;
    fm.s = s;
    fm.t = 0;
    fm.go = false;
    fm.late = 0;
    fm.scale = 1.0f;
    fm.slow_t = fm.slow_cool = 0;
    fm.lost = 0;
    fm.eye_set = false;
    game.ntrucks = s->n;
    film_g = 1.0f;
    for(int i = 0; i < MAX_TRUCKS; i++) trucks[i].remote = trucks[i].absent = false;
    for(int i = 0; i < s->n; i++) {
        const FilmTruck *f = &s->t[i];
        Truck *t = &trucks[i];
        t->model = f->model;
        t->livery = f->livery;
        t->tcs_off = false;
        t->horn = false;
        if(f->route == PARKED) {
            truck_place(t, v3(f->x, 0, f->z), f->back * (3.14159265f / 180.0f));
            truck_set_mode(t, DRIVE_4WD);
        }
        else if(f->route == ON_PATH) {
            int k0 = (int)f->z < s->npath - 1 ? (int)f->z : s->npath - 2;
            V3 d = vnorm(v3(s->path[k0 + 1][0] - s->path[k0][0], 0, s->path[k0 + 1][1] - s->path[k0][1]));
            V3 at = vadd(vmad(v3(s->path[k0][0], 0, s->path[k0][1]), d, -f->back), vscale(v3(d.z, 0, -d.x), f->x));
            truck_place(t, at, atan2f(d.x, d.z));
            truck_set_mode(t, s->path_mode);
            fm.pk[i] = k0 + 1;
        }
        else {
            /* 11, 12, 13: on the way in from the start (route 0), bound
               for route 1, 2 or 3 at the fork. */
            int on = f->route >= 10 ? 0 : f->route;
            const lvl_route_t *r = &lvl_routes[on];
            int at;
            route_walk(r, nearest_pt(r, f->x, f->z), -f->back, &at);
            if(f->x == 0 && f->z == 0) at = 0;
            truck_place(t, v3(r->pts[at].x, 0, r->pts[at].z), route_heading(r, at));
            int plan = f->route >= 10 ? f->route - 10 : on >= 1 && on <= 3 ? on : 1 + i % 3;
            ai_start_on(i, plan, on, at, f->delay);
            if(i == 0 && s->shot == SH_POST) {
                int pa;
                V3 p = route_walk(r, at, s->a[0], &pa);
                float h = route_heading(r, pa);
                V3 side = v3(cosf(h), 0, -sinf(h));
                fm.post = vmad(p, side, s->a[1]);
            }
        }
        t->cpu = f->lights < 0;
        t->lights = f->lights < 0 ? LIGHTS_OFF : (uint8_t)f->lights;
    }
    if(s->shot == SH_POST) fm.post.y = ground(fm.post.x, fm.post.z) + s->a[2];
    if(s->n > 0) fm.yaw = truck_heading(&trucks[s->watch]);
    fm.heli_a = fm.yaw + s->a[3];
    tod_set_hour(s->hour);
    tod_relight_all();
    damage_reset();
    game.damage_off = false;
    gag_begin(s);
    fx_clear();
    deer_reset();
    birds_reset();
    camera_reset();
    if(s->n > 0) {
        audio_drive_start();
        if(s->watch) audio_listen(s->watch);
    }
    else audio_drive_stop();
    printf("RL FILM scene %d %s\n", n, s->name);
}

/* -DRL_FILM_PICK=3,7,8 films only those scenes, in that order. */
#ifdef RL_FILM_PICK
static const int pick[] = {RL_FILM_PICK};
#define NPICK (int)(sizeof(pick) / sizeof(pick[0]))
#endif
static int pick_i;

static int scene_at(int i) {
#ifdef RL_FILM_PICK
    return i < NPICK ? pick[i] : NSCENES;
#else
    return i;
#endif
}

void film_start(void) {
    game.paused = game.photo = false;
    game.cam_mode = 0;
    game_set_state(ST_DRIVE);
    pick_i = 0;
    scene_begin(scene_at(0));
}

/* ---- Driving a path ---- */

static void path_drive(int i, float dt) {
    const Scene *s = fm.s;
    Truck *t = &trucks[i];
    TruckInput *in = &t->in;
    int *k = &fm.pk[i];
    float off = s->t[i].x;
    V3 tgt = t->pos;
    for(;;) {
        if(*k >= s->npath) break;
        /* The path's point, this truck's distance to the right of it. */
        V3 seg = vnorm(v3(s->path[*k][0] - s->path[*k - 1][0], 0, s->path[*k][1] - s->path[*k - 1][1]));
        tgt = vadd(v3(s->path[*k][0], 0, s->path[*k][1]), vscale(v3(seg.z, 0, -seg.x), off));
        float dx = tgt.x - t->pos.x, dz = tgt.z - t->pos.z;
        /* Past it, or near: on to the next. */
        if(dx * dx + dz * dz < 5.0f * 5.0f || vdot(seg, v3(dx, 0, dz)) < 0) (*k)++;
        else break;
    }
    if(*k >= s->npath || fm.t < s->t[i].delay) {
        in->steer = 0;
        in->throttle = 0;
        in->brake = t->speed > 1.0f ? 0.7f : 0;
        in->handbrake = t->speed <= 1.0f;
        return;
    }
    V3 d = vsub(tgt, t->pos);
    d.y = 0;
    float lx = vdot(d, t->ax[0]), lz = vdot(d, t->ax[2]);
    const TruckSpec *sp = &t->spec;
    float wb = sp->wheel[WFL].z - sp->wheel[WRL].z;
    float curv = 2.0f * lx / fmaxf(lx * lx + lz * lz, 1.0f);
    float steer = atanf(wb * curv) / sp->max_steer;
    if(lz < 0.5f) steer = lx >= 0 ? 1.0f : -1.0f;
    float err = s->path_speed - t->speed;
    in->steer = clampf(steer, -1, 1);
    in->throttle = clampf(err * 0.45f + 0.2f, 0, 1);
    in->brake = clampf(-err * 0.3f, 0, 1);
    in->handbrake = false;
    (void)dt;
}

/* A route taken flat out: the trail's own points, chased at the scene's
   speed with no thought for what is beside it. */
static void hot_drive(int i) {
    const Scene *s = fm.s;
    const lvl_route_t *r = fm.hot[i];
    Truck *t = &trucks[i];
    TruckInput *in = &t->in;
    int *k = &fm.pk[i];
    int dir = fm.hot_dir[i];
    while(*k > 0 && *k < r->n - 1) {
        float dx = r->pts[*k].x - t->pos.x, dz = r->pts[*k].z - t->pos.z;
        float sx = r->pts[*k].x - r->pts[*k - dir].x, sz = r->pts[*k].z - r->pts[*k - dir].z;
        if(dx * dx + dz * dz < 7.0f * 7.0f || sx * dx + sz * dz < 0) *k += dir;
        else break;
    }
    V3 d = v3(r->pts[*k].x - t->pos.x, 0, r->pts[*k].z - t->pos.z);
    float lx = vdot(d, t->ax[0]), lz = vdot(d, t->ax[2]);
    float wb = t->spec.wheel[WFL].z - t->spec.wheel[WRL].z;
    float steer = atanf(wb * 2.0f * lx / fmaxf(lx * lx + lz * lz, 1.0f)) / t->spec.max_steer;
    if(lz < 0.5f) steer = lx >= 0 ? 1.0f : -1.0f;
    float err = s->path_speed - t->speed;
    in->steer = clampf(steer, -1, 1);
    in->throttle = fm.t < s->t[i].delay ? 0 : clampf(err * 0.45f + 0.2f, 0, 1);
    in->brake = clampf(-err * 0.15f, 0, 1);
    in->handbrake = false;
}

/* Straight at the tree, and on past where it stands. */
static void tree_drive(void) {
    const Scene *s = fm.s;
    Truck *t = &trucks[0];
    V3 d = v3(gg.dir.x - t->pos.x, 0, gg.dir.z - t->pos.z);
    float lx = vdot(d, t->ax[0]), lz = vdot(d, t->ax[2]);
    t->in.steer = lz > 3.0f ? clampf(2.0f * lx / lz, -1, 1) : 0;
    t->in.throttle = clampf((s->path_speed - t->speed) * 0.45f + 0.2f, 0, 1);
    t->in.brake = 0;
    t->in.handbrake = false;
}

/* ---- The camera ---- */

static void eye_to(V3 want, float k, float dt) {
    float g = ground(want.x, want.z) + 0.6f;
    if(want.y < g) want.y = g;
    if(!fm.eye_set) {
        fm.eye = want;
        fm.eye_set = true;
    }
    fm.eye = vlerp(fm.eye, want, smooth_k(k, dt));
    g = ground(fm.eye.x, fm.eye.z) + 0.5f;
    if(fm.eye.y < g) fm.eye.y = g;
}

static V3 agl(float x, float h, float z) {
    return v3(x, ground(x, z) + h, z);
}

static void film_camera(float dt) {
    const Scene *s = fm.s;
    const float *a = s->a;
    float fov = s->fov > 0 ? s->fov : 1.2f;
    const Truck *t = s->n > 0 ? &trucks[s->watch] : NULL;
    V3 focus = t ? vadd(t->pos, v3(0, 0.8f, 0)) : v3(0, 0, 0);
    if(t) fm.yaw += wrap_angle(truck_heading(t) - fm.yaw) * smooth_k(2.0f, dt);
    V3 fwd = heading_fwd(fm.yaw), side = v3(fwd.z, 0, -fwd.x);
    switch(s->shot) {
        case SH_CHASE:
        case SH_FAR:
        case SH_BUMPER:
            game.cam_mode = s->shot == SH_CHASE ? 0 : s->shot == SH_FAR ? 1 : 2;
            camera_follow(t, dt);
            break;
        case SH_HELI: {
            fm.heli_a += dt * a[2];
            float k = clampf((fm.t - s->pre) / s->secs, 0, 1);
            float h = a[1] * (1.0f - 0.3f * k);
            eye_to(vadd(t->pos, v3(sinf(fm.heli_a) * a[0], h, cosf(fm.heli_a) * a[0])), 4.0f, dt);
            camera_set(fm.eye, vadd(vmad(focus, fwd, 3.0f), v3(0, 0.4f, 0)), fov);
            break;
        }
        case SH_POST: {
            float d = vlen(vsub(focus, fm.post));
            camera_set(fm.post, focus, clampf(1.45f - d * 0.02f, 0.7f, 1.3f));
            break;
        }
        case SH_STATIC: {
            V3 eye = agl(a[0], a[1], a[2]);
            if(a[6] != 0 || a[8] != 0) {
                float k = clampf((fm.t - s->pre) / s->secs, 0, 1);
                k = k * k * (3.0f - 2.0f * k);
                eye = vlerp(eye, agl(a[6], a[7], a[8]), k);
            }
            V3 look;
            if(a[3] == 0 && a[5] == 0 && t) {
                if(!fm.eye_set) {
                    fm.post = focus;
                    fm.eye_set = true;
                }
                fm.post = vlerp(fm.post, focus, smooth_k(5.0f, dt));
                look = fm.post;
            }
            else look = agl(a[3], a[4], a[5]);
            camera_set(eye, look, fov);
            break;
        }
        case SH_LEAD:
            eye_to(vadd(vadd(vmad(t->pos, fwd, a[0]), vscale(side, a[1])), v3(0, a[2], 0)), 3.0f, dt);
            camera_set(fm.eye, vadd(focus, v3(0, 0.2f, 0)), fov);
            break;
        case SH_SIDE:
            eye_to(vadd(vadd(vmad(t->pos, side, a[0]), vscale(fwd, a[2])), v3(0, a[1], 0)), 3.0f, dt);
            camera_set(fm.eye, vmad(focus, fwd, 2.5f), fov);
            break;
        case SH_GAG:
            gag_camera(s, fov);
            break;
        case SH_ORBIT: {
            float b = a[6] + a[5] * fmaxf(fm.t - s->pre, 0);
            float g = ground(a[0], a[1]);
            V3 eye = v3(a[0] + sinf(b) * a[3], g + a[4], a[1] + cosf(b) * a[3]);
            float eg = ground(eye.x, eye.z) + 1.0f;
            if(eye.y < eg) eye.y = eg;
            camera_set(eye, v3(a[0], g + a[2], a[1]), fov);
            break;
        }
    }
}

void film_update(float dt) {
    const Scene *s = fm.s;
    if(fm.scene >= NSCENES) return;
    /* Slow motion for a moment when something comes off a truck. */
    float real = dt;
    if(s->slow > 0) {
        int lost = 0;
        for(int i = 0; i < game.ntrucks; i++)
            lost += __builtin_popcount(damage_look(i)->pieces) + __builtin_popcount(damage_look(i)->lamps);
        fm.slow_cool -= real;
        if(fm.go && lost > fm.lost && fm.slow_cool <= 0) fm.slow_t = 1.5f, fm.slow_cool = 3.5f;
        fm.lost = lost;
        fm.slow_t -= real;
        fm.scale += ((fm.slow_t > 0 ? s->slow : 1.0f) - fm.scale) * smooth_k(fm.slow_t > 0 ? 30.0f : 6.0f, real);
        dt *= fm.scale;
    }
    for(int i = 0; i < game.ntrucks; i++) {
        if((s->gag == G_TREE || s->gag == G_SLOPE) && i == 0) tree_drive();
        else if(s->t[i].route == PARKED) {
            TruckInput *in = &trucks[i].in;
            /* (the handbrake: the footbrake held at a standstill selects reverse) */
            in->steer = in->throttle = in->brake = 0;
            in->handbrake = true;
        }
        else if(s->t[i].route == ON_PATH) path_drive(i, dt);
        else if(s->gag == G_HOT && fm.hot[i]) hot_drive(i);
        else ai_drive(i, dt);
    }
    gag_update(s, dt);
    for(int i = 0; i < game.ntrucks; i++)
        if(!trucks[i].absent) truck_update(&trucks[i], dt);
    damage_update(dt);
    game.clock += dt;
    fx_update(dt);
#ifdef RL_FILM_SCOUT
    /* -DRL_FILM_SCOUT: a picture every two seconds of a shot and what the
       watched truck has lost, for choosing where to cut. */
    {
        static int shots;
        float since = fm.t - s->pre;
        if(fm.go && s->n && since >= shots * 2.0f + 1.0f) {
            const DamageLook *d = damage_look(s->watch);
            printf("RL FILM scout %s t=%.0f dmg %d%% pieces %03x lamps %x up %.2f mph %.0f\n", s->name, (double)since, d->percent,
                   d->pieces, d->lamps, (double)trucks[s->watch].ax[1].y, (double)(trucks[s->watch].speed * MPS_TO_MPH));
#ifdef RL_FILM_SHOTS
            qa_shot("%s-%d", s->name, shots);
#endif
            shots++;
        }
        if(!fm.go) shots = 0;
    }
#endif
    /* A time-lapse runs the clock on (tod_update relights as it can). */
    if(s->rate > 0 && fm.go) tod.hour = fmodf(tod.hour + s->rate * dt, 24.0f);
    fm.t += real;
    film_camera(real);
    if(!fm.go && fm.t >= s->pre) {
        fm.go = true;
        printf("RL FILM go %d %s\n", fm.scene, s->name);
    }
    if(fm.go && eng_frame.steps > 1) fm.late++;
    if(fm.t >= s->pre + s->secs) {
        printf("RL FILM cut %d %s late %d\n", fm.scene, s->name, fm.late);
        int next = scene_at(++pick_i);
        if(next >= NSCENES) {
            printf("RL FILM done\n");
#ifdef RL_FILM_SHOTS
            printf("RL AUTOTEST: PASS\n");
#endif
            fm.scene = NSCENES;
            return;
        }
        scene_begin(next);
    }
}
#endif
