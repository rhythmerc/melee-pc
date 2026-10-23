/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_scene.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"
#include "pc/xr_place.h"

#include <aurora/xr.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <melee/gm/gmscene.h>
#include <melee/gr/ground.h>
#include <melee/gr/stage.h>
#include <melee/mp/forward.h>
#include <melee/mp/mplib.h>
#include <melee/mp/types.h>
#include <melee/ft/ftlib.h>
#include <melee/pl/player.h>
#include <melee/cm/camera.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/generator.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/lobj.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stage parts shown in 3D regardless of layer, or hidden regardless of it.
 * Keyed by GrKind (gr/forward.h) and the part's map_id (its index in the
 * stage's part table). A rule can name one joint of the part instead, by
 * its index in a depth-first walk of the part's joint tree; it then applies
 * to that joint and everything under it, or to one of its meshes (DObjs,
 * counted from 0). Later rules win. Extend with MELEE_XR_PARTS while
 * surveying stages: "16:1,-16:4,-16:1/7,-16:2/2.1" shows part 1, hides part
 * 4, hides joint 7 of part 1, and hides mesh 1 of joint 2 of part 2, all of
 * stage 16. MELEE_XR_JOINT_LOG lists every part's joints once. */
typedef struct {
    int grkind;
    int map_id;
    int jobj; /* -1: the whole part */
    int dobj; /* -1: the whole joint */
    bool show;
} PartRule;

#define GRKIND_PSTADIUM 0x10 /* Gr_Kind_PStadium */
#define PSTYPE_DISPLAY 1     /* PsType_Display: the stadium's big screen */

static const PartRule s_builtin_rules[] = {
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, -1, -1, true},
    /* The big screen's part also carries the city: joint 11 is the 3D
     * buildings, joint 12 the sky, mountains and city lights. */
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, 11, -1, false},
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, 12, -1, false},
    /* Part 2 joint 2 is the stadium bowl: stands, walls, entrance tunnels,
     * a dark floor that read as a black void under the arena, and the
     * light rings beside the big screen. */
    {GRKIND_PSTADIUM, 2, 2, -1, false},
    /* Part 2 joint 3 is the stage body: meshes 28-31 are the column under
     * the platform, which reached down to the old floor. */
    {GRKIND_PSTADIUM, 2, 3, 28, false},
    {GRKIND_PSTADIUM, 2, 3, 29, false},
    {GRKIND_PSTADIUM, 2, 3, 30, false},
    {GRKIND_PSTADIUM, 2, 3, 31, false},

    /* Survey of 2026-10-01 (desktop dumps; layer 2 is already out). Only the
     * obvious stage/background splits; the rest are listed in
     * docs/quest-xr.md. */
    /* Fountain of Dreams: the vortex, pink sea and rings. */
    {0x0C, 1, -1, -1, false},
    /* Kongo Jungle: the jungle and cliffs around the waterfall. */
    {0x04, 6, -1, -1, false},
    /* Kongo Jungle: the flat river behind the waterfall (joints 7 and 9) and
     * the surface layers over it, leaving the waterfall at the front. */
    {0x04, 4, 5, -1, false},
    {0x04, 4, 7, -1, false},
    {0x04, 4, 9, -1, false},
    {0x04, 4, 10, -1, false},
    {0x04, 4, 26, -1, false},
    {0x04, 4, 35, -1, false},
    /* Corneria: the terrain under the Great Fox, and the coastline that
     * scrolls past about a minute in. */
    {0x0E, 4, -1, -1, false},
    {0x0E, 8, -1, -1, false},
    {0x0E, 9, -1, -1, false},
    /* Yoshi's Story: the cardboard sea and hills (part 3 joints 13-21). */
    {0x0A, 3, 13, -1, false},
    {0x0A, 3, 14, -1, false},
    {0x0A, 3, 15, -1, false},
    {0x0A, 3, 16, -1, false},
    {0x0A, 3, 17, -1, false},
    {0x0A, 3, 18, -1, false},
    {0x0A, 3, 19, -1, false},
    {0x0A, 3, 20, -1, false},
    {0x0A, 3, 21, -1, false},
    /* Great Bay: land, sky and mountains. The sea (part 4) stays: it hides
     * the turtle's and the stilts' underwater parts. */
    {0x06, 3, -1, -1, false},
    /* Peach's Castle: everything around the castle is in part 3 with it.
     * Hidden: the red grounds (joint 8's own meshes), the bridge and stairs
     * (13), the hills, path and fence (25), the warp medallion (26), and
     * the trees and hedge (28's children), keeping the roof flags (65, 74,
     * 83, 92). The castle body stays whole. */
    {0x02, 3, 8, 0, false},
    {0x02, 3, 8, 1, false},
    {0x02, 3, 8, 2, false},
    {0x02, 3, 13, -1, false},
    {0x02, 3, 25, -1, false},
    {0x02, 3, 26, -1, false},
    {0x02, 3, 28, -1, false},
    {0x02, 3, 65, -1, true},
    {0x02, 3, 74, -1, true},
    {0x02, 3, 83, -1, true},
    {0x02, 3, 92, -1, true},
    /* Jungle Japes: the jungle, sky and moon (part 4) and the birds far
     * behind (part 5) go; the river (part 6, on the far layer) comes back,
     * bounded to the stage below. */
    {0x05, 4, -1, -1, false},
    {0x05, 5, -1, -1, false},
    {0x05, 6, -1, -1, true},
    /* Venom: the canyon walls and floor around the Great Fox. */
    {0x0F, 7, -1, -1, false},
    /* Kongo Jungle 64: the jungle (part 3 joint 7) and the sky (joint 34). */
    {0x1E, 3, 7, -1, false},
    {0x1E, 3, 34, -1, false},
    /* Yoshi's Island: the sky (part 1 joint 28). The clouds far behind are
     * pulled in (s_moves). The tall slanted rock at the right is still
     * shown: it is mesh 30 of joint 10 with more of it that mesh rules
     * don't reach. */
    {0x0B, 1, 28, -1, false},
    /* Brinstar: the cave, its walls, stalactites, pillars and chains. */
    {0x08, 1, -1, -1, false},
    /* Onett: everything is in part 5. Hidden: the town behind (joint 14) and
     * the hills (23); the rest is boxed in by clip planes (s_clips). */
    {0x14, 5, 14, -1, false},
    {0x14, 5, 23, -1, false},
    /* Fourside: the city (part 6 joint 2) around the three buildings you
     * fight on (joints 11, 24 and 31 of the same part). */
    {0x15, 6, 2, -1, false},
    /* Mushroom Kingdom: everything is in part 3. Hidden: the sky and hills
     * (joint 46). The mushroom poles and clouds just behind (32) stay; the
     * ground's deep blocks are cut (s_clips). */
    {0x18, 3, 46, -1, false},
    /* Mushroom Kingdom II: the clouds and trees behind (part 2 joint 8). The
     * cliffs are floor out past the blast zones, so they are boxed in
     * (s_clips). */
    {0x19, 2, 8, -1, false},
    /* Big Blue: the sky and clouds (part 2) and the corner brackets around
     * the action (31). The track, cars and passing craft are boxed in
     * (s_clips). */
    {0x13, 2, -1, -1, false},
    {0x13, 31, -1, -1, false},
    /* Mute City: the city floor far below (part 30, joint 4, mesh 12, and the
     * haze over it, 14) and the sky dome (13). The track, towers and cars
     * are boxed into a diorama (s_clips). */
    {0x12, 30, 4, 12, false},
    {0x12, 30, 4, 13, false},
    {0x12, 30, 4, 14, false},
    /* Rainbow Cruise: the corner brackets around the action (part 3). The
     * course and the ship are boxed in (s_clips). */
    {0x03, 3, -1, -1, false},
    /* Race to the Finish: the dark red backdrop behind the whole course
     * (part 1 joint 1, mesh 45). The course slides through a window
     * (s_follows). */
    {0x27, 1, 1, 45, false},
    /* Snag the Trophies (Classic's bonus stage): the starfield, the planet
     * and the sea of clouds around the tower and pillars (part 1, joints 6,
     * 7, 9, 12 and 13, far out under joint 5). */
    {0x26, 1, 6, -1, false},
    {0x26, 1, 7, -1, false},
    {0x26, 1, 9, -1, false},
    {0x26, 1, 12, -1, false},
    {0x26, 1, 13, -1, false},
    /* Adventure's Underground Maze: the maze itself is part 4, on the far
     * layer; shown, and cut to the window (s_follows). */
    {0x20, 4, -1, -1, true},
    /* All-Star's rest area: the sky dome (part 3). The meadow, its hills
     * and trees reach far behind; boxed to the fight (s_clips). */
    {0x42, 3, -1, -1, false},
    /* Home-Run Contest: the stands and sky (part 3, four copies the stage
     * slides along after the camera). The field streams through a window
     * that holds on the platform until the bag leaves it (s_follows). */
    {0x43, 3, -1, -1, false},
};

/* Joints moved in the 3D view only: the joint and everything under it are
 * scaled by `scale` about `pivot` (game units) and the pivot is put at
 * `to`. */
typedef struct {
    int grkind;
    int map_id;
    int jobj;
    float pivot[3];
    float to[3];
    float scale;
} MoveRule;

/* Pokémon Stadium's big screen stands far behind the stage (its frame,
 * joint 2, is based at z -215). Pull it in to just behind the stage and
 * shrink it. Moved at joint 1, the parent of everything the screen shows:
 * the frame (2), the picture (3), and the overlays the game unhides during
 * a transformation (4-10). MELEE_XR_JUMBOTRON="scale,x,y,z" overrides. */
static MoveRule s_moves[16] = {
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, 1, {0, -30, -215}, {0, -10, -75}, 0.55f},
    /* Great Bay: the sea, shrunk to the stage's footprint and moved under
     * it (it only extended toward the player). */
    {0x06, 4, 2, {0, 0, 0}, {10, 0, -80}, 0.56f},
    /* Yoshi's Island: the clouds (part 1 joints 20-23, 25-27, 29, with the
     * Bullet Bill and the sign), from hundreds of units behind and to the
     * sides to just behind the stage, a little above the back pillars'
     * trees: shrunk to 0.3 about the middle of the cloud field. */
#define YI_CLOUDS(j) {0x0B, 1, j, {100, 90, -350}, {0, 70, -150}, 0.3f}
    YI_CLOUDS(20), YI_CLOUDS(21), YI_CLOUDS(22), YI_CLOUDS(23),
    YI_CLOUDS(25), YI_CLOUDS(26), YI_CLOUDS(27), YI_CLOUDS(29),
#undef YI_CLOUDS
};
static int s_move_count = -1; /* built-ins, then MELEE_XR_MOVE entries */
#define MOVE_COUNT s_move_count

/* Stage parts cut below a height in the 3D view only (game units; the stage
 * stays whole in the flat frame). Fighters and items are never clipped. */
typedef struct {
    int grkind;
    int map_id;
    float plane[4]; /* kept where a*x + b*y + c*z + d >= 0 (game world) */
    float fade;     /* > 0: dissolve across this many units (soft clip) */
} ClipRule;

/* Cut below y / behind z (toward -z, away from the player) / in front of z
 * / left of x / right of x. Up to eight rules per part combine
 * (AURORA_XR_MAX_CLIPS). */
#define CLIP_BELOW(gk, part, y, fade) {gk, part, {0.f, 1.f, 0.f, -(y)}, fade}
#define CLIP_BEHIND(gk, part, z, fade) {gk, part, {0.f, 0.f, 1.f, -(z)}, fade}
#define CLIP_FRONT(gk, part, z, fade) {gk, part, {0.f, 0.f, -1.f, (z)}, fade}
#define CLIP_LEFT(gk, part, x, fade) {gk, part, {1.f, 0.f, 0.f, -(x)}, fade}
#define CLIP_RIGHT(gk, part, x, fade) {gk, part, {-1.f, 0.f, 0.f, (x)}, fade}

static const ClipRule s_clips[] = {
    CLIP_BELOW(0x0C, 3, -80.f, 0.f), /* Fountain of Dreams: the pole under the ornament */
    /* Kongo Jungle: the waterfall down to the floating rock, fading out, and
     * cut short behind the stage. */
    CLIP_BELOW(0x04, 4, -70.f, 35.f),
    CLIP_BEHIND(0x04, 4, -90.f, 15.f),
    CLIP_BELOW(0x0A, 3, -40.f, 0.f), /* Yoshi's Story: the pillar under the Shy Guys' path */
    CLIP_BELOW(0x06, 1, 0.f, 0.f),   /* Great Bay: the turtle below the waterline */
    CLIP_BELOW(0x06, 2, 0.f, 0.f),   /* Great Bay: the pier's stilts, rocks and screw */
    /* Great Bay: the sea bounded to the stage's footprint. */
    CLIP_LEFT(0x06, 4, -320.f, 10.f),
    CLIP_RIGHT(0x06, 4, 220.f, 10.f),
    CLIP_FRONT(0x06, 4, 220.f, 10.f),
    CLIP_BELOW(0x0B, 1, -40.f, 0.f), /* Yoshi's Island: the ground, halfway down */
    /* Jungle Japes: the river bounded to a rectangle just past the piers. */
    CLIP_LEFT(0x05, 6, -125.f, 8.f),
    CLIP_RIGHT(0x05, 6, 125.f, 8.f),
    CLIP_FRONT(0x05, 6, 45.f, 8.f),
    CLIP_BEHIND(0x05, 6, -100.f, 8.f),
    /* Onett: the block you fight on, with its lot, the clothesline between
     * the poles over the right house (it has collision) and the road in
     * front, cut out of the town. */
    CLIP_LEFT(0x14, 5, -130.f, 0.f),
    CLIP_RIGHT(0x14, 5, 135.f, 0.f), /* short of the bus stop sign, which was cut in half */
    CLIP_BEHIND(0x14, 5, -70.f, 0.f),
    CLIP_FRONT(0x14, 5, 70.f, 0.f),
    CLIP_BELOW(0x18, 3, -40.f, 15.f), /* Mushroom Kingdom: the ground's deep blocks */
    /* Mushroom Kingdom II: the cliffs, cut at the blast zones and below,
     * with the waterfall and the pillars under the bridge. */
    CLIP_LEFT(0x19, 2, -150.f, 10.f),
    CLIP_RIGHT(0x19, 2, 150.f, 10.f),
    CLIP_BELOW(0x19, 2, -50.f, 20.f),
    /* Big Blue: the track streams through a box, solid out to the blast
     * zones (x +-152) and dissolving over the 70 units past them: the track
     * (parts 34 and 36, also cut below and above), the cars (33) and
     * the passing craft fighters ride (32). The Falcon Flyer stays put. */
#define BB_BOX(part) CLIP_LEFT(0x13, part, -220.f, 70.f), CLIP_RIGHT(0x13, part, 220.f, 70.f)
    BB_BOX(32), BB_BOX(33), BB_BOX(34), BB_BOX(36),
#undef BB_BOX
    CLIP_BELOW(0x13, 34, -60.f, 30.f),
    CLIP_BELOW(0x13, 36, -60.f, 30.f),
    CLIP_BELOW(0x13, 33, -60.f, 30.f), /* cars thrown off the track */
    /* ...and above the action, where other stretches of it sweep overhead. */
    {0x13, 34, {0.f, -1.f, 0.f, 150.f}, 40.f},
    {0x13, 36, {0.f, -1.f, 0.f, 150.f}, 40.f},
    /* Mute City: like Big Blue, the action stays put while the city streams
     * past, but its track runs toward the player (the fighters fight across
     * it). A diorama: a block of the city (part 30), solid out past the blast
     * zones (x +-210), 90 units either side of the fight in z, and from y -80
     * (the track under the stage at the start sits just below -30) to about
     * the top blast zone (200), dissolving past that, so track, gates and
     * towers stream in and out of it. Its traffic (part 2) gets the same
     * box. */
#define MC_BOX(part)                                                                                       \
    CLIP_LEFT(0x12, part, -280.f, 70.f), CLIP_RIGHT(0x12, part, 280.f, 70.f), CLIP_BEHIND(0x12, part, -150.f, 60.f), \
        CLIP_FRONT(0x12, part, 150.f, 60.f), CLIP_BELOW(0x12, part, -110.f, 30.f),                           \
        {0x12, part, {0.f, -1.f, 0.f, 260.f}, 60.f}
    MC_BOX(30), MC_BOX(2),
#undef MC_BOX
    /* Rainbow Cruise: the action stays put while the course (parts 1, 4, 5)
     * streams past and the ship (6) sails, sinks and comes back. Boxed
     * solid to about the blast zones (x -168 to 185, y -66 to 198) and 90
     * units either side of the fight in z, dissolving past that, so the
     * course streams in and out and the ship fades as it sinks away. */
#define RC_BOX(part)                                                                                       \
    CLIP_LEFT(0x03, part, -238.f, 70.f), CLIP_RIGHT(0x03, part, 255.f, 70.f), CLIP_BELOW(0x03, part, -106.f, 40.f), \
        {0x03, part, {0.f, -1.f, 0.f, 258.f}, 60.f}, CLIP_BEHIND(0x03, part, -150.f, 60.f),                    \
        CLIP_FRONT(0x03, part, 150.f, 60.f)
    RC_BOX(1), RC_BOX(4), RC_BOX(5), RC_BOX(6),
#undef RC_BOX
    /* Brinstar: only the stretch of the acid's river around the stage (it
     * runs the length of the cave). LevelRule hides it while it is low. Its
     * cost is fill, so it is cut 10 past the stage's depth: the model (part
     * 6) reaches z -35.3 to 34.4 (tools/stagebounds.py). */
    CLIP_LEFT(0x08, 8, -120.f, 0.f),
    CLIP_RIGHT(0x08, 8, 125.f, 0.f),
    CLIP_FRONT(0x08, 8, 45.f, 0.f),
    CLIP_BEHIND(0x08, 8, -45.f, 0.f),
    /* All-Star's rest area: the meadow (parts 0 and 1) boxed to the fight,
     * about its blast zones across, cut hard. */
    CLIP_LEFT(0x42, 0, -200.f, 0.f), CLIP_RIGHT(0x42, 0, 200.f, 0.f), CLIP_BEHIND(0x42, 0, -130.f, 0.f),
    CLIP_FRONT(0x42, 0, 80.f, 0.f), CLIP_BELOW(0x42, 0, -40.f, 0.f),
    CLIP_LEFT(0x42, 1, -200.f, 0.f), CLIP_RIGHT(0x42, 1, 200.f, 0.f), CLIP_BEHIND(0x42, 1, -130.f, 0.f),
    CLIP_FRONT(0x42, 1, 80.f, 0.f), CLIP_BELOW(0x42, 1, -40.f, 0.f),
    /* Brinstar Depths: Kraid (parts 1 and 4) rises from far below; cut
     * just under the bottom blast zone (-128), fading. */
    CLIP_BELOW(0x09, 1, -150.f, 40.f),
    CLIP_BELOW(0x09, 4, -150.f, 40.f),
    /* Poke Floats: the action stays put while the giant floats (parts 2 to
     * 26, Squirtle first) drift through. Boxed solid to about the blast
     * zones (x +-163, y -101 to 152) and 120 units either side of the
     * fight in z, dissolving past that, so the floats drift in and out and
     * their bodies end below the fight. Floats still pop in and out where
     * the box shows them (docs/quest-xr.md); a tighter box (2026-10-10)
     * was too awkward to play on. */
#define PF_BOX(part)                                                                                       \
    CLIP_LEFT(0x11, part, -190.f, 60.f), CLIP_RIGHT(0x11, part, 190.f, 60.f), CLIP_BELOW(0x11, part, -110.f, 40.f), \
        {0x11, part, {0.f, -1.f, 0.f, 190.f}, 50.f}, CLIP_BEHIND(0x11, part, -120.f, 60.f),                    \
        CLIP_FRONT(0x11, part, 120.f, 60.f)
    PF_BOX(2), PF_BOX(3), PF_BOX(4), PF_BOX(5), PF_BOX(6), PF_BOX(7), PF_BOX(8), PF_BOX(9), PF_BOX(10),
    PF_BOX(11), PF_BOX(12), PF_BOX(13), PF_BOX(14), PF_BOX(15), PF_BOX(16), PF_BOX(17), PF_BOX(18),
    PF_BOX(19), PF_BOX(20), PF_BOX(21), PF_BOX(22), PF_BOX(23), PF_BOX(24), PF_BOX(25), PF_BOX(26),
#undef PF_BOX
    /* Icicle Mountain: the action stays put while the mountain's segments
     * (parts 1 to 7) stream down through it. Boxed solid to about the blast
     * zones (x +-133, y -126 to 140), dissolving past that, so the climb
     * streams in at the top and out at the bottom. */
#define IM_BOX(part)                                                                                       \
    CLIP_LEFT(0x16, part, -150.f, 40.f), CLIP_RIGHT(0x16, part, 150.f, 40.f), CLIP_BELOW(0x16, part, -140.f, 40.f), \
        {0x16, part, {0.f, -1.f, 0.f, 160.f}, 40.f}
    IM_BOX(1), IM_BOX(2), IM_BOX(3), IM_BOX(4), IM_BOX(5), IM_BOX(6), IM_BOX(7),
#undef IM_BOX
};
#define CLIP_COUNT ((int)(sizeof s_clips / sizeof s_clips[0]))

/* Where each stage sits in the arena by default: the game point placed at
 * the arena position (its main floor, centered on where the fight happens)
 * and its size against the arena's shared scale (1: Final Destination's
 * ~170-unit floor is about 1 m wide at the default 0.006 m/unit). Sizes
 * meet halfway between keeping the game's proportions and making every
 * stage's floors as wide as Final Destination's, sqrt(171 / floor width),
 * so Temple still reads as big and Fountain as small, but both fit in view
 * at the default distance. The player's grabs place only the stage being
 * played, for the rest of the session. Stages not listed sit at the world
 * origin at scale 1. MELEE_XR_CENTER="x,y,z" and MELEE_XR_STAGE_SCALE
 * override for the stage being played. */
typedef struct {
    int grkind;
    float x, y, z;
    float scale;
} StagePlacement;

static const StagePlacement s_placements[] = {
    {0x02, 0.f, 84.f, 0.f, 0.8f},    /* Peach's Castle: the roof, high above the origin */
    {0x04, 5.f, 0.f, 0.f, 1.f},      /* Kongo Jungle */
    {0x05, 0.f, 0.f, 0.f, 0.9f},     /* Jungle Japes */
    {0x06, -10.f, 0.f, 0.f, 0.85f},  /* Great Bay: its floors reach further left */
    {0x07, 0.f, 0.f, 0.f, 0.55f},    /* Hyrule Temple: smaller than the rule gives, to fit in view */
    {0x08, 0.f, 0.f, 0.f, 1.1f},     /* Brinstar */
    {0x09, 0.f, 0.f, 0.f, 0.95f},    /* Brinstar Depths */
    {0x0A, 0.f, 0.f, 0.f, 1.15f},    /* Yoshi's Story */
    {0x0B, 0.f, 0.f, 0.f, 0.9f},     /* Yoshi's Island */
    {0x0C, 0.f, 0.f, 0.f, 1.15f},    /* Fountain of Dreams */
    {0x0D, 0.f, 0.f, 0.f, 0.9f},     /* Green Greens */
    {0x0E, -10.f, 285.f, 0.f, 0.8f}, /* Corneria: the Great Fox's deck, far above the origin */
    {0x0F, 0.f, 0.f, 0.f, 0.95f},    /* Venom */
    {0x10, 0.f, 0.f, 0.f, 1.f},      /* Pokemon Stadium */
    {0x11, 0.f, 0.f, 0.f, 0.85f},    /* Poke Floats: its box is as wide as Big Blue's */
    {0x14, 0.f, 0.f, 0.f, 0.8f},     /* Onett */
    {0x15, 0.f, 0.f, 0.f, 0.8f},     /* Fourside */
    {0x13, 0.f, 0.f, 0.f, 0.85f},    /* Big Blue */
    {0x12, 0.f, 0.f, 40.f, 0.8f},    /* Mute City: its box is wider than the blast zones, and reaches 150
                                      * toward the player; set back a little */
    {0x03, 0.f, 0.f, 0.f, 0.85f},    /* Rainbow Cruise: as wide a box as Big Blue's */
    {0x18, 0.f, 0.f, 0.f, 0.9f},     /* Mushroom Kingdom */
    {0x19, 0.f, 0.f, 0.f, 0.8f},     /* Mushroom Kingdom II: its cliffs reach the blast zones */
    {0x16, 0.f, 0.f, 0.f, 0.8f},     /* Icicle Mountain: a tall box */
    {0x1B, 0.f, 0.f, 0.f, 1.1f},     /* Flat Zone */
    {0x1C, 0.f, 0.f, 0.f, 1.05f},    /* Dream Land */
    {0x1D, 0.f, 0.f, 0.f, 1.f},      /* Yoshi's Island 64 */
    {0x1E, 0.f, 0.f, 0.f, 1.f},      /* Kongo Jungle 64 */
    {0x24, 0.f, 0.f, 0.f, 1.1f},     /* Battlefield */
    {0x27, 0.f, 0.f, 0.f, 0.8f},     /* Race to the Finish: its window (s_follows) */
    {0x26, 0.f, 0.f, 0.f, 0.95f},    /* Snag the Trophies */
    {0x1F, 0.f, 0.f, 0.f, 0.8f},     /* Adventure: Mushroom Kingdom course, its window (s_follows) */
    {0x20, 0.f, 0.f, 0.f, 0.8f},     /* Adventure: Underground Maze, its window */
    {0x21, 0.f, 0.f, 0.f, 0.8f},     /* Adventure: Escape from Brinstar, its window */
    {0x22, 0.f, 0.f, 0.f, 0.8f},     /* Adventure: F-Zero Grand Prix, its window */
    {0x43, 0.f, 0.f, 0.f, 1.f},      /* Home-Run Contest: its window (s_follows) */
    {0x42, 0.f, 0.f, 0.f, 0.8f},     /* All-Star's rest area: floors x -198 to 198 */
    {0x25, 0.f, 0.f, 0.f, 1.f},      /* Final Destination */
    /* Break the Targets (surveyed 2026-10-17, MELEE_STAGE_SURVEY): each
     * course is the play area, its floors spread across and up, so the
     * center is the middle of all its floors and the size rule takes the
     * larger of their width and height (a tall course fits too). */
    {0x28, -50.f, 30.f, 0.f, 0.65f},  /* Mario: floors x -210 to 112, y -161 to 218 (tall) */
    {0x29, 30.f, -20.f, 0.f, 0.75f},  /* Captain Falcon: x -130 to 190, y -155 to 110 */
    {0x2A, 0.f, 85.f, 0.f, 0.85f},    /* Young Link: x -120 to 120, y -40 to 210 (tall) */
    {0x2B, 0.f, 80.f, 0.f, 0.65f},    /* Donkey Kong: x -190 to 190, y -35 to 200 */
    {0x2C, 15.f, -10.f, 0.f, 0.85f},  /* Dr. Mario: x -96 to 130, y -105 to 90 */
    {0x2D, -15.f, 5.f, 0.f, 0.85f},   /* Falco: x -140 to 110, y -80 to 90 */
    {0x2E, 10.f, 5.f, 0.f, 0.8f},     /* Fox: x -120 to 145, y -130 to 140 (tall) */
    {0x2F, 0.f, 260.f, 0.f, 0.55f},   /* Ice Climbers: a climb, x -120 to 120, y -20 to 539 */
    {0x30, -10.f, 45.f, 0.f, 0.8f},   /* Kirby: x -150 to 130, y -70 to 160 */
    {0x31, 65.f, -30.f, 0.f, 0.7f},   /* Bowser: x -105 to 235, y -145 to 85 */
    {0x32, -10.f, 0.f, 0.f, 0.85f},   /* Link: x -135 to 110, y -90 to 90 */
    {0x33, 0.f, 5.f, 0.f, 1.05f},     /* Luigi: x -80 to 80, y -70 to 80 */
    {0x34, -30.f, 35.f, 0.f, 0.85f},  /* Marth: x -150 to 90, y -70 to 140 */
    {0x35, 5.f, 0.f, 0.f, 0.85f},     /* Mewtwo: x -120 to 130, y -80 to 85 */
    {0x36, 0.f, -25.f, 0.f, 0.75f},   /* Ness: x -150 to 150, y -130 to 80 */
    {0x37, 20.f, 20.f, 0.f, 0.8f},    /* Peach: x -110 to 150, y -90 to 130 */
    {0x38, -25.f, 20.f, 0.f, 0.75f},  /* Pichu: x -180 to 130, y -93 to 135 */
    {0x39, 10.f, 20.f, 0.f, 0.8f},    /* Pikachu: x -130 to 150, y -80 to 115 */
    {0x3A, 5.f, 5.f, 0.f, 0.75f},     /* Jigglypuff: x -150 to 156, y -60 to 70 */
    {0x3B, 10.f, 15.f, 0.f, 0.85f},   /* Samus: x -110 to 130, y -110 to 140 (tall) */
    {0x3C, -5.f, 0.f, 0.f, 0.95f},    /* Sheik: one floor, x -100 to 90 */
    {0x3D, -10.f, 30.f, 0.f, 0.75f},  /* Yoshi: x -155 to 130, y -110 to 165 */
    {0x3E, -15.f, -5.f, 0.f, 0.85f},  /* Zelda: x -128 to 100, y -90 to 80 */
    {0x3F, -10.f, 5.f, 0.f, 1.f},     /* Mr. Game & Watch: x -92 to 77, y -29 to 41 */
    {0x40, -20.f, 50.f, 0.f, 0.8f},   /* Roy: x -155 to 110, y -20 to 117 */
    {0x41, -10.f, 25.f, 0.f, 0.75f},  /* Ganondorf: x -171 to 147, y 0 to 51 */
    /* Adventure's trophy fights, on giant trophy stands. */
    {0x44, 5.f, -15.f, 0.f, 1.05f},   /* floors x -70 to 79, y -58 to 27 */
    {0x45, 5.f, -10.f, 0.f, 0.95f},   /* floors x -90 to 101, y -42 to 18 */
    {0x46, 0.f, 0.f, 0.f, 1.05f},     /* floors x -80 to 80, y -52 to 56 */
};
#define PLACEMENT_COUNT ((int)(sizeof s_placements / sizeof s_placements[0]))

/* Stages whose camera roams a course far bigger than the arena (Race to the
 * Finish follows player one across it). The arena shows a window of the
 * course around a center that follows the fight camera's focus, and the
 * course slides through it: the center stays put while the focus is within
 * `slack` of it (game units, x and y) and is dragged along once it goes
 * past, so the world doesn't move with every jump. In mixed reality a box
 * `half` units either side of the center (x, y, z) clips everything the 3D
 * view draws, fighters too, dissolving over `fade` units inside it.
 * MELEE_XR_FOLLOW="hx,hy,hz,sx,sy,fade" overrides for the stage played
 * (one fade for all sides). `fade` is per axis (x, y, z): 0 cuts that pair
 * of sides solid. A negative slack[1] keeps the window's height. A rule
 * with `hold` set keeps the window at its start until follow_released says
 * the stage's action has begun, then follows. */
typedef enum {
    FOLLOW_ALWAYS,
    FOLLOW_AFTER_LAUNCH, /* Home-Run Contest: once the bag has left the platform */
} FollowHold;

typedef struct {
    int grkind;
    float half[3];
    float slack[2];
    float fade[3];
    FollowHold hold;
    float start[2]; /* a held window's center until the platform is measured (y: always) */
    bool open_top;  /* no top side: things flying high stay in view */
    /* Seconds for the window to settle on where the slack puts it: the
     * camera's own easing alone made it feel stiff (Adventure's Mushroom
     * Kingdom course). 0: at once. */
    float ease;
    /* While the game's camera is locked on a stretch of the course
     * (Camera_80030AF8 false: the Underground Maze's platform fights), the
     * window sits on the camera's target instead, zooms by lock_zoom, and
     * its box is the stage's blast zones (the fight's own bounds). 0:
     * never. */
    float lock_zoom;
    bool hud_on_top; /* scenery reaches the HUD's height (aurora_xr_set_hud_on_top) */
    /* Follows player one's fighter rather than the camera's focus: the
     * Underground Maze's camera holds still for a second while the player
     * is hit, and the player could walk out of the window. */
    bool follow_player;
} FollowRule;

static const FollowRule s_follows[] = {
    {.grkind = 0x27, /* Race to the Finish */
     .half = {160.f, 110.f, 80.f}, .slack = {40.f, 30.f}, .fade = {25.f, 25.f, 25.f}, .ease = 0.25f},
    /* Adventure's courses, whose camera follows player one through them. */
    {.grkind = 0x1F, /* Mushroom Kingdom */
     .half = {160.f, 110.f, 80.f}, .slack = {60.f, 35.f}, .fade = {25.f, 25.f, 25.f}, .ease = 0.35f},
    {.grkind = 0x20, /* Underground Maze: the scenery reaches the HUD */
     .half = {160.f, 110.f, 80.f}, .slack = {40.f, 30.f}, .fade = {25.f, 25.f, 25.f}, .ease = 0.3f,
     .lock_zoom = 1.4f, .hud_on_top = true, .follow_player = true},
    {.grkind = 0x21, /* Escape from Brinstar: the shaft reaches the HUD */
     .half = {160.f, 110.f, 80.f}, .slack = {40.f, 30.f}, .fade = {25.f, 25.f, 25.f}, .ease = 0.25f,
     .hud_on_top = true},
    {.grkind = 0x22, /* F-Zero Grand Prix */
     .half = {160.f, 110.f, 80.f}, .slack = {40.f, 30.f}, .fade = {25.f, 25.f, 25.f}, .ease = 0.25f},
    /* Home-Run Contest: held centered on the platform while the bag is on
     * it; then along after the camera, which follows the bag, but never up
     * or down: the field stays in view, and with no top a high flyer does
     * too. Front and back are cut solid. No easing: the bag outran it into
     * the window's dissolving sides. */
    {.grkind = 0x43,
     .half = {160.f, 110.f, 80.f}, .slack = {40.f, -1.f}, .fade = {25.f, 25.f, 0.f},
     .hold = FOLLOW_AFTER_LAUNCH, .start = {0.f, 60.f}, .open_top = true},
};
#define FOLLOW_COUNT ((int)(sizeof s_follows / sizeof s_follows[0]))

/* Stage parts hidden in the 3D view while one of their joints (by index in
 * a depth-first walk, as in PartRule) sits below a height (its translation,
 * game units). For hazards that only matter once they come up. */
typedef struct {
    int grkind;
    int map_id;
    int jobj;
    float min_y;  /* below: hidden */
    float full_y; /* above: solid; in between it dissolves in */
} LevelRule;

static const LevelRule s_levels[] = {
    /* Brinstar: the acid (joint 1's height is its level, about -250 to 90;
     * the surface sits about 90 below). It starts far below, out of bounds,
     * and fades in as it rises to just under the stage. */
    {0x08, 8, 1, -50.f, -10.f},
};
#define LEVEL_COUNT ((int)(sizeof s_levels / sizeof s_levels[0]))

/* What a stage stands on a real surface with, for the soft shadow it casts
 * there in mixed reality (aurora_xr_set_stage_footprint): the x and z span
 * of its body and its underside, the lowest point the 3D view keeps (game
 * units). Measured from the disc's models (tools/stagebounds.py). Stages not
 * listed take their floors' x span, 30 units either side in z, and their
 * lowest cut (s_clips) or -40 as the underside. Stages whose course slides
 * through a window (s_follows) cast none. */
typedef struct {
    int grkind;
    float xmin, xmax, zmin, zmax, ybottom;
} StageFootprint;

static const StageFootprint s_footprints[] = {
    {0x24, -85.5f, 85.5f, -36.5f, 36.5f, -50.f},   /* Battlefield: the platform (part 6) */
    {0x25, -85.6f, 85.6f, -85.6f, 85.6f, -139.f},  /* Final Destination: part 3, its point far below */
    {0x08, -73.f, 63.1f, -35.3f, 34.4f, -35.f},    /* Brinstar: part 6 */
    {0x0C, -85.9f, 85.9f, -93.7f, 80.6f, -80.f},   /* Fountain of Dreams: the basin; the pole is cut at -80 */
    {0x16, -150.f, 150.f, -48.f, 48.f, -140.f},    /* Icicle Mountain: its box (s_clips) */
    {0x13, -152.f, 152.f, -60.f, 60.f, -60.f},     /* Big Blue: the solid part of its box, cut below -60 */
    {0x1C, -77.3f, 77.3f, -50.f, 50.f, -36.f},     /* Dream Land: parts 4 and 5 */
};
#define FOOTPRINT_COUNT ((int)(sizeof s_footprints / sizeof s_footprints[0]))

/* The room's grade during a stage's fights in mixed reality
 * (aurora_xr_set_room_grade): passthrough nudged toward the stage's
 * palette, gains per channel in linear light and a saturation. The night
 * and space stages (Battlefield, Final Destination, Fourside) take the
 * room well down, the rest only tint it. Stages not listed leave the room
 * as it is. MELEE_XR_GRADE="r,g,b,saturation" overrides for the stage
 * played. */
typedef struct {
    int grkind;
    float r, g, b, saturation;
} RoomGrade;

static const RoomGrade s_grades[] = {
    {0x08, 1.f, 0.86f, 0.78f, 1.05f},   /* Brinstar: warm red, a little darker */
    {0x16, 0.88f, 0.95f, 1.f, 0.92f},   /* Icicle Mountain: cool */
    {0x13, 0.88f, 0.95f, 1.f, 0.92f},   /* Big Blue: cool */
    {0x0C, 0.9f, 0.8f, 0.98f, 0.85f},   /* Fountain of Dreams: dusk violet */
    {0x24, 0.38f, 0.38f, 0.42f, 0.6f},  /* Battlefield: house lights well down, a little cool */
    {0x25, 0.27f, 0.27f, 0.31f, 0.5f},  /* Final Destination: darker still, out in space */
    {0x15, 0.32f, 0.36f, 0.5f, 0.65f},  /* Fourside: the city at night, dark and blue */
    /* Venom: a warm, dim canyon from the start; its lights then move it
     * further as the Great Fox flies through the caves (dynamic, below).
     * Without a base the room stayed as it was until the lights first
     * dimmed, halfway through a fight. */
    {0x0F, 0.95f, 0.86f, 0.8f, 0.9f},
};
#define GRADE_COUNT ((int)(sizeof s_grades / sizeof s_grades[0]))

/* Stages whose own lighting changes during a fight, which the room's grade
 * then follows: Venom, whose Great Fox flies through caves where the
 * stage's lights dim and redden. Each frame the 3D view draws, the stage's
 * lights (its light objects as their animations leave them; read only) are
 * summed per channel and set against the brightest they have been this
 * fight, and the room takes MELEE_XR_GRADE_DYNAMIC (0.6; 0: off) of that
 * change, within 0.55 to 1.1 per channel, on top of the stage's own grade.
 * MELEE_XR_LIGHT_LOG=1 logs the lights once a second. */
static const int s_dynamic_grades[] = {0x0F /* Venom */};
#define DYNAMIC_GRADE_COUNT ((int)(sizeof s_dynamic_grades / sizeof s_dynamic_grades[0]))

static RoomGrade s_grade; /* the current stage's own */
static bool s_grade_dynamic;
static float s_light_base[3]; /* the stage's lights at their brightest this fight */
static float s_light_sent[3]; /* the gains last sent */

static void dynamic_grade_reset(void) {
    for (int c = 0; c < 3; c++) {
        s_light_base[c] = 0.f;
        s_light_sent[c] = 1.f;
    }
    aurora_xr_set_room_grade(s_grade.r, s_grade.g, s_grade.b, s_grade.saturation);
}

static void dynamic_grade(void) {
    static float strength = -1.f;
    static int log = -1;
    if (strength < 0.f) {
        const char* v = getenv("MELEE_XR_GRADE_DYNAMIC");
        strength = v != NULL ? (float)atof(v) : 0.6f;
        strength = strength < 0.f ? 0.f : strength > 1.f ? 1.f : strength;
        log = getenv("MELEE_XR_LIGHT_LOG") != NULL;
    }
    HSD_GObj* lights = s_grade_dynamic && strength > 0.f ? Ground_801C498C() : NULL;
    if (lights == NULL) {
        return;
    }
    float sum[3] = {0.f, 0.f, 0.f};
    for (HSD_LObj* l = (HSD_LObj*)lights->hsd_obj; l != NULL; l = HSD_LObjGetNext(l)) {
        sum[0] += l->color.r / 255.f;
        sum[1] += l->color.g / 255.f;
        sum[2] += l->color.b / 255.f;
    }
    const float lum = 0.2126f * sum[0] + 0.7152f * sum[1] + 0.0722f * sum[2];
    if (lum > 0.2126f * s_light_base[0] + 0.7152f * s_light_base[1] + 0.0722f * s_light_base[2]) {
        memcpy(s_light_base, sum, sizeof sum);
    }
    float gain[3];
    bool changed = false;
    for (int c = 0; c < 3; c++) {
        const float ratio = s_light_base[c] > 0.01f ? sum[c] / s_light_base[c] : 1.f;
        gain[c] = 1.f + strength * (ratio - 1.f);
        gain[c] = gain[c] < 0.55f ? 0.55f : gain[c] > 1.1f ? 1.1f : gain[c];
        changed |= fabsf(gain[c] - s_light_sent[c]) >= 0.02f || (gain[c] == 1.f && s_light_sent[c] != 1.f);
    }
    if (changed) {
        memcpy(s_light_sent, gain, sizeof gain);
        aurora_xr_set_room_grade(s_grade.r * gain[0], s_grade.g * gain[1], s_grade.b * gain[2], s_grade.saturation);
    }
    static Uint64 logged;
    if (log && SDL_GetTicks() - logged >= 1000) {
        logged = SDL_GetTicks();
        pc_log_line("xr: stage lights %.2f,%.2f,%.2f (brightest %.2f,%.2f,%.2f): room grade x%.2f,%.2f,%.2f", sum[0],
                    sum[1], sum[2], s_light_base[0], s_light_base[1], s_light_base[2], s_light_sent[0],
                    s_light_sent[1], s_light_sent[2]);
    }
}

static void room_grade(int grkind) {
    RoomGrade g = {grkind, 1.f, 1.f, 1.f, 1.f};
    for (int i = 0; i < GRADE_COUNT; i++) {
        if (s_grades[i].grkind == grkind) {
            g = s_grades[i];
        }
    }
    const char* env = getenv("MELEE_XR_GRADE");
    if (env != NULL) {
        sscanf(env, "%f,%f,%f,%f", &g.r, &g.g, &g.b, &g.saturation);
    }
    s_grade = g;
    s_grade_dynamic = false;
    for (int i = 0; i < DYNAMIC_GRADE_COUNT; i++) {
        s_grade_dynamic |= s_dynamic_grades[i] == grkind;
    }
    dynamic_grade_reset();
}

static HSD_JObj* nth_joint(HSD_JObj* jobj, int* n) {
    for (; jobj != NULL; jobj = jobj->next) {
        if ((*n)-- == 0) {
            return jobj;
        }
        HSD_JObj* found = nth_joint(jobj->child, n);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* 1: shown; 0: hidden; in between, how far a rising part has faded in. */
static float level_opacity(int grkind, int map_id, HSD_JObj* root) {
    for (int i = 0; i < LEVEL_COUNT; i++) {
        if (s_levels[i].grkind != grkind || s_levels[i].map_id != map_id || root == NULL) {
            continue;
        }
        int n = s_levels[i].jobj;
        /* The root alone: its siblings are other parts. */
        HSD_JObj* j = n == 0 ? root : (n--, nth_joint(root->child, &n));
        if (j != NULL && getenv("MELEE_XR_LEVEL_LOG") != NULL) {
            static unsigned calls;
            if (calls++ % 120 == 0) {
                pc_log_line("xr: stage %d part %d joint %d at y %.1f (shown from %.1f)", grkind, map_id,
                            s_levels[i].jobj, j->translate.y, s_levels[i].min_y);
            }
        }
        if (j != NULL) {
            const LevelRule* l = &s_levels[i];
            float lo = l->min_y, hi = l->full_y;
            const char* band = getenv("MELEE_XR_LEVEL_FADE"); /* surveying: "min,full" */
            if (band != NULL) {
                sscanf(band, "%f,%f", &lo, &hi);
            }
            const float t = hi > lo ? (j->translate.y - lo) / (hi - lo) : (j->translate.y >= lo ? 1.f : 0.f);
            return t <= 0.f ? 0.f : t >= 1.f ? 1.f : t;
        }
    }
    return 1.f;
}

#define MAX_RULES 256
static PartRule s_rules[MAX_RULES];
static int s_rule_count = -1;
/* Stage passes draw layers 2, 1, 0, 3 in that order. Layer 2 is the far
 * background (Final Destination: the space backdrop, parts 4-8); the stage
 * itself sits on layers 1 and 0 (FD: parts 1, 3 and 0, 2). Show everything
 * but layer 2 unless a rule says otherwise. */
static unsigned s_layer_mask = 0xB;
static bool s_log_parts;
static bool s_log_joints;
static int s_category = AURORA_XR_MONO;

/* The stage part being drawn, when it has joint rules: the joints with
 * anything hidden or moved, sorted by pointer for bsearch, with whether
 * the whole joint is hidden, a mask of their hidden meshes, and their move
 * (-1: none). */
#define MAX_HIDDEN_JOINTS 1024
#define MAX_DOBJS 256 /* Onett's town joint alone has 100 meshes */
typedef struct {
    uint64_t w[MAX_DOBJS / 64];
} MeshMask;
typedef struct {
    HSD_JObj* jobj;
    bool all; /* the whole joint */
    MeshMask dobjs;
    int move;
} HiddenJoint;

static bool mask_test(const MeshMask* m, int i) {
    return i >= 0 && i < MAX_DOBJS && (m->w[i / 64] >> (i % 64) & 1);
}

static void mask_set(MeshMask* m, int i, bool on) {
    if (i < 0 || i >= MAX_DOBJS) {
        return;
    }
    const uint64_t bit = (uint64_t) 1 << (i % 64);
    m->w[i / 64] = on ? m->w[i / 64] | bit : m->w[i / 64] & ~bit;
}

static bool mask_any(const MeshMask* m) {
    for (int k = 0; k < MAX_DOBJS / 64; k++) {
        if (m->w[k] != 0) {
            return true;
        }
    }
    return false;
}
enum { CHANGED_MONO = 1, CHANGED_MOVE = 2 };
static bool s_joint_mode;
static HiddenJoint s_hidden_joints[MAX_HIDDEN_JOINTS];
static int s_hidden_joint_count;

static void load_rules(void) {
    s_rule_count = 0;
    for (size_t i = 0; i < sizeof s_builtin_rules / sizeof s_builtin_rules[0]; i++) {
        s_rules[s_rule_count++] = s_builtin_rules[i];
    }
    const char* mask = getenv("MELEE_XR_STAGE_LAYERS");
    if (mask != NULL && mask[0] != '\0') {
        s_layer_mask = (unsigned)strtoul(mask, NULL, 0);
    }
    s_log_parts = getenv("MELEE_XR_STAGE_LOG") != NULL;
    s_log_joints = getenv("MELEE_XR_JOINT_LOG") != NULL;
    s_move_count = 0;
    while (s_move_count < (int)(sizeof s_moves / sizeof s_moves[0]) && s_moves[s_move_count].scale != 0.f) {
        s_move_count++;
    }
    /* MELEE_XR_MOVE="gk:part:joint:scale:px:py:pz:tx:ty:tz;...": try moves. */
    for (const char* m = getenv("MELEE_XR_MOVE"); m != NULL && *m != '\0';) {
        MoveRule r;
        if (s_move_count < (int)(sizeof s_moves / sizeof s_moves[0]) &&
            sscanf(m, "%d:%d:%d:%f:%f:%f:%f:%f:%f:%f", &r.grkind, &r.map_id, &r.jobj, &r.scale, &r.pivot[0],
                &r.pivot[1], &r.pivot[2], &r.to[0], &r.to[1], &r.to[2]) == 10) {
            s_moves[s_move_count++] = r;
        }
        m = strchr(m, ';');
        m = m != NULL ? m + 1 : NULL;
    }
    const char* jumbo = getenv("MELEE_XR_JUMBOTRON");
    float js, jx, jy, jz;
    if (jumbo != NULL && sscanf(jumbo, "%f,%f,%f,%f", &js, &jx, &jy, &jz) == 4) {
        for (int i = 0; i < MOVE_COUNT; i++) {
            if (s_moves[i].grkind == GRKIND_PSTADIUM && s_moves[i].map_id == PSTYPE_DISPLAY) {
                s_moves[i].scale = js;
                s_moves[i].to[0] = jx;
                s_moves[i].to[1] = jy;
                s_moves[i].to[2] = jz;
            }
        }
    }
    const char* spec = getenv("MELEE_XR_PARTS");
    while (spec != NULL && *spec != '\0' && s_rule_count < MAX_RULES) {
        bool show = true;
        if (*spec == '-' || *spec == '+') {
            show = *spec == '+';
            spec++;
        }
        char* end = NULL;
        const long kind = strtol(spec, &end, 0);
        if (end == spec || *end != ':') {
            break;
        }
        spec = end + 1;
        const long id = strtol(spec, &end, 0);
        if (end == spec) {
            break;
        }
        long joint = -1, mesh = -1;
        if (*end == '/') {
            spec = end + 1;
            joint = strtol(spec, &end, 0);
            if (end == spec) {
                break;
            }
            if (*end == '.') {
                spec = end + 1;
                mesh = strtol(spec, &end, 0);
                if (end == spec || mesh < 0 || mesh >= MAX_DOBJS) {
                    break;
                }
            }
        }
        s_rules[s_rule_count++] = (PartRule){(int)kind, (int)id, (int)joint, (int)mesh, show};
        spec = *end == ',' ? end + 1 : end;
    }
}

static bool part_visible(int grkind, int map_id, int layer) {
    if (s_rule_count < 0) {
        load_rules();
    }
    bool visible = layer >= 0 && layer < 8 && (s_layer_mask & (1u << layer)) != 0;
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].grkind == grkind && s_rules[i].map_id == map_id && s_rules[i].jobj < 0) {
            visible = s_rules[i].show; /* later rules (env) win */
        }
    }
    if (s_log_parts) {
        /* Once per (stage, part): enough to map a stage's layers. */
        static unsigned seen[64][4];
        const unsigned bucket = (unsigned)grkind & 63u, bit = (unsigned)map_id & 127u;
        if (!(seen[bucket][bit / 32] & (1u << (bit % 32)))) {
            seen[bucket][bit / 32] |= 1u << (bit % 32);
            pc_log_line("xr: stage %d part %d layer %d -> %s", grkind, map_id, layer,
                visible ? "3D" : "hidden");
        }
    }
    return visible;
}

static bool s_flat_view;

void pc_xr_flat_view(bool on) { s_flat_view = on; }

bool pc_xr_world_3d(void) { return !s_flat_view && pc_xr_presenting(); }

/* The stage being followed (s_follows), for the stage last drawn; NULL when
 * it isn't one. Its center, and whether that has been placed yet. */
static FollowRule s_follow_rule;
static const FollowRule* s_follow;
static float s_follow_center[3];
static bool s_follow_placed;
static bool s_follow_released; /* a held window has started following (follow_released) */
static float s_follow_target[2]; /* where the slack (or a locked camera) puts the center; it eases there */
static float s_follow_zoom = 1.f;
static bool s_follow_locked; /* the game's camera is locked on a stretch (lock_zoom) */
/* The platform a held window starts on: the x span of the floors at the
 * stage's top floor height (stage_top), NAN until measured. */
static float s_platform_left = NAN, s_platform_right = NAN;
static float s_focus[3];

static void follow_stage(int grkind) {
    s_follow = NULL;
    s_follow_placed = false;
    s_follow_released = false;
    s_follow_zoom = 1.f;
    s_follow_locked = false;
    s_platform_left = s_platform_right = NAN;
    for (int i = 0; i < FOLLOW_COUNT; i++) {
        if (s_follows[i].grkind == grkind) {
            s_follow_rule = s_follows[i];
            const char* env = getenv("MELEE_XR_FOLLOW");
            if (env != NULL) {
                FollowRule* r = &s_follow_rule;
                float fade = r->fade[0];
                sscanf(env, "%f,%f,%f,%f,%f,%f", &r->half[0], &r->half[1], &r->half[2], &r->slack[0],
                    &r->slack[1], &fade);
                r->fade[0] = r->fade[1] = r->fade[2] = fade;
            }
            s_follow = &s_follow_rule;
            aurora_xr_set_hud_on_top(s_follow->hud_on_top);
        }
    }
}

/* The window's box: kept inside all six planes (game world). */
static int follow_planes(float planes[][4], float fades[]) {
    const float* c = s_follow_center;
    const float* h = s_follow->half;
    /* Locked on a fight: the fight's own bounds, its blast zones. */
    float lo[3] = {c[0] - h[0], c[1] - h[1], c[2] - h[2]};
    float hi[3] = {c[0] + h[0], c[1] + h[1], c[2] + h[2]};
    if (s_follow_locked) {
        lo[0] = Stage_GetBlastZoneLeftOffset();
        hi[0] = Stage_GetBlastZoneRightOffset();
        lo[1] = Stage_GetBlastZoneBottomOffset();
        hi[1] = Stage_GetBlastZoneTopOffset();
    }
    const float box[6][4] = {
        {1.f, 0.f, 0.f, -lo[0]}, {-1.f, 0.f, 0.f, hi[0]},
        {0.f, 1.f, 0.f, -lo[1]}, {0.f, -1.f, 0.f, hi[1]},
        {0.f, 0.f, 1.f, -lo[2]}, {0.f, 0.f, -1.f, hi[2]},
    };
    int n = 0;
    for (int i = 0; i < 6; i++) {
        if (i == 3 && s_follow->open_top) {
            continue; /* {0, -1, 0}: the top */
        }
        memcpy(planes[n], box[i], sizeof box[i]);
        fades[n++] = s_follow->fade[i / 2];
    }
    return n;
}

/* The clip everything in the 3D view gets between stage parts: the
 * window's box while following in mixed reality, else none. */
static void base_clip(void) {
    if (s_follow != NULL && s_follow_placed && pc_xr_mixed_reality()) {
        float planes[AURORA_XR_MAX_CLIPS][4], fades[AURORA_XR_MAX_CLIPS];
        const int n = follow_planes(planes, fades);
        aurora_xr_world_clips4((const float(*)[4])planes, fades, n);
    } else {
        aurora_xr_world_clip(NULL);
    }
}

void pc_xr_camera_focus(const float interest[3]) { memcpy(s_focus, interest, sizeof s_focus); }

/* Whether a held window may start following. Home-Run Contest: the stage's
 * distance reading (Ground_801C57F0, grHomeRun_8021EA30) is the bag's
 * travel past the platform's edge, 0 until it leaves; latched, so a bag
 * that lands short of the edge doesn't snap the window back. */
static bool follow_released(void) {
    if (!s_follow_released) {
        switch (s_follow->hold) {
        case FOLLOW_ALWAYS:
            s_follow_released = true;
            break;
        case FOLLOW_AFTER_LAUNCH: {
            /* The bag (player slot 1) past the platform's right edge. The
             * stage's own distance reading starts at x 67, well inside the
             * platform, so a bag shoved to the edge counted as launched. */
            HSD_GObj* bag = Player_GetEntity(1);
            if (bag != NULL && !isnan(s_platform_right)) {
                Vec3 pos;
                ftLib_GetPos(bag, &pos);
                s_follow_released = pos.x > s_platform_right + 10.f;
            }
            break;
        }
        }
        if (s_follow_released && s_follow->hold != FOLLOW_ALWAYS) {
            pc_log_line("xr: the window follows from focus %.0f,%.0f", s_focus[0], s_focus[1]);
        }
    }
    return s_follow_released;
}

/* Drags the window's center after the focus, past the slack; a held
 * window stays at its start until released. */
static void follow_update(void) {
    if (s_follow == NULL) {
        return;
    }
    if (!s_follow_placed) {
        /* A held window is placed below, until released. */
        memcpy(s_follow_center, s_focus, sizeof s_follow_center);
        s_follow_center[2] = 0.f;
        s_follow_target[0] = s_follow_center[0];
        s_follow_target[1] = s_follow_center[1];
        s_follow_placed = true;
    }
    if (!follow_released()) {
        /* Held: placed again each frame, as the platform is measured once
         * the stage's collision is in (stage_top). */
        s_follow_center[0] =
            isnan(s_platform_left) ? s_follow->start[0] : 0.5f * (s_platform_left + s_platform_right);
        s_follow_center[1] = s_follow->start[1];
        s_follow_target[0] = s_follow_center[0];
        s_follow_target[1] = s_follow_center[1];
        aurora_xr_set_stage_center(s_follow_center[0], s_follow_center[1], s_follow_center[2]);
        static int held_log = -1, held_frames;
        if (held_log < 0) {
            held_log = getenv("MELEE_XR_FOLLOW_LOG") != NULL;
        }
        HSD_GObj* bag = Player_GetEntity(1);
        if (held_log && bag != NULL && ++held_frames % 30 == 0) {
            Vec3 pos;
            ftLib_GetPos(bag, &pos);
            pc_log_line("xr: window held at %.0f,%.0f; bag at %.0f,%.0f", s_follow_center[0], s_follow_center[1], pos.x,
                        pos.y);
        }
        return;
    }
    const bool locked = s_follow->lock_zoom > 0.f && !Camera_80030AF8();
    if (locked != s_follow_locked) {
        pc_log_line("xr: camera %s; blast zones x %.0f to %.0f, y %.0f to %.0f", locked ? "locked" : "free",
                    Stage_GetBlastZoneLeftOffset(), Stage_GetBlastZoneRightOffset(), Stage_GetBlastZoneBottomOffset(),
                    Stage_GetBlastZoneTopOffset());
        s_follow_locked = locked;
    }
    float focus[2] = {s_focus[0], s_focus[1]};
    HSD_GObj* player = s_follow->follow_player ? Player_GetEntity(0) : NULL;
    if (player != NULL) {
        Vec3 pos;
        ftLib_GetPos(player, &pos);
        focus[0] = pos.x;
        focus[1] = pos.y + 10.f; /* about the body's middle */
    }
    if (locked) {
        Vec3 t;
        Stage_UnkSetVec3TCam_Offset(&t);
        s_follow_target[0] = t.x;
        s_follow_target[1] = t.y;
    } else {
        for (int k = 0; k < 2; k++) {
            const float d = focus[k] - s_follow_target[k];
            const float slack = s_follow->slack[k];
            if (slack < 0.f) {
                continue; /* this axis stays put */
            }
            if (d > slack) {
                s_follow_target[k] = focus[k] - slack;
            } else if (d < -slack) {
                s_follow_target[k] = focus[k] + slack;
            }
        }
    }
    /* Eased toward the target, at 60 frames a second. */
    const float step = s_follow->ease > 0.f ? 1.f - expf(-1.f / (60.f * s_follow->ease)) : 1.f;
    for (int k = 0; k < 2; k++) {
        s_follow_center[k] += (s_follow_target[k] - s_follow_center[k]) * step;
    }
    const float zoom = locked ? s_follow->lock_zoom : 1.f;
    if (s_follow_zoom != zoom) {
        s_follow_zoom += (zoom - s_follow_zoom) * step;
        if (fabsf(zoom - s_follow_zoom) < 0.002f) {
            s_follow_zoom = zoom;
        }
        aurora_xr_set_stage_zoom(s_follow_zoom);
    }
    aurora_xr_set_stage_center(s_follow_center[0], s_follow_center[1], s_follow_center[2]);
    /* MELEE_XR_FOLLOW_LOG: the window's center and the focus, once a second. */
    static int log_on = -1, frames;
    if (log_on < 0) {
        log_on = getenv("MELEE_XR_FOLLOW_LOG") != NULL;
    }
    if (log_on && ++frames % 60 == 0) {
        pc_log_line("xr: window at %.0f,%.0f, focus %.0f,%.0f, zoom %.2f%s", s_follow_center[0], s_follow_center[1],
                    s_focus[0], s_focus[1], s_follow_zoom, Camera_80030AF8() ? "" : ", camera locked");
    }
}

/* Slippi's pause: the overlay only, the match runs on (gm/gmvs.c). */
extern bool gm_vs_soft_paused(void);

/* MELEE_XR_PROBE=1: a log line whenever what the 3D view draws around the
 * blast zones changes from one drawn frame to the next (draws of fighters
 * off the flat camera, magnifier bubbles, KO explosions clipped to the
 * blast box, KOs), to line up with the XR stats (AURORA_XR_LOG_PERIOD). */
static struct {
    int on;
    unsigned frame;
    int offscreen, magnified, blast, ko;
    int last_offscreen, last_magnified, last_blast;
} s_probe = {.on = -1};

static bool probe_on(void) {
    if (s_probe.on < 0) {
        s_probe.on = getenv("MELEE_XR_PROBE") != NULL && getenv("MELEE_XR_PROBE")[0] == '1';
    }
    return s_probe.on;
}

static void probe_frame(void) {
    if (!probe_on()) {
        return;
    }
    s_probe.frame++;
    if (s_probe.offscreen != s_probe.last_offscreen || s_probe.magnified != s_probe.last_magnified ||
        s_probe.blast != s_probe.last_blast || s_probe.ko > 0) {
        pc_log_line("xr-probe: frame %u offscreen %d magnified %d blast %d%s", s_probe.frame, s_probe.offscreen,
                    s_probe.magnified, s_probe.blast, s_probe.ko > 0 ? " KO" : "");
    }
    s_probe.last_offscreen = s_probe.offscreen;
    s_probe.last_magnified = s_probe.magnified;
    s_probe.last_blast = s_probe.blast;
    s_probe.offscreen = s_probe.magnified = s_probe.blast = s_probe.ko = 0;
}

void pc_xr_world_camera(const float view[3][4]) {
    probe_frame();
    if (s_flat_view) {
        pc_xr_mono_camera();
        return;
    }
    /* Pause flags 1 and 2: a player paused the match (gm_DoPauseChecksAndRoutine). */
    const bool stopped = gm_GetDbPauseFlag(1) || gm_GetDbPauseFlag(2);
    aurora_xr_set_paused(stopped || gm_vs_soft_paused());
    aurora_xr_set_match_stopped(stopped);
    s_category = AURORA_XR_WORLD;
    aurora_xr_camera(AURORA_XR_WORLD, view);
    follow_update();
    base_clip();
    dynamic_grade();
}

void pc_xr_hud_camera(void) {
    if (s_flat_view) {
        pc_xr_mono_camera();
        return;
    }
    s_category = AURORA_XR_HUD;
    aurora_xr_camera(AURORA_XR_HUD, NULL);
}

void pc_xr_mono_camera(void) {
    s_category = AURORA_XR_MONO;
    aurora_xr_camera(AURORA_XR_MONO, NULL);
}

/* Fighters past the flat camera's view. The game stops drawing them there
 * (ftLib_UpdateScreenVisibility) and shows a magnifier bubble at the screen
 * edge instead; the 3D view sees the whole arena, so in XR they keep drawing,
 * pulsing between see-through and solid to say they're out where the game
 * would have lost them. The visibility test still runs and sets its flags,
 * so nothing the game reads changes. Presentation only: the 3D view draws
 * them, the flat frame's camera can't see them anyway. MELEE_XR_OFFSCREEN=0
 * keeps the game's behavior; =2 draws every fighter this way (to measure it). */
static int offscreen_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* v = getenv("MELEE_XR_OFFSCREEN");
        mode = v == NULL ? 1 : *v == '0' ? 0 : *v == '2' ? 2 : 1;
    }
    return mode;
}

bool pc_xr_fighter_offscreen_forced(void) { return offscreen_mode() == 2 && aurora_xr_active(); }

bool pc_xr_fighter_offscreen_begin(void) {
    const int on = offscreen_mode() != 0;
    if (!on || !aurora_xr_active() || s_category != AURORA_XR_WORLD) {
        return false;
    }
    /* 1.5 pulses a second, from 30% to 85% (dithered). MELEE_XR_OFFSCREEN_OPACITY:
     * a fixed opacity instead (a measurement). */
    const double t = (double)SDL_GetTicksNS() * 1e-9;
    static float fixed_opacity = -2.f;
    if (fixed_opacity == -2.f) {
        const char* v = getenv("MELEE_XR_OFFSCREEN_OPACITY");
        fixed_opacity = v != NULL ? (float)atof(v) : -1.f;
    }
    const float opacity = fixed_opacity >= 0.f ? fixed_opacity
                                               : 0.575f + 0.275f * (float)sin(t * 2.0 * 3.14159265 * 1.5);
    float planes[AURORA_XR_MAX_CLIPS][4], fades[AURORA_XR_MAX_CLIPS];
    const int n = s_follow != NULL && s_follow_placed && pc_xr_mixed_reality() ? follow_planes(planes, fades) : 0;
    aurora_xr_world_clips4_fade((const float(*)[4])planes, fades, n, opacity);
    s_probe.offscreen++;
    return true;
}

void pc_xr_fighter_offscreen_end(void) { base_clip(); }

/* Each fighter takes the room light at its middle (its ECB's centre) for
 * all its draws: one probe that moves with it, blended from the room
 * light's grid (aurora_xr_light_probe), so it's lit as one body wherever it
 * is on a big stage. */
static bool s_fighter_light;

void pc_xr_fighter_light_begin(HSD_GObj* gobj) {
    if (!aurora_xr_active() || s_category != AURORA_XR_WORLD) {
        return;
    }
    Vec3 c;
    ftLib_GetECBCenter(gobj, &c);
    const float p[3] = { c.x, c.y, c.z };
    aurora_xr_light_probe(p);
    s_fighter_light = true;
}

void pc_xr_fighter_light_end(void) {
    if (s_fighter_light) {
        aurora_xr_light_probe(NULL);
        s_fighter_light = false;
    }
}

/* The magnifier bubble (and its arrow) goes to the flat frame only: in XR
 * the fighter itself stays in view. Its game state is untouched, including
 * the off-screen flag the damage tick reads. */
static int s_magnify_saved = -1;

bool pc_xr_magnify_begin(void) {
    if (!aurora_xr_active() || s_category != AURORA_XR_HUD) {
        return false;
    }
    s_magnify_saved = s_category;
    pc_xr_mono_camera();
    s_probe.magnified++;
    return true;
}

void pc_xr_magnify_end(void) {
    if (s_magnify_saved == AURORA_XR_HUD) {
        pc_xr_hud_camera();
    }
    s_magnify_saved = -1;
}

static bool has_joint_rules(int grkind, int map_id) {
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].grkind == grkind && s_rules[i].map_id == map_id && s_rules[i].jobj >= 0) {
            return true;
        }
    }
    for (int i = 0; i < MOVE_COUNT; i++) {
        if (s_moves[i].grkind == grkind && s_moves[i].map_id == map_id) {
            return true;
        }
    }
    return false;
}

/* Depth-first, in the order HSD_JObjDispAll draws: a joint, its children,
 * then its next sibling. */
static void walk_joints(HSD_JObj* jobj, int grkind, int map_id, int depth, bool visible, int move, int* index,
    bool log) {
    for (; jobj != NULL; jobj = jobj->next) {
        const int idx = (*index)++;
        bool vis = visible;
        int mv = move;
        for (int i = 0; i < MOVE_COUNT; i++) {
            if (s_moves[i].grkind == grkind && s_moves[i].map_id == map_id && s_moves[i].jobj == idx) {
                mv = i;
            }
        }
        MeshMask dobjs = {{0}}; /* meshes hidden while the joint is shown */
        for (int i = 0; i < s_rule_count; i++) {
            const PartRule* r = &s_rules[i];
            if (r->grkind != grkind || r->map_id != map_id || r->jobj != idx) {
                continue;
            }
            if (r->dobj < 0) {
                vis = r->show;
                dobjs = (MeshMask){{0}};
            } else {
                mask_set(&dobjs, r->dobj, !r->show);
            }
        }
        const bool some = mask_any(&dobjs);
        if ((!vis || some || mv >= 0) && s_hidden_joint_count < MAX_HIDDEN_JOINTS) {
            s_hidden_joints[s_hidden_joint_count++] = (HiddenJoint){jobj, !vis, dobjs, mv};
        }
        if (log) {
            int meshes = 0;
            if (union_type_dobj(jobj)) {
                for (HSD_DObj* d = jobj->u.dobj; d != NULL; d = d->next) {
                    meshes++;
                }
            }
            pc_log_line("xr: stage %d part %d joint %d depth %d %d meshes%s at %.0f,%.0f,%.0f -> %s", grkind,
                map_id, idx, depth, meshes, (jobj->flags & JOBJ_HIDDEN) ? " (hidden)" : "", jobj->mtx[0][3],
                jobj->mtx[1][3], jobj->mtx[2][3], vis ? (some ? "3D, some meshes hidden" : "3D") : "hidden");
        }
        walk_joints(jobj->child, grkind, map_id, depth + 1, vis, mv, index, log);
    }
}

static int compare_joint(const void* a, const void* b) {
    const uintptr_t x = (uintptr_t)((const HiddenJoint*)a)->jobj, y = (uintptr_t)((const HiddenJoint*)b)->jobj;
    return x < y ? -1 : x > y;
}

static const HiddenJoint* find_joint(HSD_JObj* jobj) {
    if (!s_joint_mode || s_hidden_joint_count == 0) {
        return NULL;
    }
    const HiddenJoint key = {jobj, 0, -1};
    return bsearch(&key, s_hidden_joints, (size_t)s_hidden_joint_count, sizeof s_hidden_joints[0], compare_joint);
}

/* MELEE_XR_JOINT_LOG: each part's joints, once, on the part's 60th draw so
 * its matrices are set up. */
static bool joint_log_due(int grkind, int map_id) {
    static unsigned char draws[64][128];
    unsigned char* n = &draws[(unsigned)grkind & 63u][(unsigned)map_id & 127u];
    if (*n > 60) {
        return false;
    }
    return ++*n == 60;
}

static int s_center_grkind = -1;
static bool s_clip_active;

static int s_xr_mode = -1; /* 1 mixed reality, 0 full VR */

static void xr_mode_init(void) {
    if (s_xr_mode < 0) {
        const char* m = getenv("MELEE_XR_MODE");
        s_xr_mode = m == NULL || strcmp(m, "vr") != 0;
        aurora_xr_set_passthrough(s_xr_mode == 1);
    }
}

bool pc_xr_mixed_reality(void) {
    xr_mode_init();
    return s_xr_mode == 1 && aurora_xr_active();
}

bool pc_xr_presenting(void) { return aurora_xr_active(); }

bool pc_xr_toggle_mode(void) {
    if (!aurora_xr_active()) {
        return false;
    }
    xr_mode_init();
    s_xr_mode = !s_xr_mode;
    aurora_xr_set_passthrough(s_xr_mode == 1);
    pc_log_line("xr: %s", s_xr_mode ? "mixed reality" : "full VR");
    return true;
}

/* One player wears the headset, and netplay sends port 1, so an external
 * gamepad plays as player one in XR: the newest one connected takes the
 * port, and when port 1 has none (a pad paired before the session began, or
 * port 1's pad unplugged), the lowest-port pad moves there. The headset's
 * own controllers merge into port 1's virtual pad (keyboard.c) either way.
 * Only pads aurora tracks count: SDL also lists devices aurora skips (Quest's
 * own input shows up as a gamepad and still holds SDL's first player slot). */
static void xr_gamepad_to_port1(s32 index) {
    /* Through PAD so controller_ports.dat agrees and aurora's saved
     * preferences don't move it back on the next hotplug. */
    PADSetPortForIndex((u32)index, 0);
    pc_log_line("xr: gamepad '%s' on port 1", PADGetName(0));
}

/* A tracked pad on no port at all: SDL numbers players by every gamepad it
 * sees, Quest's own devices included (two of them by 2026-10-15), and
 * the saved port preferences then clear a pad whose number lands on a port
 * they name: an 8BitDo ended up on no port, dead in game. */
static s32 xr_portless_pad(void) {
    for (u32 index = 0; index < PADCount(); index++) {
        bool placed = false;
        for (u32 port = 0; port < PAD_CHANMAX; port++) {
            if (PADGetIndexForPort(port) == (s32)index) {
                placed = true;
                break;
            }
        }
        if (!placed) {
            return (s32)index;
        }
    }
    return -1;
}

void pc_xr_gamepad_added(int instance) {
    if (!aurora_xr_active() || instance < 0) {
        return;
    }
    SDL_Gamepad* pad = SDL_GetGamepadFromID((SDL_JoystickID)instance);
    const int port = pad != NULL ? SDL_GetGamepadPlayerIndex(pad) : 0;
    if (port > 0 && port < PAD_CHANMAX) {
        const s32 index = PADGetIndexForPort((u32)port);
        if (index >= 0) {
            xr_gamepad_to_port1(index);
        }
    } else if (port < 0 || port >= PAD_CHANMAX) {
        const s32 index = xr_portless_pad();
        if (index >= 0) {
            xr_gamepad_to_port1(index);
        }
    }
}

static void xr_gamepad_fill_port1(void) {
    if (!aurora_xr_active() || PADGetIndexForPort(0) >= 0) {
        return;
    }
    for (u32 port = 1; port < PAD_CHANMAX; port++) {
        const s32 index = PADGetIndexForPort(port);
        if (index >= 0) {
            xr_gamepad_to_port1(index);
            return;
        }
    }
    const s32 index = xr_portless_pad();
    if (index >= 0) {
        xr_gamepad_to_port1(index);
    }
}

void pc_xr_poll_control(void) {
    xr_gamepad_fill_port1();
    static const char* path;
    static int frames;
    static char last[16];
    if (frames++ % 60 != 0) {
        return;
    }
    if (path == NULL) {
        path = getenv("MELEE_XR_CONTROL");
        if (path == NULL) {
            path = "";
        }
    }
    FILE* f = *path != '\0' ? fopen(path, "r") : NULL;
    if (f == NULL) {
        return;
    }
    char cmd[16] = {0};
    if (fgets(cmd, sizeof cmd, f) != NULL) {
        cmd[strcspn(cmd, "\r\n ")] = '\0';
    }
    fclose(f);
    if (strcmp(cmd, last) == 0) {
        return;
    }
    strcpy(last, cmd);
    const int want = strcmp(cmd, "mr") == 0 ? 1 : strcmp(cmd, "vr") == 0 ? 0 : -1;
    xr_mode_init();
    if (want >= 0 && want != s_xr_mode) {
        pc_xr_toggle_mode();
    }
}

/* The highest floor within the blast zones (world units), for the HUD;
 * NAN when the stage has none or its collision isn't loaded yet. The walk
 * is in mplib.c: the collision data is in the disc's byte order, which this
 * file isn't built to read on every platform (clang on Android). */
static int s_top_grkind = -1;
/* Every floor's x span within the blast zones (stage_top). */
static float s_floors_left = NAN, s_floors_right = NAN;

void pc_xr_stage_load(void) {
    s_top_grkind = -1;
    if (s_grade_dynamic) {
        dynamic_grade_reset(); /* the next fight starts out of the caves */
    }
    s_follow_placed = false; /* the next fight's window starts on its focus */
    /* And a held one holds again: a Home-Run retry reloads the same stage,
     * and its window started out released, following the camera. */
    s_follow_released = false;
}

static float stage_top(void) {
    float top, min_x, max_x;
    if (!mpLib_FloorExtent(Stage_GetBlastZoneLeftOffset(), Stage_GetBlastZoneRightOffset(),
                           Stage_GetBlastZoneBottomOffset(), Stage_GetBlastZoneTopOffset(), &top, &min_x, &max_x)) {
        return NAN;
    }
    /* The platform: floors within 3 units of the top floor's height. */
    float ptop, pleft, pright;
    if (mpLib_FloorExtent(Stage_GetBlastZoneLeftOffset(), Stage_GetBlastZoneRightOffset(), top - 3.f, top + 3.f, &ptop,
                          &pleft, &pright)) {
        s_platform_left = pleft;
        s_platform_right = pright;
    }
    s_floors_left = min_x;
    s_floors_right = max_x;
    pc_log_line("xr: stage %d top floor at y %.1f from x %.1f to %.1f; floors from x %.1f to %.1f; blast zones x %.0f to "
                "%.0f, y %.0f "
                "to %.0f",
                stage_info.grkind, top, s_platform_left, s_platform_right, min_x, max_x, Stage_GetBlastZoneLeftOffset(), Stage_GetBlastZoneRightOffset(),
                Stage_GetBlastZoneBottomOffset(), Stage_GetBlastZoneTopOffset());
    return top;
}

/* The stage's footprint for its shadow on the real surface below
 * (s_footprints), once its floors are in. */
static void stage_footprint(int grkind) {
    if (s_follow != NULL) {
        aurora_xr_set_stage_footprint(0.f, 0.f, 0.f, 0.f, 0.f);
        return;
    }
    for (int i = 0; i < FOOTPRINT_COUNT; i++) {
        const StageFootprint* f = &s_footprints[i];
        if (f->grkind == grkind) {
            aurora_xr_set_stage_footprint(f->xmin, f->xmax, f->zmin, f->zmax, f->ybottom);
            return;
        }
    }
    float bottom = -40.f;
    bool cut = false;
    for (int i = 0; i < CLIP_COUNT; i++) {
        const float* p = s_clips[i].plane;
        if (s_clips[i].grkind == grkind && p[0] == 0.f && p[1] == 1.f && p[2] == 0.f && (!cut || -p[3] < bottom)) {
            bottom = -p[3]; /* CLIP_BELOW: kept above y = -d */
            cut = true;
        }
    }
    aurora_xr_set_stage_footprint(s_floors_left, s_floors_right, -30.f, 30.f, bottom);
}

bool pc_xr_stage_part_begin(int grkind, int map_id, int layer, HSD_JObj* root) {
    if (s_category != AURORA_XR_WORLD) {
        return false;
    }
    if (grkind != s_center_grkind) {
        s_center_grkind = grkind;
        float c[3] = {0.f, 0.f, 0.f};
        float scale = 1.f;
        for (int i = 0; i < PLACEMENT_COUNT; i++) {
            if (s_placements[i].grkind == grkind && grkind != 0) {
                c[0] = s_placements[i].x;
                c[1] = s_placements[i].y;
                c[2] = s_placements[i].z;
                scale = s_placements[i].scale;
            }
        }
        const char* env = getenv("MELEE_XR_CENTER");
        if (env != NULL) {
            sscanf(env, "%f,%f,%f", &c[0], &c[1], &c[2]);
        }
        env = getenv("MELEE_XR_STAGE_SCALE");
        if (env != NULL) {
            scale = (float) atof(env);
        }
        aurora_xr_set_stage(grkind, c[0], c[1], c[2], scale);
        room_grade(grkind);
        s_top_grkind = -1;
        follow_stage(grkind);
    }
    if (grkind != s_top_grkind) {
        /* Until the stage's collision is in (then once per fight). A
         * followed stage's floors pass through the window: the HUD keeps
         * its fixed height there. */
        const float top = stage_top();
        aurora_xr_set_stage_top(s_follow != NULL ? NAN : top);
        if (!isnan(top)) {
            s_top_grkind = grkind;
            stage_footprint(grkind);
            pc_xr_place_stage_ready(grkind);
        }
    }
    s_clip_active = false;
    if (!pc_xr_mixed_reality()) {
        return false; /* full VR: the whole stage, unmoved and unclipped */
    }
    if (getenv("MELEE_XR_CLIP_LOG") != NULL) {
        static unsigned n;
        if (n++ < 40) {
            pc_log_line("xr: part begin stage %d part %d layer %d", grkind, map_id, layer);
        }
    }
    float planes[AURORA_XR_MAX_CLIPS][4];
    float fades[AURORA_XR_MAX_CLIPS];
    int n_planes = s_follow != NULL && s_follow_placed ? follow_planes(planes, fades) : 0;
    for (int i = 0; i < CLIP_COUNT && n_planes < AURORA_XR_MAX_CLIPS; i++) {
        if (s_clips[i].grkind == grkind && s_clips[i].map_id == map_id && grkind != 0) {
            memcpy(planes[n_planes], s_clips[i].plane, sizeof planes[0]);
            fades[n_planes++] = s_clips[i].fade;
        }
    }
    /* MELEE_XR_CLIP="<grkind>:<map_id>:<y>[:<fade>],...": try clip heights;
     * MELEE_XR_CLIPZ the same for a cut behind z; MELEE_XR_CLIPP
     * "<grkind>:<map_id>:<a>:<b>:<c>:<d>[:<fade>];..." any plane (kept where
     * ax+by+cz+d >= 0). Any env plane replaces the built-in ones (up to
     * AURORA_XR_MAX_CLIPS). */
    float env_planes[AURORA_XR_MAX_CLIPS][4];
    float env_fades[AURORA_XR_MAX_CLIPS] = {0.f};
    int env_n = 0;
    const char* env = getenv("MELEE_XR_CLIP");
    while (env != NULL && *env != '\0') {
        int gk, id;
        float y, fade = 0.f;
        if (sscanf(env, "%d:%d:%f:%f", &gk, &id, &y, &fade) >= 3 && gk == grkind && id == map_id && env_n < AURORA_XR_MAX_CLIPS) {
            const float plane[4] = {0.f, 1.f, 0.f, -y};
            memcpy(env_planes[env_n], plane, sizeof plane);
            env_fades[env_n++] = fade;
        }
        env = strchr(env, ',');
        env = env != NULL ? env + 1 : NULL;
    }
    for (const char* z = getenv("MELEE_XR_CLIPZ"); z != NULL && *z != '\0';) {
        int gk, id;
        float v, fade = 0.f;
        if (sscanf(z, "%d:%d:%f:%f", &gk, &id, &v, &fade) >= 3 && gk == grkind && id == map_id && env_n < AURORA_XR_MAX_CLIPS) {
            const float plane[4] = {0.f, 0.f, 1.f, -v};
            memcpy(env_planes[env_n], plane, sizeof plane);
            env_fades[env_n++] = fade;
        }
        z = strchr(z, ',');
        z = z != NULL ? z + 1 : NULL;
    }
    for (const char* q = getenv("MELEE_XR_CLIPP"); q != NULL && *q != '\0';) {
        int gk, id;
        float pl[4], fade = 0.f;
        if (sscanf(q, "%d:%d:%f:%f:%f:%f:%f", &gk, &id, &pl[0], &pl[1], &pl[2], &pl[3], &fade) >= 6 &&
            gk == grkind && id == map_id && env_n < AURORA_XR_MAX_CLIPS) {
            memcpy(env_planes[env_n], pl, sizeof pl);
            env_fades[env_n++] = fade;
        }
        q = strchr(q, ';');
        q = q != NULL ? q + 1 : NULL;
    }
    if (env_n > 0) {
        memcpy(planes, env_planes, sizeof planes);
        memcpy(fades, env_fades, sizeof fades);
        n_planes = env_n;
    }
    const float opacity = level_opacity(grkind, map_id, root);
    if (n_planes > 0 || (opacity > 0.f && opacity < 1.f)) {
        aurora_xr_world_clips4_fade((const float(*)[4])planes, fades, n_planes, opacity);
        s_clip_active = true;
    }
    const bool visible = part_visible(grkind, map_id, layer) && opacity > 0.f;
    const bool log = s_log_joints && joint_log_due(grkind, map_id);
    if (has_joint_rules(grkind, map_id) || log) {
        s_hidden_joint_count = 0;
        int index = 0;
        walk_joints(root, grkind, map_id, 0, visible, -1, &index, log);
        qsort(s_hidden_joints, (size_t)s_hidden_joint_count, sizeof s_hidden_joints[0], compare_joint);
        s_joint_mode = true;
        return false;
    }
    if (visible) {
        return false;
    }
    /* Not re-drawn in 3D. The flat frame still draws it where a pass is
     * read (the stadium screen's feed copies it), and drops it elsewhere. */
    aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
    return true;
}

void pc_xr_stage_part_end(bool hidden) {
    s_joint_mode = false;
    if (s_clip_active) {
        base_clip();
        s_clip_active = false;
    }
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
}

/* Stage particles left out of the 3D view, by GrKind and the particle's
 * bank, id and texture group (HSD_Particle bank/idnum/texGroup; -1 is any:
 * ids number instances, so most rules want -1 there, and a bank's texture
 * groups tell its kinds apart). Extend with MELEE_XR_PTCL
 * "<grkind>:<bank>:<id>[:<texgroup>],..."; MELEE_XR_PTCL_LOG lists each one
 * drawn, MELEE_XR_PTCL_STATS=1 what each bank and texture group draws. */
typedef struct {
    int grkind;
    int bank;
    int id;
    int tex_group;
} ParticleRule;

static const ParticleRule s_particles[] = {
    /* Peach's Castle: the water's splashes, left floating behind the castle
     * once its water is hidden (texture group 2, y 2..10, the stage's only
     * particles in a fight: MELEE_XR_PTCL_STATS; a 3D dump with and
     * without). */
    {0x02, 30, -1, 2},
    /* Kongo Jungle: the river's splashes (bank 30 is the stage's own), left
     * floating once the river is hidden. */
    {0x04, 30, -1, -1},
    /* Battlefield and Final Destination: the background's drifting sparks
     * and streaks, ~3600 behind the stage (MELEE_XR_PTCL_LOG). */
    {0x24, 30, -1, -1},
    {0x25, 30, -1, -1},
    /* Corneria: the sea's spray, thrown up along two lines under the Great
     * Fox for part of its flight (its zako generators, effects 30000 and
     * 30004: texture groups 0 and 1, y -24..38). The sea isn't shown, and
     * while it lasts the spray draws over ten times a KO's particles by
     * summed size squared (a flat desktop tally). */
    {0x0E, 30, -1, 0},
    {0x0E, 30, -1, 1},
    /* Fountain of Dreams: the falling stars and their trails (texture group
     * 1, y -178..299; MELEE_XR_PTCL_STATS). The fountain's water is groups 0,
     * 2 and 3 of the same bank and stays. */
    {0x0C, 30, -1, 1},
    /* Pokémon Stadium: the fireworks over the stands, all match long: rockets
     * rising from the stands (texture group 0, y 0..420), their sparks (1,
     * y 90..500, ~50 a frame) and bursts (2). Nothing else on the stage draws
     * from these groups (MELEE_XR_PTCL_STATS; a 3D dump with and without). */
    {0x10, 30, -1, 0},
    {0x10, 30, -1, 1},
    {0x10, 30, -1, 2},
};
#define PARTICLE_COUNT ((int)(sizeof s_particles / sizeof s_particles[0]))

/* MELEE_XR_PTCL, parsed once: this runs for every particle drawn. */
#define PARTICLE_ENV_MAX 32
static ParticleRule s_env_particles[PARTICLE_ENV_MAX];
static int s_env_particle_count = -1;

static void load_particle_rules(void) {
    s_env_particle_count = 0;
    for (const char* q = getenv("MELEE_XR_PTCL"); q != NULL && *q != '\0';) {
        int g, b, n, t = -1;
        if (sscanf(q, "%d:%d:%d:%d", &g, &b, &n, &t) >= 3 && s_env_particle_count < PARTICLE_ENV_MAX) {
            s_env_particles[s_env_particle_count++] = (ParticleRule){g, b, n, t};
        }
        q = strchr(q, ',');
        q = q != NULL ? q + 1 : NULL;
    }
}

static bool particle_rule(int grkind, int bank, int id, int tex_group) {
    if (s_env_particle_count < 0) {
        load_particle_rules();
    }
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        const ParticleRule* r = &s_particles[i];
        if (r->grkind == grkind && r->bank == bank && (r->id < 0 || r->id == id) &&
            (r->tex_group < 0 || r->tex_group == tex_group)) {
            return true;
        }
    }
    for (int i = 0; i < s_env_particle_count; i++) {
        const ParticleRule* r = &s_env_particles[i];
        if (r->grkind == grkind && r->bank == bank && (r->id < 0 || r->id == id) &&
            (r->tex_group < 0 || r->tex_group == tex_group)) {
            return true;
        }
    }
    return false;
}

/* MELEE_XR_PTCL_STATS=1: every 10 s, the bank/texture groups the 3D view
 * drew the most of, by count and by the sum of size squared (a stand-in for
 * the pixels they cover), with a separate tally for the second after each
 * KO (pc_xr_heavy_effect). */
typedef struct {
    unsigned count;
    double area;
    float y_min, y_max;
} ParticleStat;
static ParticleStat s_ptcl_stats[2][64][64];
static Uint64 s_ptcl_stats_since, s_ko_window_until;

typedef struct {
    int bank, tex;
    ParticleStat s;
} ParticleTop;

static int compare_particle_top(const void* a, const void* b) {
    const double x = ((const ParticleTop*)a)->s.area, y = ((const ParticleTop*)b)->s.area;
    return (x < y) - (x > y); /* largest first */
}

static void particle_stats_log(int which) {
    static ParticleTop all[64 * 64];
    int n = 0;
    for (int b = 0; b < 64; b++) {
        for (int t = 0; t < 64; t++) {
            if (s_ptcl_stats[which][b][t].count != 0) {
                all[n++] = (ParticleTop){b, t, s_ptcl_stats[which][b][t]};
            }
        }
    }
    qsort(all, (size_t)n, sizeof all[0], compare_particle_top);
    for (int i = 0; i < n && i < 8; i++) {
        pc_log_line("xr: particles%s stage %d bank %d tex %d: %u drawn, area %.0f, y %.0f..%.0f",
            which ? " after KOs," : ",", s_center_grkind, all[i].bank, all[i].tex, all[i].s.count, all[i].s.area,
            all[i].s.y_min, all[i].s.y_max);
    }
    memset(s_ptcl_stats[which], 0, sizeof s_ptcl_stats[which]);
}

static void particle_stats(int bank, int tex_group, float size, const float pos[3]) {
    const Uint64 now = SDL_GetTicks();
    if (s_ptcl_stats_since == 0) {
        s_ptcl_stats_since = now;
    }
    for (int which = 0; which < 2; which++) {
        if (which == 1 && now > s_ko_window_until) {
            break;
        }
        ParticleStat* s = &s_ptcl_stats[which][bank & 63][tex_group & 63];
        if (s->count == 0 || pos[1] < s->y_min) {
            s->y_min = pos[1];
        }
        if (s->count == 0 || pos[1] > s->y_max) {
            s->y_max = pos[1];
        }
        s->count++;
        s->area += (double)size * size;
    }
    if (now - s_ptcl_stats_since >= 10000) {
        s_ptcl_stats_since = now;
        particle_stats_log(0);
        particle_stats_log(1);
    }
}

/* A launched fighter's knockback smoke (efAsync 0x408: a puff every few
 * frames while it flies, ftCo_Damage.c) stays out of the 3D view, VR too,
 * like a KO's own particles: the trail of a hard launch, most of all on its
 * way to a KO, took frames with the explosion's (the player's report).
 * MELEE_XR_KB_SMOKE_SHOW=1 keeps it. Only the trail's: its puffs are told
 * from other dust by their generator's instance number (HSD_Particle
 * idnum), noted as each puff starts. A number is forgotten once 2048 generators
 * have been made since (a puff lives about a second; numbers come around
 * after 65280), and no sooner during a pause, which makes none. */
#define KB_SMOKE_SLOTS 128 /* about two seconds of four fighters' trails */
static unsigned short s_kb_smoke[KB_SMOKE_SLOTS];
static int s_kb_smoke_next, s_kb_smoke_count;

void pc_xr_kb_smoke(unsigned idnum) {
    s_kb_smoke[s_kb_smoke_next] = (unsigned short)idnum;
    s_kb_smoke_next = (s_kb_smoke_next + 1) % KB_SMOKE_SLOTS;
    if (s_kb_smoke_count < KB_SMOKE_SLOTS) {
        s_kb_smoke_count++;
    }
}

/* How many generators were made from `idnum`'s up to `now`, both counting
 * 256 to 65535 and around. */
static unsigned generator_age(unsigned idnum, unsigned now) {
    return now >= idnum ? now - idnum : now + (65536u - 256u) - idnum;
}

static bool kb_smoke(int idnum) {
    if (s_kb_smoke_count == 0) {
        return false;
    }
    const unsigned now = pc_ps_generator_serial();
    for (int i = 0; i < s_kb_smoke_count; i++) {
        if (s_kb_smoke[i] == (unsigned)idnum && generator_age((unsigned)idnum, now) < 2048) {
            return true;
        }
    }
    return false;
}

/* A stage particle's marker is out (pc_xr_particle_begin). */
static bool s_ptcl_stage;
extern unsigned int aurora_draw_tag;
void GXInsertDebugMarker(const char* label);

bool pc_xr_particle_begin(int bank, int id, int tex_group, float size, const float pos[3]) {
    if (s_category != AURORA_XR_WORLD || s_center_grkind <= 0) {
        return false;
    }
    static int log = -1, hide_all = -1, stats = -1;
    if (stats < 0) {
        stats = getenv("MELEE_XR_PTCL_STATS") != NULL;
    }
    if (stats) {
        particle_stats(bank, tex_group, size, pos);
    }
    if (log < 0) {
        log = getenv("MELEE_XR_PTCL_LOG") != NULL;
        /* MELEE_XR_PTCL_TEST_HIDE=1: every particle out of 3D and the flat
         * frame, to measure what particles cost (a test knob). */
        hide_all = getenv("MELEE_XR_PTCL_TEST_HIDE") != NULL;
    }
    if (hide_all) {
        aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
        return true;
    }
    /* A KO's own particles (the common bank's texture groups 23-25), on
     * every stage and in VR too, as the explosion's ring and beam: after a
     * KO they drew ~15x their usual area, 1.6M size^2 a second, and with
     * the ring and beam gone they were what still took the eye pass to
     * 10-18 ms (MELEE_XR_PTCL_STATS, four CPUs on Brinstar).
     * MELEE_XR_KO_PTCL_SHOW=1 keeps them. */
    static int ko_show = -1;
    if (ko_show < 0) {
        ko_show = getenv("MELEE_XR_KO_PTCL_SHOW") != NULL;
    }
    if (!ko_show && bank == 0 && tex_group >= 23 && tex_group <= 25) {
        aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
        return true;
    }
    static int smoke_show = -1;
    if (smoke_show < 0) {
        smoke_show = getenv("MELEE_XR_KB_SMOKE_SHOW") != NULL;
    }
    if (!smoke_show && bank == 0 && kb_smoke(id)) {
        aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
        return true;
    }
    if (log) {
        static unsigned char seen[256][64];
        unsigned char* f = &seen[bank & 255][(id & 511) >> 3];
        if (!(*f & (1u << (id & 7)))) {
            *f |= 1u << (id & 7);
            pc_log_line("xr: stage %d particle %d:%d at %.0f,%.0f,%.0f", s_center_grkind, bank, id, pos[0], pos[1],
                        pos[2]);
        }
    }
    if (!pc_xr_mixed_reality() || !particle_rule(s_center_grkind, bank, id, tex_group)) {
        /* The stage's own particles (bank 30, the stage's effect bank: its
         * water, waterfalls, dust) draw in the effects' GX link, which the
         * room light leaves out; marked as the stage's (link 2) they are lit
         * by the room with the stage (Fountain of Dreams' water stayed lit
         * in a dark room). */
        if (bank == 30) {
            GXInsertDebugMarker("L2");
            s_ptcl_stage = true;
        }
        return false;
    }
    aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
    return true;
}

void pc_xr_heavy_effect(void) {
    s_ko_window_until = SDL_GetTicks() + 1000; /* MELEE_XR_PTCL_STATS */
    s_probe.ko++;
    aurora_xr_heavy_effect();
}

bool pc_xr_world_still_begin(void) {
    if (s_category != AURORA_XR_WORLD) {
        return false;
    }
    aurora_xr_world_still(true);
    return true;
}

void pc_xr_world_still_end(bool began) {
    if (began) {
        aurora_xr_world_still(false);
    }
}

int pc_xr_blast_clip_begin(void) {
    static int off = -1;
    if (off < 0) {
        off = getenv("MELEE_XR_BLAST_CLIP_OFF") != NULL; /* the whole explosion, for comparison */
    }
    if (s_category != AURORA_XR_WORLD || s_center_grkind <= 0) {
        return 0;
    }
    s_probe.blast++;
    if (off) {
        return 1;
    }
    float planes[AURORA_XR_MAX_CLIPS][4];
    float fades[AURORA_XR_MAX_CLIPS] = {0.f};
    int n = s_follow != NULL && s_follow_placed && pc_xr_mixed_reality() ? follow_planes(planes, fades) : 0;
    const float box[4][4] = {
        {1.f, 0.f, 0.f, -Stage_GetBlastZoneLeftOffset()},
        {-1.f, 0.f, 0.f, Stage_GetBlastZoneRightOffset()},
        {0.f, 1.f, 0.f, -Stage_GetBlastZoneBottomOffset()},
        {0.f, -1.f, 0.f, Stage_GetBlastZoneTopOffset()},
    };
    for (int i = 0; i < 4 && n < AURORA_XR_MAX_CLIPS; i++) {
        memcpy(planes[n], box[i], sizeof box[i]);
        fades[n++] = 0.f;
    }
    aurora_xr_world_clips4((const float(*)[4])planes, fades, n);
    return 2;
}

void pc_xr_blast_clip_end(int began) {
    if (began == 2) {
        base_clip();
    }
}

void pc_xr_particle_end(bool hidden) {
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
    if (s_ptcl_stage) {
        /* Back to the link drawing (gobj.c's marker). */
        char buf[12];
        snprintf(buf, sizeof(buf), "L%u", aurora_draw_tag < 0xF000u ? aurora_draw_tag : 99u);
        GXInsertDebugMarker(buf);
        s_ptcl_stage = false;
    }
}

int pc_xr_jobj_begin(HSD_JObj* jobj) {
    const HiddenJoint* h = find_joint(jobj);
    if (h == NULL) {
        return 0;
    }
    if (h->all) {
        aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
        return CHANGED_MONO;
    }
    if (h->move < 0) {
        return 0;
    }
    const MoveRule* m = &s_moves[h->move];
    const float s = m->scale;
    const float t[3][4] = {
        {s, 0, 0, m->to[0] - s * m->pivot[0]},
        {0, s, 0, m->to[1] - s * m->pivot[1]},
        {0, 0, s, m->to[2] - s * m->pivot[2]},
    };
    aurora_xr_world_transform(t);
    return CHANGED_MOVE;
}

int pc_xr_dobj_begin(HSD_JObj* jobj, int dobj_index) {
    const HiddenJoint* h = find_joint(jobj);
    /* A wholly hidden joint is already out of the 3D view. */
    if (h == NULL || h->all || !mask_test(&h->dobjs, dobj_index)) {
        return 0;
    }
    aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
    return CHANGED_MONO;
}

void pc_xr_jobj_end(int changed) {
    if (changed & CHANGED_MONO) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
    if (changed & CHANGED_MOVE) {
        aurora_xr_world_transform(NULL);
    }
}

#endif
