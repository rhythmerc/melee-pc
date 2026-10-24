#include "mnslippi.h"

#include "forward.h"
#include "inlines.h"
#include "mnfight.h"
#include "mnmain.h"
#include "mnnamenew.h"
#include "types.h"
#include "pc/pc.h"
#include "pc/slippi_menu.h"
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_unsplit.h>
#include <melee/gm/types.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lblanguage.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/sislib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The page's way in and out, in frames, as Fight Controls': hidden while the
 * panel's tab slides in, then a fade and a rise. */
enum {
    SLIPPI_HOLD_FRAMES = 20,
    SLIPPI_IN_FRAMES = 10,
};

static struct {
    bool active;
    HSD_Archive* ext; ///< MnExtAll, for the keyboard
    bool page_up;     ///< the frame, backdrop and overlay are built
    int frames;       ///< since the page was built
    HSD_Text* hint;
    char hint_text[128];
} s;

static void mnSlippi_Settle(void);

const char* mnSlippi_Title(void)
{
    return s.active ? pc_slippi_menu_title() : "Login";
}

/* The hint box: literal text, placed as the PC descriptions are (mnfight.c). */
static void mnSlippi_SetHint(const char* text)
{
    HSD_Text* t;

    if (s.hint != NULL) {
        HSD_SisLib_803A5CC4(s.hint);
    }
    t = HSD_SisLib_803A6754(0, mn_804D6BB4);
    s.hint = t;
    t->pos_x = -9.0f;
    t->pos_y = 9.3f;
    t->pos_z = 17.0f;
    t->default_kerning = 1;
    t->font_size.x = 0.022f;
    t->font_size.y = 0.022f;
    HSD_SisLib_803A6B98(t, 0.0f, 0.0f, "%s", text);
}

/* The menu's backdrop, camera and panel, as mnMain_Scene_OnEnter builds them,
 * the panel sliding in from the Slippi menu's frame to ours. */
static void mnSlippi_BuildPage(void)
{
    void* dp_[8];

    mn_804D6BB8 = lbArchive_LoadSymbols(
        "MnMaAll", &dp_[0], "MenMainBack_Top_joint", &dp_[1], "MenMainBack_Top_animjoint", &dp_[2],
        "MenMainBack_Top_matanim_joint", &dp_[3], "MenMainBack_Top_shapeanim_joint", &MenMain_cam,
        "ScMenMain_cam_int1_camera", &MenMain_lights, "ScMenMain_scene_lights", &MenMain_fog, "ScMenMain_fog",
        &dp_[4], "MenMainPanel_Top_joint", &dp_[5], "MenMainPanel_Top_animjoint", &dp_[6],
        "MenMainPanel_Top_matanim_joint", &dp_[7], "MenMainPanel_Top_shapeanim_joint", 0);
    DP_SET(MenMainBack_Top.joint, dp_[0]);
    DP_SET(MenMainBack_Top.animjoint, dp_[1]);
    DP_SET(MenMainBack_Top.matanim_joint, dp_[2]);
    DP_SET(MenMainBack_Top.shapeanim_joint, dp_[3]);
    DP_SET(MenMainPanel_Top.joint, dp_[4]);
    DP_SET(MenMainPanel_Top.animjoint, dp_[5]);
    DP_SET(MenMainPanel_Top.matanim_joint, dp_[6]);
    DP_SET(MenMainPanel_Top.shapeanim_joint, dp_[7]);
    if (lbLang_IsSavedLanguageUS()) {
        HSD_SisLib_803A62A0(0, "SdMenu.usd", "SIS_MenuData");
    } else {
        HSD_SisLib_803A62A0(0, "SdMenu.dat", "SIS_MenuData");
    }
    mn_804A04F0.cur_menu = MENU_KIND_ONLINE_SLIPPI;
    mn_804A04F0.prev_menu = MENU_KIND_ONLINE_SLIPPI;
    /* As Name Entry builds its screen: the camera, the text canvas the tab
     * and hint draw on (mn_8022BEDC), the backdrop, then the panel. */
    mn_8022C304();
    mn_8022BCF8();
    mn_8022BEDC(mn_8022BE34());
    mn_80229B2C();
    mn_80229DC0();
    /* The panel's think sees the change and plays our range in. */
    mn_804A04F0.cur_menu = MENU_KIND_SLIPPI_PAGE;
    mn_804A04F0.entering_menu = 1;
    mn_804D6BC8.cooldown = 5;

    s.page_up = true;
    s.frames = 0;
    s.hint = NULL;
    s.hint_text[0] = '\0';
}

/* Everything the page built goes, for the keyboard's own screen. */
static void mnSlippi_TearDown(void)
{
    if (!s.page_up) {
        return;
    }
    pc_slippi_menu_show(false);
    s.hint = NULL; /* freed with every other text: */
    HSD_SisLib_803A5E70();
    mn_8022EBDC();
    mnFight_PanelTitleForget();
    s.page_up = false;
}

static bool mnSlippi_KeyboardDone(bool confirmed, const char* text, void* user)
{
    (void) user;
    pc_slippi_menu_keyboard_done(confirmed, text);
    mnSlippi_Settle();
    return true;
}

/* MELEE_SLIPPI_TYPED="a@b.c;secret": a test's answers to the keyboards, in
 * order, so a desktop run can walk the pages without typing on the screen
 * ("<back>" is Back). Each answer is used once. */
static bool mnSlippi_ScriptedAnswer(void)
{
    static int used;
    const char* script = getenv("MELEE_SLIPPI_TYPED");
    char answer[100];
    int i = 0;
    const char* p;

    if (script == NULL || script[0] == '\0') {
        return false;
    }
    for (p = script; *p != '\0' && i < used; p++) {
        i += *p == ';';
    }
    if (i < used || (*p == '\0' && used > 0 && p[-1] != ';')) {
        return false;
    }
    {
        size_t n = strcspn(p, ";");
        if (n >= sizeof answer) {
            n = sizeof answer - 1;
        }
        memcpy(answer, p, n);
        answer[n] = '\0';
    }
    used++;
    pc_log_line("slippi pages: scripted keyboard answer %d", used);
    pc_slippi_menu_keyboard_done(strcmp(answer, "<back>") != 0, answer);
    return true;
}

/* What comes next: Melee's keyboard, the way out, or the page. */
static void mnSlippi_Settle(void)
{
    PcSlippiKeyboard kb;

    if (pc_slippi_menu_leave()) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    while (pc_slippi_menu_keyboard(&kb)) {
        if (mnSlippi_ScriptedAnswer()) {
            continue;
        }
        mnSlippi_TearDown();
        mnNameNew_PcOpen(s.ext, kb.initial, kb.max_len, kb.mask, kb.shiftable, kb.prompt,
                         mnSlippi_KeyboardDone, NULL, false, NULL);
        return;
    }
    if (pc_slippi_menu_leave()) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    if (!s.page_up && !mnNameNew_PcActive()) {
        mnSlippi_BuildPage();
    }
}

void mnSlippi_Begin(bool register_)
{
    memset(&s, 0, sizeof s);
    s.active = true;
    /* What the keyboard needs: the menu text and its archive. */
    if (lbLang_IsSavedLanguageUS()) {
        HSD_SisLib_803A62A0(0, "SdMenu.usd", "SIS_MenuData");
        s.ext = lbArchive_LoadArchive("MnExtAll.usd");
    } else {
        HSD_SisLib_803A62A0(0, "SdMenu.dat", "SIS_MenuData");
        s.ext = lbArchive_LoadArchive("MnExtAll.dat");
    }
    pc_slippi_menu_begin(register_ ? PC_SLIPPI_MENU_REGISTER : PC_SLIPPI_MENU_LOGIN);
    mnSlippi_Settle();
}

void mnSlippi_Frame(void)
{
    u32 buttons;
    const char* hint;

    if (!s.active) {
        return;
    }
    mnSlippi_Settle();
    if (!s.page_up) {
        return;
    }
    s.frames++;
    if (s.frames == SLIPPI_HOLD_FRAMES) {
        pc_slippi_menu_show(true);
        pc_slippi_menu_presence(0.0f);
    } else if (s.frames > SLIPPI_HOLD_FRAMES) {
        const int in = s.frames - SLIPPI_HOLD_FRAMES;
        pc_slippi_menu_presence(in >= SLIPPI_IN_FRAMES ? 1.0f : (f32) in / SLIPPI_IN_FRAMES);
    }
    hint = pc_slippi_menu_hint();
    if (hint != NULL && strcmp(hint, s.hint_text) != 0) {
        snprintf(s.hint_text, sizeof s.hint_text, "%s", hint);
        mnSlippi_SetHint(s.hint_text);
    }

    if (mn_804D6BC8.cooldown != 0) {
        Menu_DecrementAnimTimer();
        return;
    }
    buttons = mn_80229624(4);
    if (buttons & MenuInput_Up) {
        pc_slippi_menu_input(PC_SLIPPI_IN_UP);
    } else if (buttons & MenuInput_Down) {
        pc_slippi_menu_input(PC_SLIPPI_IN_DOWN);
    } else if (buttons & MenuInput_Left) {
        pc_slippi_menu_input(PC_SLIPPI_IN_LEFT);
    } else if (buttons & MenuInput_Right) {
        pc_slippi_menu_input(PC_SLIPPI_IN_RIGHT);
    }
    if (buttons & MenuInput_StartButton) {
        pc_slippi_menu_input(PC_SLIPPI_IN_START);
    } else if (buttons & MenuInput_Confirm) {
        pc_slippi_menu_input(PC_SLIPPI_IN_CONFIRM);
    } else if (buttons & MenuInput_Back) {
        pc_slippi_menu_input(PC_SLIPPI_IN_BACK);
    }
    if (buttons & MenuInput_XButton) {
        pc_slippi_menu_input(PC_SLIPPI_IN_X);
    } else if (buttons & MenuInput_YButton) {
        pc_slippi_menu_input(PC_SLIPPI_IN_Y);
    }
}

void mnSlippi_End(void)
{
    if (!s.active) {
        return;
    }
    pc_slippi_menu_end();
    if (s.ext != NULL) {
        lbArchive_80016EFC(s.ext);
        s.ext = NULL;
    }
    mnFight_PanelTitleForget();
    s.active = false;
}
