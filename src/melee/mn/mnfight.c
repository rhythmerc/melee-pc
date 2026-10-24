#include "mnfight.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include "mnslippi.h"
#include "pc/fight_menu.h"
#include <melee/it/forward.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbspdisplay.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/sislib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The panel's joint with the sub-screen's title in its tab (MenMainPanel:
 * 84 is "Options", 85 the screen's own, Erase Data's in our frames). */
#define PANEL_TITLE_JOINT 85

/* The screen's (src/pc/fight_menu.cpp, RmlUi) way in and out, in frames:
 * hidden while the Options rows leave (as Sound's panel waits, its cursor =
 * 0x14), a fade and a rise in, and a fade out while the rows come back. */
enum {
    FIGHT_HOLD_FRAMES = 20,
    FIGHT_IN_FRAMES = 10,
    FIGHT_OUT_FRAMES = 8,
};

typedef enum FightPhase {
    FIGHT_HOLD,
    FIGHT_IN,
    FIGHT_SHOWN,
    FIGHT_OUT,
} FightPhase;

typedef struct FightMenu {
    HSD_Text* hint;
    char hint_text[128]; ///< what the hint box shows
    FightPhase phase;
    int frames; ///< into the phase
} FightMenu;

static HSD_GObj* mnFight_gobj;

/* The hint box's text: literal, sized and placed as the PC descriptions in
 * mnmain.c's (mnOnline_Description). */
static void mnFight_SetHint(FightMenu* menu, const char* text)
{
    HSD_Text* t;

    if (menu->hint != NULL) {
        HSD_SisLib_803A5CC4(menu->hint);
    }
    t = HSD_SisLib_803A6754(0, mn_804D6BB4);
    menu->hint = t;
    t->pos_x = -9.0f;
    t->pos_y = 9.3f;
    t->pos_z = 17.0f;
    t->default_kerning = 1;
    t->font_size.x = 0.022f;
    t->font_size.y = 0.022f;
    HSD_SisLib_803A6B98(t, 0.0f, 0.0f, "%s", text);
}

static void mnFight_FreeHint(FightMenu* menu)
{
    if (menu->hint != NULL) {
        HSD_SisLib_803A5CC4(menu->hint);
        menu->hint = NULL;
    }
}

/* The page's own GObj: the screen's fades and the hint box. It outlives the
 * page by the fade out (the Options rows already back), then goes. */
static void mnFight_Frame(HSD_GObj* gobj)
{
    FightMenu* menu = HSD_GObjGetUserData(gobj);
    const char* hint;

    if (mn_804A04F0.cur_menu != MENU_KIND_SETTINGS_FIGHT && menu->phase != FIGHT_OUT) {
        /* Options writes its own hint as its rows come back. */
        mnFight_FreeHint(menu);
        menu->phase = FIGHT_OUT;
        menu->frames = 0;
    }
    menu->frames++;
    switch (menu->phase) {
    case FIGHT_HOLD:
        if (menu->frames >= FIGHT_HOLD_FRAMES) {
            pc_fight_menu_show(true);
            pc_fight_menu_presence(0.0f);
            menu->phase = FIGHT_IN;
            menu->frames = 0;
        }
        return;
    case FIGHT_IN:
        pc_fight_menu_presence((f32) menu->frames / FIGHT_IN_FRAMES);
        if (menu->frames >= FIGHT_IN_FRAMES) {
            menu->phase = FIGHT_SHOWN;
        }
        break;
    case FIGHT_SHOWN:
        break;
    case FIGHT_OUT:
        pc_fight_menu_presence(1.0f - (f32) menu->frames / FIGHT_OUT_FRAMES);
        if (menu->frames >= FIGHT_OUT_FRAMES) {
            pc_fight_menu_show(false);
            mnFight_gobj = NULL;
            HSD_GObjFree(gobj);
        }
        return;
    }
    hint = pc_fight_menu_hint();
    if (hint != NULL && strcmp(hint, menu->hint_text) != 0) {
        snprintf(menu->hint_text, sizeof menu->hint_text, "%s", hint);
        mnFight_SetHint(menu, menu->hint_text);
    }
}

static void mnFight_Think(HSD_GObj* gobj)
{
    u64 events;

    if (mn_804D6BC8.cooldown != 0) {
        Menu_DecrementAnimTimer();
        return;
    }
    events = Menu_GetAllInputs();
    /* The screen takes the menus' input; B with nothing to back out of
     * leaves the page. */
    if (events & MenuInput_Up) {
        pc_fight_menu_input(PC_FIGHT_MENU_UP);
    } else if (events & MenuInput_Down) {
        pc_fight_menu_input(PC_FIGHT_MENU_DOWN);
    } else if (events & MenuInput_Left) {
        pc_fight_menu_input(PC_FIGHT_MENU_LEFT);
    } else if (events & MenuInput_Right) {
        pc_fight_menu_input(PC_FIGHT_MENU_RIGHT);
    } else if (events & MenuInput_Confirm) {
        pc_fight_menu_input(PC_FIGHT_MENU_CONFIRM);
    }
    if ((events & MenuInput_Back) && !pc_fight_menu_input(PC_FIGHT_MENU_BACK)) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_SETTINGS, SEL_SETTINGS_FIGHT, 3);
        return;
    }
}

void mnFight_Init(void)
{
    HSD_GObj* gobj;
    HSD_GObjProc* proc;
    FightMenu* menu;

    /* Back in before the last fade out finished: that one goes now. */
    if (mnFight_gobj != NULL) {
        pc_fight_menu_show(false);
        mnFight_FreeHint(HSD_GObjGetUserData(mnFight_gobj));
        HSD_GObjFree(mnFight_gobj);
        mnFight_gobj = NULL;
    }
    mn_804D6BC8.cooldown = 5;
    mn_804A04F0.prev_menu = mn_804A04F0.cur_menu;
    mn_804A04F0.cur_menu = MENU_KIND_SETTINGS_FIGHT;
    mn_804A04F0.hovered_selection = 0;

    gobj = GObj_Create(HSD_GOBJ_CLASS_ITEM, 7U, 0x80U);
    mnFight_gobj = gobj;
    menu = HSD_MemAlloc(sizeof(FightMenu));
    menu->hint = NULL;
    menu->hint_text[0] = '\0';
    menu->phase = FIGHT_HOLD;
    menu->frames = 0;
    GObj_InitUserData(gobj, 0U, HSD_Free, menu);
    proc = HSD_GObj_SetupProc(gobj, mnFight_Frame, 0U);
    proc->flags_3 = HSD_GObj_804D783C;
    mnFight_SetHint(menu, "Choose what each button does in a fight.");

    proc = HSD_GObj_SetupProc(GObj_Create(0, 1, 0x80), mnFight_Think, 0);
    proc->flags_3 = HSD_GObj_804D783C;
}

static HSD_JObj* mnFight_titleJoint;

/* The stock titles are slanted (italic) textures on the tab's joint; the
 * font is upright. Before the glyphs go out, the text's position matrix is
 * the joint's world rotation with an x-by-y shear, about the joint's origin
 * (as mnmain.c's mn_PcLabelLean leans the PC row labels). */
static void mnFight_TitleItalic(void* gobj_)
{
    HSD_JObj* jobj = mnFight_titleJoint;
    Mtx view, rot, out;
    float a[3], len;
    int c, r;

    (void) gobj_;
    if (jobj == NULL) {
        return;
    }
    a[0] = jobj->mtx[0][3];
    a[1] = jobj->mtx[1][3];
    a[2] = jobj->mtx[2][3];
    for (c = 0; c < 3; c++) {
        len = sqrtf(jobj->mtx[0][c] * jobj->mtx[0][c] + jobj->mtx[1][c] * jobj->mtx[1][c] +
                    jobj->mtx[2][c] * jobj->mtx[2][c]);
        if (len < 1e-4f) {
            return;
        }
        for (r = 0; r < 3; r++) {
            rot[r][c] = jobj->mtx[r][c] / len;
        }
    }
    {
        /* About the stock titles' slant: x moves right a fifth of y. */
        const float k = 0.2f;
        Mtx shear = { { 1.0f, k, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
        Mtx turned;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                turned[r][c] = rot[r][0] * shear[0][c] + rot[r][1] * shear[1][c] + rot[r][2] * shear[2][c];
            }
        }
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                rot[r][c] = turned[r][c];
            }
            rot[r][3] = a[r] - (rot[r][0] * a[0] + rot[r][1] * a[1] + rot[r][2] * a[2]);
        }
    }
    HSD_CObjGetViewingMtx(HSD_CObjGetCurrent(), view);
    MTXConcat(view, rot, out);
    GXLoadPosMtxImm(out, 0);
}

/* The tab's sub-title of a PC page, or NULL for a stock kind. */
static const char* mnFight_PageTitle(MenuKind kind)
{
    switch (kind) {
    case MENU_KIND_SETTINGS_FIGHT:
        return "Fight Controls";
    case MENU_KIND_SLIPPI_PAGE:
        return mnSlippi_Title();
    default:
        return NULL;
    }
}

/* The tab's title on a PC page: the borrowed page's texture hidden, ours in
 * SIS text where it sits, following it through the tab's slide in and out. */
static HSD_Text* mnFight_title;
static char mnFight_titleShown[32];
static bool mnFight_titleHidden;

void mnFight_PanelTitle(HSD_JObj* panel, MenuKind cur, MenuKind prev, bool leaving)
{
    /* Sampled from the stock sub-titles: a light grey. */
    static const GXColor colour = { 0x9C, 0x9C, 0x9C, 0xFF };
    const char* text = mnFight_PageTitle(cur);
    HSD_JObj* joint = NULL;
    Vec3 pos;

    if (text == NULL && leaving) {
        text = mnFight_PageTitle(prev);
    }
    lb_80011E24(panel, &joint, PANEL_TITLE_JOINT, -1);
    if (joint == NULL) {
        return;
    }
    mnFight_titleJoint = joint;
    if (mnFight_title != NULL && (text == NULL || strcmp(text, mnFight_titleShown) != 0)) {
        HSD_SisLib_803A5CC4(mnFight_title);
        mnFight_title = NULL;
    }
    if (text == NULL) {
        if (mnFight_titleHidden) {
            HSD_JObjClearFlagsAll(joint, JOBJ_HIDDEN);
            mnFight_titleHidden = false;
        }
        return;
    }
    HSD_JObjSetFlagsAll(joint, JOBJ_HIDDEN);
    mnFight_titleHidden = true;
    if (mnFight_title == NULL) {
        snprintf(mnFight_titleShown, sizeof mnFight_titleShown, "%s", text);
        mnFight_title = HSD_SisLib_803A6754(0, mn_804D6BB4);
        mnFight_title->default_kerning = 1;
        /* Condensed a little: 14 letters in the tab that holds "Erase Data". */
        mnFight_title->font_size.x = 0.031f;
        mnFight_title->font_size.y = 0.038f;
        HSD_SisLib_803A6B98(mnFight_title, 0.0f, 0.0f, "%s", mnFight_titleShown);
        mnFight_title->render_callback = mnFight_TitleItalic;
    }
    lb_8000B1CC(joint, NULL, &pos);
    /* The stock sub-titles start a little in from the tab's slanted left edge
     * (Name Entry's, measured: ~12 px of 1280); the joint's origin is on it. */
    mnFight_title->pos_x = pos.x + 0.4f;
    mnFight_title->pos_y = -pos.y - 16.0f * mnFight_title->font_size.y;
    mnFight_title->pos_z = pos.z;
    HSD_SisLib_803A74F0(mnFight_title, 0, (GXColor*) &colour);
}

void mnFight_PanelTitleForget(void)
{
    mnFight_title = NULL;
    mnFight_titleHidden = false;
}
