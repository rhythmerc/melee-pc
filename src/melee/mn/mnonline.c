#include "mnonline.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include <melee/gm/gmonlinemode.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/gobj.h>

#include "pc/net_match.h"
#include "pc/slippi/slp_user.h"
#include "pc/xr_place.h"

/* Main Menu > Online Mode. Each entry selects its GM_ONLINE lobby mode. The
 * labels are drawn text (the menu's own labels are textures with no room for
 * new words), spelled as Slippi spells them. */

static const char* const online_labels[] = {
    "Slippi",
    "LAN",
};

/* <= 44 chars: that is what fits the bottom bar at mnmain.c's font size */
static const char* const online_descriptions[] = {
    "Play over the internet with Slippi.",
    "Play another player on your local network.",
};

/* Online Mode > Slippi: signed out there is only Login; signed in it is
 * Unranked, Direct, Place Stage (only with a headset presenting) and Logout. */
enum { row_unranked, row_direct, row_place, row_logout, row_count };
static const char* const slippi_in_labels[row_count] = { "Unranked", "Direct", "Place Stage",
                                                         "Logout" };
static const char* const slippi_in_descriptions[row_count] = {
    "Play unranked matches.",
    "Play a specific person.",
    "Place the stage in your room.",
    "Sign out of your Slippi account.",
};

/* The signed-in menu's row at `selection`, or -1. */
static int slippiRow(int selection)
{
    const bool place = pc_xr_place_available();
    if (selection < 0 || selection >= (place ? row_count : row_count - 1)) {
        return -1;
    }
    return !place && selection >= row_place ? selection + 1 : selection;
}

static const char* notice;

static int slippi_built_count;

int mnOnline_SlippiCount(void)
{
    return pc_net_match_slippi_ready() ? (pc_xr_place_available() ? row_count : row_count - 1) : 1;
}

void mnOnline_SlippiBuilt(void)
{
    slippi_built_count = mnOnline_SlippiCount();
}

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_ONLINE) {
        return "Online Mode";
    }
    if (kind == MENU_KIND_SETTINGS && selection == SEL_SETTINGS_PLACE) {
        return "Place Stage";
    }
    if (kind == MENU_KIND_SETTINGS && selection == SEL_SETTINGS_FIGHT) {
        return "Fight Controls";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_labels))
    {
        return online_labels[selection];
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI) {
        if (!pc_net_match_slippi_ready()) {
            return selection == 0 ? "Login" : NULL;
        }
        if (slippiRow(selection) >= 0) {
            return slippi_in_labels[slippiRow(selection)];
        }
    }
    return NULL;
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_ONLINE) {
        return "Play against other players over the network.";
    }
    if (kind == MENU_KIND_SETTINGS && selection == SEL_SETTINGS_PLACE) {
        return slippi_in_descriptions[row_place];
    }
    if (kind == MENU_KIND_SETTINGS && selection == SEL_SETTINGS_FIGHT) {
        return "Choose what each button does in a fight.";
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_descriptions))
    {
        return online_descriptions[selection];
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI) {
        if (!pc_net_match_slippi_ready()) {
            return selection == 0 ? "Sign in to your Slippi account." : NULL;
        }
        if (slippiRow(selection) >= 0) {
            return slippi_in_descriptions[slippiRow(selection)];
        }
    }
    return NULL;
}

const char* mnOnline_TakeNotice(void)
{
    const char* s = notice;
    notice = NULL;
    return s;
}

static void enterOnline(OnlineKind kind)
{
    MenuExitData* data = gm_GetCurrentSceneExitData();
    sfxForward();
    gmOnline_SetKind(kind);
    data->pending_mode = GM_ONLINE;
    gm_801A4B60();
}

/* Unranked, Direct or LAN with a headset presenting places the stage first
 * if Place Stage hasn't run this launch, even with a stage spot found in the
 * room: online there is no hold before each fight, so this is the player's
 * only chance (src/pc/xr_place.h). Then it goes on to the mode. */
static void enterAfterPlacing(OnlineKind kind)
{
    if (pc_xr_place_needed_online()) {
        gmOnline_SetKindAfterPlace(kind);
        enterOnline(ONLINE_KIND_PLACE);
        return;
    }
    enterOnline(kind);
}

void mnOnline_PlaceFromOptions(void)
{
    gmOnline_SetPlaceFromOptions(true);
    enterOnline(ONLINE_KIND_PLACE);
}

/* Up/Down wrap over `count` entries. */
static void moveHover(u32 buttons, int count)
{
    if (buttons & MenuInput_Up) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + count - 1) % count;
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + 1) % count;
    }
}

/// @brief Online menu think, after mn_8022D594: Slippi or LAN
void mnOnline_Think(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        switch (mn_804A04F0.hovered_selection) {
        case SEL_ONLINE_SLIPPI:
            sfxForward();
            mn_80229894(MENU_KIND_ONLINE_SLIPPI, SEL_SLIPPI_UNRANKED, 1);
            break;
        case SEL_ONLINE_LAN:
            enterAfterPlacing(ONLINE_KIND_LAN);
            break;
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_MAIN, SEL_MAIN_ONLINE, 3);
    } else {
        moveHover(buttons, (int) ARRAY_SIZE(online_labels));
    }
}

/// @brief Online Mode > Slippi think: Login, or the Slippi modes and Logout
void mnOnline_SlippiThink(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);
    bool signed_in = pc_net_match_slippi_ready();

    mn_804A04F0.buttons = buttons;
    if (mnOnline_SlippiCount() != slippi_built_count) {
        /* The account changed under the menu (a sign-out, or a sign-in that
         * finished): build it again with the rows that now apply. */
        mn_80229894(MENU_KIND_ONLINE_SLIPPI, 0, 1);
        return;
    }
    if (buttons & MenuInput_Confirm) {
        if (!signed_in) {
            enterOnline(ONLINE_KIND_LOGIN);
            return;
        }
        switch (slippiRow(mn_804A04F0.hovered_selection)) {
        case row_unranked:
            enterAfterPlacing(ONLINE_KIND_UNRANKED);
            break;
        case row_direct:
            enterAfterPlacing(ONLINE_KIND_DIRECT);
            break;
        case row_place:
            enterOnline(ONLINE_KIND_PLACE);
            break;
        case row_logout:
            sfxForward();
            pc_slippi_account_forget();
            pc_net_match_slippi_refresh();
            break; /* the account check above rebuilds the menu next frame */
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_ONLINE, SEL_ONLINE_SLIPPI, 3);
    } else {
        moveHover(buttons, mnOnline_SlippiCount());
    }
}
