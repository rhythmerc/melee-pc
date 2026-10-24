/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Fight Controls screen (fight_menu.h). */
#include "fight_menu.h"

#include "fight_map.h"
#include "launcher.h"
#include "pc.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <aurora/rmlui.hpp>
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <dolphin/pad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void lbAudioAx_80024030(int);

namespace {

enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };

// The layout, in px, scaled to fit the panel frame's inside (its aspect, a
// little wider than 16:9).
constexpr float kScreenW = 1120.f, kScreenH = 660.f;
constexpr float kBoxW = 260.f, kBoxH = 56.f, kStub = 20.f, kEdge = 12.f;
constexpr float kTop = 8.f, kBottom = 652.f; // the boxes' columns
constexpr float kArtScale = 0.8f;            // the pictures were drawn for 640 px across
constexpr float kResetW = 220.f, kResetH = 40.f;
constexpr int kFixed = -1; // the pause button's box: shown, never mapped
constexpr int kListPop = 6; // frames the list takes to open

struct InputName {
    const char* file;  // fight_map.c's name, and the picture's
    const char* label;
};
const InputName kTouchInputs[PAD_FIGHT_TOUCH_COUNT] = {
    {"left_stick", "Left stick"},   {"right_stick", "Right stick"}, {"left_stick_click", "Left stick click"},
    {"right_stick_click", "Right stick click"}, {"left_trigger", "Left trigger"}, {"right_trigger", "Right trigger"},
    {"a", "A"},                     {"b", "B"},                     {"x", "X"},
    {"y", "Y"},                     {"left_grip", "Left grip"},     {"right_grip", "Right grip"},
};
const InputName kGamepadInputs[PAD_FIGHT_PAD_COUNT] = {
    {"left_stick", "Left stick"},   {"right_stick", "Right stick"}, {"left_stick_click", "Left stick click"},
    {"right_stick_click", "Right stick click"}, {"left_trigger", "Left trigger"}, {"right_trigger", "Right trigger"},
    {"south", "A (bottom)"},        {"east", "B (right)"},          {"west", "X (left)"},
    {"north", "Y (top)"},           {"left_bumper", "Left bumper"}, {"right_bumper", "Right bumper"},
    {"dpad_up", "D-pad up"},        {"dpad_down", "D-pad down"},    {"dpad_left", "D-pad left"},
    {"dpad_right", "D-pad right"},
};

const char* action_label(PcFightAction a) {
    static const char* const names[PC_FIGHT_ACTION_COUNT] = {
        "(none)", "Move", "Smash attack", "Shield", "Grab", "Jump", "Attack", "Special", "Taunt",
    };
    return a >= 0 && a < PC_FIGHT_ACTION_COUNT ? names[a] : "?";
}

// What the hint box says on a list entry.
const char* action_hint(PcFightAction a) {
    static const char* const hints[PC_FIGHT_ACTION_COUNT] = {
        "This input does nothing in a fight.",
        "Walk and run; tilt attacks with Attack.",
        "Smash attacks and aerials, by direction.",
        "Shield; press a trigger lightly for a light shield.",
        "Grab, and Z's other moves.",
        "Jump; again in the air for a double jump.",
        "Normal attacks.",
        "Special moves.",
        "Taunt.",
    };
    return a >= 0 && a < PC_FIGHT_ACTION_COUNT ? hints[a] : "";
}

struct Slot {
    int input = kFixed;
    std::string file, label;
    float ax = 0, ay = 0;                     // its line's end, on the screen
    float litX = 0, litY = 0, litW = 0, litH = 0; // its lit picture, on the screen
    float x = 0, y = 0;                       // its box
    bool left = true;
};

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

struct Menu : Rml::EventListener {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    PADFightDevice device = PAD_FIGHT_DEVICE_GAMEPAD;
    std::vector<Slot> slots;
    std::vector<int> column[2]; // slot indices, left then right, top to bottom
    int cursor = 0;             // a slot; kReset: the reset button
    static constexpr int kReset = -2;
    int lastColumn = 0;         // where up from Reset goes back to
    int listFor = -1;           // the slot whose list is open
    int listAge = 0;            // frames since it opened
    std::vector<PcFightAction> items;
    int itemCursor = 0;
    float presence = 0.f;       // the page's fade: 0 gone, 1 shown
    float scale = 1.f;
    Rml::Vector2i dims{0, 0};
    float placedPresence = -1.f; // what layout() last placed, to skip a frame with nothing new
    Rml::Vector2i placedDims{0, 0};
    bool pointerWas = false, pointerPressed = false;
    std::string hintText;

    bool load() {
        if (document != nullptr)
            return true;
        auto* context = aurora::rmlui::get_context();
        if (context == nullptr)
            return false;
        const std::string dir = pc_resources_dir();
        for (const char* face : {"menu.ttf", "menu-bold.ttf"})
            Rml::LoadFontFace(dir + face);
        Rml::LoadFontFace(dir + "menu-heavy.ttf", false, static_cast<Rml::Style::FontWeight>(800));
        document = context->LoadDocument(dir + "fight-controls.rml");
        if (document == nullptr) {
            pc_log_line("fight controls: could not load fight-controls.rml");
            return false;
        }
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Mouseover, this);
        return true;
    }

    static PADFightDevice active_device(PADFightDevice fallback) {
        switch (pc_pad_active_device()) {
        case PC_PAD_DEVICE_XR:
            return PAD_FIGHT_DEVICE_TOUCH;
        case PC_PAD_DEVICE_GAMEPAD:
            return PAD_FIGHT_DEVICE_GAMEPAD;
        default:
            return fallback;
        }
    }

    static bool xr_presenting() {
#ifdef AURORA_ENABLE_OPENXR
        return aurora_xr_active();
#else
        return false;
#endif
    }

    // The picture's anchors and lit rectangles (tools/fight_controls/make_art.py).
    bool read_art(const char* name, float& w, float& h) {
        const std::string path = std::string(pc_resources_dir()) + "fight-controls/" + name + ".txt";
        size_t size = 0;
        char* text = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
        if (text == nullptr) {
            pc_log_line("fight controls: no %s", path.c_str());
            return false;
        }
        w = h = 0;
        std::string all(text, size);
        SDL_free(text);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find('\n', pos);
            if (end == std::string::npos)
                end = all.size();
            const std::string line = all.substr(pos, end - pos);
            pos = end + 1;
            char key[64];
            float v[6];
            if (line.empty() || line[0] == '#')
                continue;
            if (std::sscanf(line.c_str(), "size %f %f", &v[0], &v[1]) == 2) {
                w = v[0], h = v[1];
            } else if (std::sscanf(line.c_str(), "%63s %f %f %f %f %f %f", key, &v[0], &v[1], &v[2], &v[3], &v[4],
                                   &v[5]) == 7)
            {
                for (Slot& s : slots) {
                    if (s.file == key) {
                        s.ax = v[0], s.ay = v[1], s.litX = v[2], s.litY = v[3], s.litW = v[4], s.litH = v[5];
                    }
                }
            }
        }
        return w > 0 && h > 0;
    }

    void build() {
        const bool touch = device == PAD_FIGHT_DEVICE_TOUCH;
        const char* name = touch ? "touch" : "gamepad";
        slots.clear();
        const int n = pc_fight_map_input_count(device);
        for (int i = 0; i < n; i++) {
            const InputName& in = touch ? kTouchInputs[i] : kGamepadInputs[i];
            Slot s;
            s.input = i;
            s.file = in.file;
            s.label = in.label;
            slots.push_back(s);
        }
        Slot pause;
        pause.file = touch ? "menu" : "start";
        pause.label = touch ? "Menu" : "Start";
        slots.push_back(pause);
        float w = 0, h = 0;
        if (!read_art(name, w, h))
            w = 640, h = 450;
        // The picture between the columns, a little high, clear of Reset.
        const float aw = w * kArtScale, ah = h * kArtScale;
        const float artX = std::round((kScreenW - aw) / 2), artY = std::round((kScreenH - kResetH - 24 - ah) / 2 + 14);
        for (Slot& s : slots) {
            s.left = s.ax < w / 2;
            s.ax = artX + s.ax * kArtScale, s.ay = artY + s.ay * kArtScale;
            s.litX = artX + s.litX * kArtScale, s.litY = artY + s.litY * kArtScale;
            s.litW *= kArtScale, s.litH *= kArtScale;
        }
        for (int c = 0; c < 2; c++) {
            column[c].clear();
            for (size_t i = 0; i < slots.size(); i++)
                if (slots[i].left == (c == 0))
                    column[c].push_back(static_cast<int>(i));
            std::stable_sort(column[c].begin(), column[c].end(),
                             [this](int a, int b) { return slots[a].ay < slots[b].ay; });
            const float step = (kBottom - kTop) / std::max<size_t>(column[c].size(), 1);
            for (size_t k = 0; k < column[c].size(); k++) {
                Slot& s = slots[column[c][k]];
                s.x = c == 0 ? kEdge : kScreenW - kEdge - kBoxW;
                s.y = std::round(kTop + k * step + (step - kBoxH) / 2);
            }
        }
        std::string label = "Touch controllers";
        if (!touch) {
            const char* pad = PADGetName(0);
            label = pad != nullptr && *pad ? std::string("Gamepad · ") + pad : "Gamepad";
        }
        std::string rml = "<div id=\"device\" style=\"left: " + fmt(artX) + "px; width: " + fmt(aw) + "px;\">" +
                          Rml::StringUtilities::EncodeRml(label) + "</div>";
        rml += "<img id=\"art\" src=\"fight-controls/" + std::string(name) + ".png\" style=\"left: " + fmt(artX) +
               "px; top: " + fmt(artY) + "px; width: " + fmt(aw) + "px; height: " + fmt(ah) +
               "px;\" /><img id=\"lit\" class=\"lit\" style=\"display: none;\" />";
        for (size_t i = 0; i < slots.size(); i++) {
            const Slot& s = slots[i];
            const std::string id = std::to_string(i);
            // The stub, out of the box's inner side; the run, to the part.
            const float sx = s.left ? s.x + kBoxW : s.x, sy = s.y + kBoxH / 2;
            const float ex = s.left ? sx + kStub : sx - kStub;
            const float dx = s.ax - ex, dy = s.ay - sy;
            const double len = std::sqrt(double(dx) * dx + double(dy) * dy);
            const double deg = std::atan2(double(dy), double(dx)) * 57.29577951308232;
            rml += "<div id=\"seg-" + id + "-a\" class=\"seg\" style=\"left: " + fmt(std::min(sx, ex)) +
                   "px; top: " + fmt(sy - 1.5f) + "px; width: " + fmt(kStub) + "px;\" />";
            rml += "<div id=\"seg-" + id + "-b\" class=\"seg\" style=\"left: " + fmt(ex) + "px; top: " +
                   fmt(sy - 1.5f) + "px; width: " + fmt(static_cast<float>(len)) + "px; transform: rotate(" +
                   fmt(static_cast<float>(deg)) + "deg);\" />";
            rml += "<div id=\"dot-" + id + "\" class=\"dot\" style=\"left: " + fmt(s.ax - 4.5f) + "px; top: " +
                   fmt(s.ay - 4.5f) + "px;\" />";
            rml += "<div id=\"slot-" + id + "\" class=\"slot" + std::string(s.input == kFixed ? " fixed" : "") +
                   "\" style=\"left: " + fmt(s.x) + "px; top: " + fmt(s.y) + "px;\"><span class=\"in\">" + s.label +
                   (s.input == kFixed ? " · fixed" : "") + "</span><span class=\"act\" id=\"act-" + id +
                   "\"></span></div>";
        }
        rml += "<button id=\"reset\" style=\"left: " + fmt((kScreenW - kResetW) / 2) + "px; top: " +
               fmt(kScreenH - kResetH - 6) + "px;\">Reset to defaults</button>";
        rml += "<div id=\"list\" style=\"display: none;\" />";
        document->GetElementById("board")->SetInnerRML(rml);
        listFor = -1;
        lastColumn = 0;
        cursor = column[0].empty() ? 0 : column[0].front();
        if (slots[cursor].input == kFixed)
            cursor = first_mappable();
        refresh();
    }

    int first_mappable() const {
        for (int c = 0; c < 2; c++)
            for (int i : column[c])
                if (slots[i].input != kFixed)
                    return i;
        return 0;
    }

    void refresh() {
        for (size_t i = 0; i < slots.size(); i++) {
            const Slot& s = slots[i];
            const std::string id = std::to_string(i);
            const bool on = static_cast<int>(i) == (listFor >= 0 ? listFor : cursor);
            auto* box = document->GetElementById("slot-" + id);
            if (box == nullptr)
                continue;
            box->SetClass("cursor", on && s.input != kFixed);
            for (const char* part : {"-a", "-b"})
                if (auto* e = document->GetElementById("seg-" + id + part))
                    e->SetClass("on", on);
            if (auto* e = document->GetElementById("dot-" + id))
                e->SetClass("on", on);
            auto* act = document->GetElementById("act-" + id);
            if (s.input == kFixed) {
                act->SetInnerRML("Pause");
                continue;
            }
            const PcFightAction a = pc_fight_map_get(device, s.input);
            act->SetInnerRML(action_label(a));
            act->SetClass("unset", a == PC_FIGHT_NONE);
            box->SetClass("changed", a != pc_fight_map_default(device, s.input));
        }
        if (auto* reset = document->GetElementById("reset"))
            reset->SetClass("cursor", cursor == kReset && listFor < 0);
        // The lit part: the slot under the cursor's.
        const int lit = listFor >= 0 ? listFor : cursor;
        if (auto* img = document->GetElementById("lit")) {
            if (lit >= 0 && lit < static_cast<int>(slots.size())) {
                const Slot& s = slots[lit];
                const char* name = device == PAD_FIGHT_DEVICE_TOUCH ? "touch" : "gamepad";
                img->SetAttribute("src", std::string("fight-controls/") + name + "-" + s.file + ".png");
                img->SetProperty("left", fmt(s.litX) + "px");
                img->SetProperty("top", fmt(s.litY) + "px");
                img->SetProperty("width", fmt(s.litW) + "px");
                img->SetProperty("height", fmt(s.litH) + "px");
                img->SetProperty("display", "block");
            } else {
                img->SetProperty("display", "none");
            }
        }
        refresh_list();
    }

    void refresh_list() {
        auto* list = document->GetElementById("list");
        if (list == nullptr)
            return;
        if (listFor < 0) {
            list->SetProperty("display", "none");
            return;
        }
        const Slot& s = slots[listFor];
        const PcFightAction current = pc_fight_map_get(device, s.input);
        const PcFightAction def = pc_fight_map_default(device, s.input);
        std::string rml = "<div class=\"head\">" + s.label + "</div>";
        for (size_t j = 0; j < items.size(); j++) {
            std::string cls = "item";
            if (items[j] == PC_FIGHT_NONE)
                cls += " none";
            if (static_cast<int>(j) == itemCursor)
                cls += " cursor";
            if (items[j] == current)
                cls += " current";
            rml += "<div id=\"item-" + std::to_string(j) + "\" class=\"" + cls + "\"><span>" +
                   action_label(items[j]) + "</span>" +
                   (items[j] == def && def != PC_FIGHT_NONE ? "<span class=\"tag\">default</span>" : "") + "</div>";
        }
        list->SetInnerRML(rml);
        // Beside the box, toward the picture, centred on it, on the screen.
        const float h = 40.f + 44.f * static_cast<float>(items.size());
        const float top = std::clamp(s.y + kBoxH / 2 - h / 2, 4.f, kScreenH - 4.f - h);
        list->SetProperty("left", fmt(s.left ? s.x + kBoxW + 10 : s.x - kBoxW - 10) + "px");
        list->SetProperty("top", fmt(top) + "px");
        list->SetProperty("transform-origin", s.left ? "0% 50%" : "100% 50%");
        list->SetProperty("display", "block");
        pop_list();
    }

    // The list opening: grown out of its box, faded in, over kListPop frames.
    void pop_list() {
        auto* list = document->GetElementById("list");
        if (list == nullptr || listFor < 0)
            return;
        const float t = std::min(1.f, static_cast<float>(listAge) / kListPop);
        const float ease = 1.f - (1.f - t) * (1.f - t);
        list->SetProperty("opacity", fmt(ease));
        list->SetProperty("transform", "scale(" + fmt(0.9f + 0.1f * ease) + ")");
    }

    void open_list(int slot) {
        if (slot < 0 || slot >= static_cast<int>(slots.size()) || slots[slot].input == kFixed)
            return;
        listFor = slot;
        listAge = 0;
        items.clear();
        items.push_back(PC_FIGHT_NONE);
        if (pc_fight_map_is_stick(slots[slot].input)) {
            items.push_back(PC_FIGHT_MOVE);
            items.push_back(PC_FIGHT_SMASH);
        } else {
            for (PcFightAction a : {PC_FIGHT_SHIELD, PC_FIGHT_GRAB, PC_FIGHT_JUMP, PC_FIGHT_ATTACK, PC_FIGHT_SPECIAL,
                                    PC_FIGHT_TAUNT})
                items.push_back(a);
        }
        const PcFightAction current = pc_fight_map_get(device, slots[slot].input);
        itemCursor = 0;
        for (size_t j = 0; j < items.size(); j++)
            if (items[j] == current)
                itemCursor = static_cast<int>(j);
        lbAudioAx_80024030(SFX_CONFIRM);
        refresh();
    }

    void pick(int item) {
        if (listFor < 0 || item < 0 || item >= static_cast<int>(items.size()))
            return;
        pc_fight_map_set(device, slots[listFor].input, items[item]);
        pc_log_line("fight controls: %s %s -> %s", device == PAD_FIGHT_DEVICE_TOUCH ? "touch" : "gamepad",
                    slots[listFor].file.c_str(), action_label(items[item]));
        cursor = listFor;
        listFor = -1;
        lbAudioAx_80024030(SFX_CONFIRM);
        refresh();
    }

    void reset() {
        pc_fight_map_reset(device);
        pc_log_line("fight controls: %s back to the defaults", device == PAD_FIGHT_DEVICE_TOUCH ? "touch" : "gamepad");
        lbAudioAx_80024030(SFX_CONFIRM);
        refresh();
    }

    // Up/down within a column, down from the bottom to Reset; left/right
    // across to the nearest box. dir: 0 up, 1 down, 2 left, 3 right.
    void move(int dir) {
        if (listFor >= 0) {
            if (dir > 1)
                return;
            const int n = static_cast<int>(items.size());
            itemCursor = (itemCursor + (dir == 0 ? n - 1 : 1)) % n;
            lbAudioAx_80024030(SFX_MOVE);
            refresh_list();
            return;
        }
        const int before = cursor;
        if (cursor == kReset) {
            if (dir == 0) {
                // Back up the column it came from, to its lowest box.
                for (auto it = column[lastColumn].rbegin(); it != column[lastColumn].rend(); ++it) {
                    if (slots[*it].input != kFixed) {
                        cursor = *it;
                        break;
                    }
                }
            }
        } else {
            const int c = slots[cursor].left ? 0 : 1;
            auto& col = column[c];
            int k = static_cast<int>(std::find(col.begin(), col.end(), cursor) - col.begin());
            if (dir <= 1) {
                do {
                    k += dir == 0 ? -1 : 1;
                } while (k >= 0 && k < static_cast<int>(col.size()) && slots[col[k]].input == kFixed);
                if (k >= static_cast<int>(col.size())) {
                    lastColumn = c;
                    cursor = kReset;
                } else if (k >= 0) {
                    cursor = col[k];
                }
            } else if ((dir == 2) == (c == 1) && !column[1 - c].empty()) {
                // The nearest mappable box in the other column.
                float best = 1e9f;
                for (int i : column[1 - c]) {
                    const float d = std::fabs(slots[i].y - slots[cursor].y);
                    if (slots[i].input != kFixed && d < best)
                        best = d, cursor = i;
                }
            }
        }
        if (cursor != before) {
            lbAudioAx_80024030(SFX_MOVE);
            refresh();
        }
    }

    void select() {
        if (listFor >= 0)
            pick(itemCursor);
        else if (cursor == kReset)
            reset();
        else
            open_list(cursor);
    }

    // False when there was nothing to back out of (the page goes back).
    bool back() {
        if (listFor < 0)
            return false;
        cursor = listFor;
        listFor = -1;
        lbAudioAx_80024030(SFX_BACK);
        refresh();
        return true;
    }

    void show() {
        if (shown || !load())
            return;
        device = active_device(xr_presenting() ? PAD_FIGHT_DEVICE_TOUCH : PAD_FIGHT_DEVICE_GAMEPAD);
        build();
        shown = true;
        presence = 0.f;
        placedPresence = -1.f;
        document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        layout();
    }

    void hide() {
        if (!shown)
            return;
        shown = false;
        listFor = -1;
        document->Hide();
    }

    // What the hint box says for the cursor.
    const char* hint() {
        if (!shown)
            return nullptr;
        if (listFor >= 0)
            return action_hint(items[itemCursor]);
        if (cursor == kReset)
            return "Put every input back to its default.";
        const Slot& s = slots[cursor];
        if (s.input == kFixed)
            return "Start always pauses.";
        hintText = s.label + ": " + action_label(pc_fight_map_get(device, s.input)) + ". Press A to change.";
        return hintText.c_str();
    }

    // The 1280x720-ish screen scaled into the frame's inside (measured on
    // the Erase Data frames: 12.3-87.8% across, 17.0-84.2% down), with the
    // page's fade: a fade and a short rise in, the reverse out.
    void layout() {
        auto* context = aurora::rmlui::get_context();
        auto* screen = document->GetElementById("screen");
        if (context == nullptr || screen == nullptr)
            return;
        const Rml::Vector2i d = context->GetDimensions();
        dims = d;
        if (d == placedDims && presence == placedPresence)
            return;
        placedDims = d;
        placedPresence = presence;
        const float x0 = 0.123f * d.x, y0 = 0.170f * d.y;
        const float w = 0.755f * d.x, h = 0.672f * d.y;
        const float p = std::clamp(presence, 0.f, 1.f);
        const float ease = 1.f - (1.f - p) * (1.f - p);
        scale = std::min(w / kScreenW, h / kScreenH);
        screen->SetProperty("transform", "scale(" + fmt(scale) + ")");
        screen->SetProperty("left", fmt(x0 + (w - kScreenW * scale) / 2) + "px");
        screen->SetProperty("top", fmt(y0 + (h - kScreenH * scale) / 2 + (1.f - ease) * 10.f * scale) + "px");
        screen->SetProperty("opacity", fmt(ease));
    }

    // A slot or an item from an element under the pointer.
    static bool id_of(Rml::Element* e, const char* prefix, int& index) {
        const size_t n = std::strlen(prefix);
        for (; e != nullptr; e = e->GetParentNode()) {
            const Rml::String& id = e->GetId();
            if (id.compare(0, n, prefix) == 0) {
                index = std::atoi(id.c_str() + n);
                return true;
            }
        }
        return false;
    }

    void ProcessEvent(Rml::Event& event) override {
        if (!shown || presence < 1.f)
            return;
        Rml::Element* target = event.GetTargetElement();
        int index = 0;
        switch (event.GetId()) {
        case Rml::EventId::Click:
            // A laser's click arrives as the menus' A too (xr_pointer.c).
            if (pointerWas)
                break;
            if (id_of(target, "item-", index)) {
                pick(index);
            } else if (id_of(target, "slot-", index)) {
                if (listFor >= 0 && listFor != index)
                    listFor = -1;
                open_list(index);
            } else if (id_of(target, "reset", index)) {
                listFor = -1;
                cursor = kReset;
                reset();
            } else if (listFor >= 0 && !id_of(target, "list", index)) {
                back(); // a click beside the list closes it
            }
            break;
        case Rml::EventId::Mouseover:
            if (listFor >= 0 && id_of(target, "item-", index) && index != itemCursor) {
                itemCursor = index;
                refresh_list();
            } else if (listFor < 0 && id_of(target, "slot-", index) && index != cursor &&
                       slots[index].input != kFixed)
            {
                cursor = index;
                lbAudioAx_80024030(SFX_MOVE);
                refresh();
            } else if (listFor < 0 && id_of(target, "reset", index) && cursor != kReset) {
                cursor = kReset;
                lbAudioAx_80024030(SFX_MOVE);
                refresh();
            }
            break;
        default:
            break;
        }
    }

    void update() {
        if (!shown)
            return;
        layout();
        // Another device picked up: show it instead.
        const PADFightDevice now = active_device(device);
        if (now != device) {
            device = now;
            build();
        }
        if (listFor >= 0 && listAge < kListPop) {
            listAge++;
            pop_list();
        }
#ifdef AURORA_ENABLE_OPENXR
        // The lasers: the mouse on the screen.
        float px = 0.f, py = 0.f;
        bool down = false;
        auto* context = aurora::rmlui::get_context();
        if (context != nullptr && aurora_xr_screen_pointer(&px, &py, &down)) {
            context->ProcessMouseMove(static_cast<int>(px * dims.x), static_cast<int>(py * dims.y), 0);
            if (down && !pointerPressed)
                context->ProcessMouseButtonDown(0, 0);
            else if (!down && pointerPressed)
                context->ProcessMouseButtonUp(0, 0);
            pointerPressed = down;
            pointerWas = true;
        } else if (pointerWas && context != nullptr) {
            if (pointerPressed)
                context->ProcessMouseButtonUp(0, 0);
            context->ProcessMouseLeave();
            pointerWas = pointerPressed = false;
        }
#endif
    }
};

Menu s_menu;

} // namespace

extern "C" void pc_fight_menu_show(bool show) {
    if (show)
        s_menu.show();
    else
        s_menu.hide();
}

extern "C" void pc_fight_menu_presence(float presence) { s_menu.presence = presence; }

extern "C" bool pc_fight_menu_input(int what) {
    if (!s_menu.shown || s_menu.presence < 1.f)
        return false;
    switch (what) {
    case PC_FIGHT_MENU_UP:
    case PC_FIGHT_MENU_DOWN:
    case PC_FIGHT_MENU_LEFT:
    case PC_FIGHT_MENU_RIGHT:
        s_menu.move(what);
        return true;
    case PC_FIGHT_MENU_CONFIRM:
        s_menu.select();
        return true;
    case PC_FIGHT_MENU_BACK:
        return s_menu.back();
    default:
        return false;
    }
}

extern "C" const char* pc_fight_menu_hint(void) { return s_menu.hint(); }

extern "C" void pc_fight_menu_update(void) { s_menu.update(); }
