# Adding menu rows and pages to Melee's menus

How to add a PC-only row to one of Melee's menus, and a PC-only page (an
Options sub-screen) behind it, so it looks and moves like the stock ones.
Written 2026-10-24 while building Options > Fight Controls
(`src/melee/mn/mnfight.c`), the first PC page. It follows the stock Sound
page (`mnsound.c`). Use this when moving launcher settings into the game.

## How a stock menu screen is built

Three pieces are on screen at once, and they belong to different code:

| What you see | Where it lives |
|---|---|
| Grid backdrop, swirl, lighting | `MenMainBack_Top` (`mn_80229B2C`), always running |
| Gold frame, its tabs ("Options / Sound"), the inner outlines, the silver hint box | The panel, `MenMainPanel_Top` (`mn_80229DC0`, think `fn_80229BF4`) |
| The page's own controls (rows, sliders, pictures) | The page's model, e.g. `MenMainConSo_Top` for Sound, and its GObjs |

- **The panel is one model for every menu.** Joint `0x29` (41) plays a frame range per menu kind from `mn_803EAE8C[kind]`: `[0]` going in, `[1]` coming out, `[2]` the idle loop.
- **Those frames carry the menu's look.** The tab's title texture, and the inner outlines with their labels (Sound's CHANNEL and VOLUME), come with the range. A new kind that borrows a range gets that screen's look, title included.
- **The panel's title pieces** sit at y 9.55, z 17.2 in panel space:
  - joint 84 is the menu's title ("Options");
  - joint 85 is the page's sub-title ("Sound", "Erase Data"), its own mesh;
  - joint 43 is the frame and tab shape;
  - joint 42's subtree is the frame, tabs and hint box border, but not the titles.
- **The menu state** is `mn_804A04F0` (`MenuFlow`): `cur_menu`, `prev_menu`, `hovered_selection`, `entering_menu`. The panel's think compares `cur_menu` with what it last showed, and plays the in or out range on its own.

## Adding a row to a stock menu

The Options rows Place Stage and Fight Controls are the worked examples.

1. **A selection id** in `src/melee/mn/forward.h` (`SEL_SETTINGS_PLACE = 6`, `SEL_SETTINGS_FIGHT = 7`), after the stock ones.
2. **`mnmain.c`, the menu's tables**, each one entry longer under `#ifdef TARGET_PC`:
   - the preview animation array (`mn_803EB4F8` for Options): borrow a stock row's frames;
   - the description SIS indices (`mn_803EB684`): any stock id; the literal text replaces it.
3. **The row count:** where the menu is built (`cur_menu == MENU_KIND_SETTINGS` near `mn_803EB6B0[...].selection_count`), set it to include the new rows.
   - A row that is sometimes unavailable is **locked** in `mn_80229938`, not left out of the count. Locked rows are hidden and skipped, as stock `SEL_SETTINGS_3` is. That keeps later rows' ids fixed: Place Stage (XR only) is locked flat, and Fight Controls after it still shows.
4. **Text:** `mnOnline_Label` and `mnOnline_Description` (`mnonline.c`) return literal strings for PC rows.
   - `mn_8022B3A0` hides the row's label texture, and `mn_UpdatePcLabels` draws SIS text over the slot, leaning as the joint does.
   - The description goes through the literal path in `mnmain.c`.
5. **Dispatch:** the menu's think (`mn_8022D104` for Options) gets a `case` for the id.
6. **Room:** the panel has 10 row joints (`mn_803EAE68`) and `mn_pcLabelPart` holds 8 labels. Options shows 7 rows in XR, 6 flat.

## Adding a page (a sub-screen)

`mnfight.c` is the template.

1. **A menu kind** in `forward.h`, after the PC ones (`MENU_KIND_SETTINGS_FIGHT = 36`). Then bump `MENU_KIND_TABLE_LEN` in `mnmain.c` to the last PC kind + 1. Every table indexed by kind needs an entry:
   - `mn_803EAE8C` (the panel's frames): borrow the stock page whose inside fits best. Fight Controls uses Erase Data's (4200-4399), whose inside is plain, because Erase Data's rows are its own model. The PC Online menus borrow VS. Mode's (400-599), so they keep that title.
   - `mn_803EB6B0` (`MenuKindData`): `{ NULL, 0, NULL, 0x01, NULL }` for a page that runs its own GObjs, as the stock sub-screens do.
   - The colour switch (`return 3;` for the Options family, near `MENU_KIND_SETTINGS_ERASE`): add the kind to its family's case, or its lights change colour.
2. **Entering** (`mnFight_Init`, after `mnSound_8024A09C`):
   - set `mn_804D6BC8.cooldown = 5`, `prev_menu = cur_menu`, `cur_menu = <kind>`, `hovered_selection = 0`;
   - create the page's GObj (class `HSD_GOBJ_CLASS_ITEM`, plink 7) and a think GObj (`GObj_Create(0, 1, 0x80)`, `flags_3 = HSD_GObj_804D783C`);
   - the menu's own think calls this after `sfxForward()`, then `HSD_GObjFree(gp)`. The Options rows leave by themselves.
3. **Waiting for the panel:** stock pages keep their content hidden for 20 frames while the old rows leave (Sound's `cursor = 0x14`).
4. **Input:** `Menu_GetAllInputs()` in the think, after `if (mn_804D6BC8.cooldown) { Menu_DecrementAnimTimer(); return; }`.
   - It merges the keyboard, Touch, SDL gamepads and laser clicks, with the menus' repeat.
   - `MenuInput_Back` / `_Confirm` / `_Up` and so on; sounds are `sfxBack()`, `sfxForward()`, `sfxMove()`.
5. **Going back:** `sfxBack(); mn_804A04F0.entering_menu = 0; mn_80229894(<parent kind>, <row>, 3);`. That rebuilds the parent in its fly-in state with `<row>` lit, frees the think GObj, and starts the parent's think.
   - The page's own GObj frees itself on its next frame, once `cur_menu` isn't its kind (`mnFight_Frame`). It also frees its SIS texts there.
6. **The tab's title:** the borrowed range shows the borrowed page's title. Each frame the panel's think calls `mnFight_PanelTitle`:
   - it hides joint 85 and draws SIS text where it is (`lb_8000B1CC` for the joint's position), so the text rides the tab's slide;
   - "ours" means `cur_menu == kind`, or leaving it (`state == 1` with `prev_menu == kind`), so the title stays up through the slide out;
   - after that it puts joint 85 back (clears `JOBJ_HIDDEN`), since the next menu needs it.
   - **Matching the stock titles:** they are slanted grey textures. The SIS font is upright, so the text's `render_callback` (`mnFight_TitleItalic`) loads a position matrix: the joint's world rotation with an x-by-y shear (k 0.2), about the joint's origin. That's the same trick `mn_PcLabelLean` uses for the row labels.
   - Colour (0x9C, 0x9C, 0x9C). Size 0.031 x 0.038, a little condensed, so 14 letters fit the tab that holds "Erase Data". At 0.05 it runs past the tab.
7. **The hint box:** literal SIS text, placed as the PC descriptions are: `HSD_SisLib_803A6754(0, mn_804D6BB4)`, pos (-9, 9.3, 17), kerning 1, size 0.022. Stock pages use `Menu_InitCenterText(menu, sis_id)` for SIS strings.
8. **The build:** `cmake/game_sources.manifest` lists every game source. A new `src/melee/...` file must be added there in sorted order, or configure fails with "game source manifest drift". `-DMELEE_UPDATE_SOURCE_MANIFEST=ON` rewrites it.

## Finding joints in a model

A temporary probe, never committed. In the model's think, under `TARGET_PC`:
- log every joint once: index, translation, dobj, child, next, flags, via `lb_80011E24(root, &j, i, -1)` for i up to 200;
- hide the indices in an env var with `HSD_JObjSetFlagsAll(j, JOBJ_HIDDEN)`.

Hide subtrees first, then single joints, and screenshot each run. Joints are numbered depth first, so a sibling of a big subtree can sit at index 80+. Stock page thinks clear `JOBJ_HIDDEN` on their own models after their wait, so hide in the think each frame, not at load.

## Checking it against a stock page

Drive the desktop build to the page and capture a frame sequence:
- **Driving:** `MELEE_BOOT_SCENE=menu MELEE_NO_ATTRACT=1 MELEE_KEY_FIFO=<fifo>`. FIFO lines are "Down 120", "X 120" (A), "Z 120" (B).
- **Capturing:** `xdotool search --name '^Xelee$'` for the window, then `import -window <id>` in a loop, with `XAUTHORITY=/run/user/1000/xauth_*`.

Put the same capture of the stock page beside it (Options > Sound, for an Options page) and compare going in and coming back. The FIFO keys reach the game's pads only, never RmlUi.

## Native widgets or an RmlUi overlay

Both are fine. Pick per page:
- **Native** (borrowed models, SIS text) for what Melee already has a widget for: rows of choices, a value changed with left and right (the Rules rows), lists (Erase Data). These get Melee's font, cursor, sounds and fly-ins for free. Most launcher settings are this kind.
- **An RmlUi overlay** on a native page for layouts Melee has no widget for. Fight Controls is the example: a device picture, leader lines, two columns of boxes and a pop-up list. RmlUi draws at the window's resolution, sharper than SIS text at the game's internal resolution, and its look is a stylesheet edit.

**An overlay must stay part of the page.** In `src/pc/fight_menu.cpp` with `mnfight.c`:
- **No backdrop, title or footer of its own.** The body is transparent; the page's frame, tab and hint box are Melee's. The menu keeps running underneath: no `VIWaitForRetrace` hold, no `PADBlockInput`.
- **Inside the frame.** The layout is scaled into the frame's inside, measured on screen as fractions of the picture (Erase Data's frames: 12.3-87.8% across, 17.0-84.2% down), and never over the hint box.
- **The page's input.** The page's think reads `Menu_GetAllInputs()` and hands it over (`pc_fight_menu_input`). The overlay reads no devices itself, or a press would count twice. A B with nothing to close returns false, and the page goes back.
- **Lasers and the mouse** move RmlUi's pointer (`aurora_xr_screen_pointer` into `ProcessMouseMove`) for hover. A laser's click already reaches the page as the menus' A (`xr_pointer.c`), so the overlay ignores RmlUi's own click while a laser points.
- **The hint box stays native.** The overlay says what the cursor is on (`pc_fight_menu_hint`), and the page writes it as SIS text when it changes.
- **Frame-synced animation.** The page's GObj counts frames and sets the overlay's presence each frame (`pc_fight_menu_presence`, 0 to 1), never with CSS timers:
  - it is hidden for the page's 20-frame wait;
  - it fades and rises in over 10 frames;
  - it fades out over 8 while the parent's rows come back. The page's GObj outlives `cur_menu` changing for those frames, so it frees its hint at once, since the parent writes its own, and goes after the fade.
  - Entering again mid-fade ends the old fade at once.
  - Input waits for presence 1.
  - Small pop-ups (the list) animate per frame in the overlay's update.
