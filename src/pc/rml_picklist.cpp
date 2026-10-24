/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A pop-up list of choices on an RmlUi document (rml_picklist.hpp). */
#include "rml_picklist.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pc::rml {
namespace {

constexpr int kPopFrames = 6; // frames the list takes to open

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

} // namespace

void PickList::open(Rml::Element* parent, const std::string& heading, std::vector<PickChoice> choices, int current,
                    float x, float y, float width, const char* origin, const char* extra_class, const char* slant) {
    close();
    if (parent == nullptr || choices.empty())
        return;
    heading_ = heading;
    choices_ = std::move(choices);
    current_ = current;
    cursor_ = std::clamp(current, 0, static_cast<int>(choices_.size()) - 1);
    age_ = 0;
    slant_ = slant != nullptr ? slant : "";
    auto child = parent->GetOwnerDocument()->CreateElement("div");
    element_ = parent->AppendChild(std::move(child));
    element_->SetClass("picklist", true);
    if (extra_class != nullptr)
        element_->SetClass(extra_class, true);
    element_->SetProperty("position", "absolute");
    element_->SetProperty("left", fmt(x) + "px");
    element_->SetProperty("top", fmt(y) + "px");
    element_->SetProperty("width", fmt(width) + "px");
    element_->SetProperty("transform-origin", origin);
    redraw();
    update();
}

void PickList::close() {
    if (element_ == nullptr)
        return;
    if (Rml::Element* parent = element_->GetParentNode())
        parent->RemoveChild(element_);
    element_ = nullptr;
    choices_.clear();
}

void PickList::redraw() {
    if (element_ == nullptr)
        return;
    std::string rml;
    if (!heading_.empty())
        rml += "<div class=\"head\">" + Rml::StringUtilities::EncodeRml(heading_) + "</div>";
    for (size_t i = 0; i < choices_.size(); i++) {
        std::string cls = "item";
        if (i == 0)
            cls += " first";
        if (static_cast<int>(i) == cursor_)
            cls += " cursor";
        if (static_cast<int>(i) == current_)
            cls += " current";
        rml += "<div class=\"" + cls + "\" data-choice=\"" + std::to_string(i) + "\"><span>" +
               Rml::StringUtilities::EncodeRml(choices_[i].label) + "</span>";
        if (!choices_[i].tag.empty())
            rml += "<span class=\"tag\">" + Rml::StringUtilities::EncodeRml(choices_[i].tag) + "</span>";
        rml += "</div>";
    }
    element_->SetInnerRML(rml);
}

bool PickList::move(int direction) {
    if (element_ == nullptr || choices_.empty())
        return false;
    const int n = static_cast<int>(choices_.size());
    cursor_ = (cursor_ + (direction < 0 ? n - 1 : 1)) % n;
    redraw();
    return true;
}

int PickList::choice_of(Rml::Element* target) const {
    for (Rml::Element* e = target; e != nullptr && e != element_; e = e->GetParentNode()) {
        if (const Rml::Variant* v = e->GetAttribute("data-choice"))
            return std::atoi(v->Get<Rml::String>().c_str());
    }
    return -1;
}

bool PickList::contains(Rml::Element* target) const {
    for (Rml::Element* e = target; e != nullptr; e = e->GetParentNode())
        if (e == element_)
            return element_ != nullptr;
    return false;
}

bool PickList::hover(Rml::Element* target) {
    if (element_ == nullptr || !contains(target))
        return false;
    const int i = choice_of(target);
    if (i < 0 || i == cursor_)
        return false;
    cursor_ = i;
    redraw();
    return true;
}

int PickList::clicked(Rml::Element* target) const {
    if (element_ == nullptr || !contains(target))
        return -1;
    return choice_of(target);
}

void PickList::update() {
    if (element_ == nullptr)
        return;
    if (age_ > kPopFrames)
        return;
    const float t = std::min(1.f, static_cast<float>(age_) / kPopFrames);
    const float ease = 1.f - (1.f - t) * (1.f - t);
    element_->SetProperty("opacity", fmt(ease));
    element_->SetProperty("transform", (slant_.empty() ? "" : slant_ + " ") + "scale(" + fmt(0.9f + 0.1f * ease) + ")");
    age_++;
}

} // namespace pc::rml
