// T4: the blown tire notice, drawn at the top centre of the screen in the
// style of the game's own notices (measured on the user's screenshots, vega
// 126): a translucent dark panel with rounded corners, a red bar along its
// bottom, an icon on the left, a yellow title and white text.
//
// Pure ImGui drawing: used by the overlay in the game and by the offline
// preview tool (tools/noticepreview).
#pragma once

#include <string>

struct ImDrawList;
struct ImFont;
struct ImVec2;

namespace notice {

struct Style {
    double seconds = 8.0;  // time on screen, fades included
    float top = 60.f;      // gap from the top of the screen, in UI units (1/900 of its height)
    float scale = 1.f;     // size on top of the screen-height scaling
};

void Configure(const Style& s);

// Any thread: show a notice (UTF-8; '\n' splits the body into lines). It
// replaces the one on screen, if any.
void Show(const std::string& title, const std::string& body);

// Seconds on a steady clock; the time Draw expects.
double Now();

// Render thread, inside an ImGui frame: draws the current notice (if any) on
// dl. Returns false when there is nothing to draw at `now`.
bool Draw(ImDrawList* dl, const ImVec2& display, ImFont* font, double now);

// Render thread: true while a notice is on screen (or due) at `now`.
bool Active(double now);

}  // namespace notice
