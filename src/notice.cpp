// T4: blown tire notice (see notice.h). Sizes are in UI units: 1/900 of the
// screen height, as in the game's own layouts (ui/*.sii use a 1440x900 canvas
// [verified: base_share.scs, vega 125]).

#include "notice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#include "imgui.h"

namespace notice {
namespace {

// Measured on the user's screenshots (vega 126).
constexpr ImU32 kPanel = IM_COL32(0x3B, 0x3B, 0x3B, 0xD9);  // ~0.85 opaque
constexpr ImU32 kBarColour = IM_COL32(0xFF, 0x55, 0x55, 0xFF);
constexpr ImU32 kTitle = IM_COL32(0xFF, 0xC2, 0x04, 0xFF);
constexpr ImU32 kText = IM_COL32(0xF4, 0xF4, 0xF4, 0xFF);
constexpr ImU32 kIconRed = IM_COL32(0xFF, 0x57, 0x57, 0xFF);

// Layout [estimate: fitted to the screenshots, scale unknown].
constexpr float kMinWidth = 300.f, kMaxWidth = 600.f;
constexpr float kPadLeft = 14.f, kIconBox = 34.f, kIconGap = 12.f, kPadRight = 18.f;
constexpr float kPadY = 11.f, kFont = 16.5f, kLine = 19.f, kBarH = 4.f, kRounding = 5.f;
constexpr double kFadeIn = 0.25, kFadeOut = 0.4;

std::mutex g_mutex;
Style g_style;
std::string g_title, g_body;
double g_start = -1.0;

ImU32 Fade(ImU32 c, float a) {
    const unsigned alpha = static_cast<unsigned>(((c >> IM_COL32_A_SHIFT) & 0xFF) * a + 0.5f);
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

// The dashboard tire pressure warning, in red like the game's handbrake
// notice icon: a tire cross-section open at the top, tread under it, '!' inside.
void DrawIcon(ImDrawList* dl, ImVec2 c, float u, float a) {
    const ImU32 red = Fade(kIconRed, a);
    u *= 1.15f;
    const float t = 2.3f * u;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };
    dl->PathLineTo(P(-7.f, -11.f));
    dl->PathBezierCubicCurveTo(P(-14.f, -5.f), P(-14.f, 5.f), P(-9.f, 8.f));
    dl->PathLineTo(P(9.f, 8.f));
    dl->PathBezierCubicCurveTo(P(14.f, 5.f), P(14.f, -5.f), P(7.f, -11.f));
    dl->PathStroke(red, ImDrawFlags_None, t);
    for (float x : {-6.f, -2.f, 2.f, 6.f}) dl->AddLine(P(x, 8.f), P(x, 12.f), red, t);  // tread
    dl->AddLine(P(0.f, -6.5f), P(0.f, 0.5f), red, t);                                 // '!'
    dl->AddCircleFilled(P(0.f, 4.f), 1.5f * u, red);
}

}  // namespace

void Configure(const Style& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_style = s;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Show(const std::string& title, const std::string& body) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_title = title;
    g_body = body;
    g_start = Now();
}

bool Active(double now) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_start >= 0.0 && now - g_start < g_style.seconds;
}

bool Draw(ImDrawList* dl, const ImVec2& display, ImFont* font, double now) {
    std::string title, body;
    Style style;
    double t;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_start < 0.0) return false;
        t = now - g_start;
        if (t < 0.0 || t >= g_style.seconds) return false;
        title = g_title;
        body = g_body;
        style = g_style;
    }
    const float a = static_cast<float>(std::min({1.0, t / kFadeIn, (style.seconds - t) / kFadeOut}));
    const float u = display.y / 900.f * style.scale;
    const float size = kFont * u;

    std::vector<std::pair<std::string, ImU32>> lines;
    if (!title.empty()) lines.emplace_back(title, kTitle);
    for (size_t p = 0; p <= body.size();) {
        size_t e = body.find('\n', p);
        if (e == std::string::npos) e = body.size();
        if (e > p) lines.emplace_back(body.substr(p, e - p), kText);
        p = e + 1;
    }
    float text_w = 0.f;
    for (const auto& l : lines) text_w = std::max(text_w, font->CalcTextSizeA(size, FLT_MAX, 0.f, l.first.c_str()).x);

    const float content_h = std::max(kIconBox * u, lines.size() * kLine * u);
    const float w = std::clamp((kPadLeft + kIconBox + kIconGap + kPadRight) * u + text_w, kMinWidth * u, kMaxWidth * u);
    const float h = 2.f * kPadY * u + content_h;
    const float slide = (1.f - std::min(1.f, static_cast<float>(t / kFadeIn))) * -8.f * u;
    const ImVec2 p0(std::floor((display.x - w) * 0.5f), std::floor(style.top * u + slide));

    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), Fade(kPanel, a), kRounding * u, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + h), ImVec2(p0.x + w, p0.y + h + kBarH * u), Fade(kBarColour, a),
                      std::min(kRounding, kBarH) * u, ImDrawFlags_RoundCornersBottom);

    const float mid = p0.y + kPadY * u + content_h * 0.5f;
    DrawIcon(dl, ImVec2(p0.x + (kPadLeft + kIconBox * 0.5f) * u, mid), u, a);

    const float tx = p0.x + (kPadLeft + kIconBox + kIconGap) * u;
    float ty = mid - lines.size() * kLine * u * 0.5f + (kLine - kFont) * u * 0.5f;
    for (const auto& l : lines) {
        dl->AddText(font, size, ImVec2(std::floor(tx), std::floor(ty)), Fade(l.second, a), l.first.c_str());
        ty += kLine * u;
    }
    return true;
}

}  // namespace notice
