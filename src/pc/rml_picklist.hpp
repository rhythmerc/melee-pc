/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A pop-up list of choices on an RmlUi document, shared by the launcher's
 * dropdown rows and Fight Controls' action lists: the choices are shown
 * before one is taken, the current one marked, a tag beside any (e.g.
 * "default"). The owner places it and hands it its input; up/down move
 * (wrapping), confirm takes the highlighted choice, back closes it. It grows
 * out of its anchor and fades in over a few frames (update()). Its
 * look is the owner's stylesheet: the element has class "picklist" plus the
 * owner's, a heading ".head", choices ".item" (".cursor", ".current",
 * ".first" for the first) and tags ".tag". */
#pragma once

#include <RmlUi/Core.h>

#include <string>
#include <vector>

namespace pc::rml {

struct PickChoice {
    std::string label;
    std::string tag; // shown beside it, or empty
};

class PickList {
public:
    // Builds the list as the last child of `parent`, at (x, y) in `parent`'s
    // px, `width` wide, the cursor on `current`. It grows out of `origin` (a
    // transform-origin: "0% 50%" from its left edge, "50% 0%" down from its
    // top).
    // `slant` is a transform the whole list keeps (e.g. "skewX(-10deg)", to
    // match slanted buttons), under its opening growth.
    void open(Rml::Element* parent, const std::string& heading, std::vector<PickChoice> choices, int current, float x,
              float y, float width, const char* origin, const char* extra_class = nullptr, const char* slant = nullptr);
    void close();
    bool is_open() const { return element_ != nullptr; }

    // The choices' height at `item_height` each plus `head_height`, for
    // placing it before open().
    static float height(size_t choices, float item_height, float head_height) {
        return head_height + item_height * static_cast<float>(choices);
    }

    int cursor() const { return cursor_; }
    // Up (-1) or down (+1), wrapping. True when the cursor moved.
    bool move(int direction);
    // The pointer over a choice (a Mouseover event's target): the cursor goes
    // there. True when it moved.
    bool hover(Rml::Element* target);
    // A Click event's target: the choice clicked, or -1 (also -1 outside).
    int clicked(Rml::Element* target) const;
    // Whether an element is the list or inside it.
    bool contains(Rml::Element* target) const;

    // Once a frame while open: the opening's fade and growth.
    void update();

private:
    void redraw();
    int choice_of(Rml::Element* target) const;

    Rml::Element* element_ = nullptr;
    std::string heading_;
    std::vector<PickChoice> choices_;
    int cursor_ = 0, current_ = 0;
    std::string slant_;
    int age_ = 0;
};

} // namespace pc::rml
