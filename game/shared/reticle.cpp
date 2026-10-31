// This file is part of Skaar. GPLv2 or later.
#include "reticle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "draw2d.h"

namespace skaar::ui {
namespace {

// Outline: graphite at fixed opacity, independent of the theme's ground, so the
// pastel reticle reads on the light range and on dark walls
constexpr Color kOutline = Rgb(35, 35, 35, 0.6f);

float Number(std::string (*cvarString)(const char*), const char* name, float fallback) {
    const std::string v = cvarString(name);
    return v.empty() ? fallback : static_cast<float>(std::atof(v.c_str()));
}

Color ParseHex(const std::string& hex, Color fallback) {
    unsigned int v = 0;
    if (hex.size() < 6 || std::sscanf(hex.c_str(), "%6x", &v) != 1) {
        return fallback;
    }
    return Rgb((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

// A rectangle given in physical pixels
void FillPx(float x, float y, float w, float h, Color c) {
    const float s = Scale();
    Fill(x / s, y / s, w / s, h / s, c);
}

}  // namespace

Reticle ReticleFromCvars(std::string (*cvarString)(const char* name)) {
    Reticle r;
    // a typed or archived cvar can hold anything: clamp before the cast (UB out of
    // range) and bound the sizes, which are handed to the renderer as rectangles
    const float styleValue = Number(cvarString, "cg_crosshairStyle", 0);
    const int style = std::isfinite(styleValue) ? static_cast<int>(std::clamp(styleValue, 0.0f, 2.0f)) : 0;
    r.style = style == 1 ? ReticleStyle::kDot : style == 2 ? ReticleStyle::kCrossDot : ReticleStyle::kCross;
    r.length = std::clamp(Number(cvarString, "cg_crosshairSize", r.length), 0.0f, 64.0f);
    r.gap = std::clamp(Number(cvarString, "cg_crosshairGap", r.gap), 0.0f, 64.0f);
    r.thickness = std::clamp(Number(cvarString, "cg_crosshairThickness", r.thickness), 0.5f, 32.0f);
    r.color = ParseHex(cvarString("cg_crosshairColor"), r.color);
    r.outline = Number(cvarString, "cg_crosshairOutline", 1) != 0;
    return r;
}

void ScreenCentre(float* cx, float* cy) {
    const float s = Scale();
    *cx = std::floor(Width() * s / 2) / s;
    *cy = std::floor(Height() * s / 2) / s;
}

void DrawReticle(float cx, float cy, const Reticle& r) {
    // work in physical pixels: whole-pixel thickness, centred on a pixel
    const float s = Scale();
    const float t = std::max(1.0f, std::round(r.thickness * s));
    const float len = std::round(r.length * s);
    const float gap = std::round(r.gap * s);
    const float x0 = std::floor(cx * s) - std::floor((t - 1) / 2);  // left/top edge of the centre bar
    const float y0 = std::floor(cy * s) - std::floor((t - 1) / 2);
    const float o = r.outline ? 1.0f : 0.0f;

    // arms: with gap 0 a horizontal and a vertical bar through the centre (a
    // clean "+"); with a gap, four separate arms
    auto cross = [&](Color c, float grow) {
        if (len <= 0) {
            return;
        }
        if (gap <= 0) {
            FillPx(x0 - len - grow, y0 - grow, 2 * len + t + 2 * grow, t + 2 * grow, c);
            FillPx(x0 - grow, y0 - len - grow, t + 2 * grow, 2 * len + t + 2 * grow, c);
        } else {
            FillPx(x0 - gap - len - grow, y0 - grow, len + 2 * grow, t + 2 * grow, c);
            FillPx(x0 + t + gap - grow, y0 - grow, len + 2 * grow, t + 2 * grow, c);
            FillPx(x0 - grow, y0 - gap - len - grow, t + 2 * grow, len + 2 * grow, c);
            FillPx(x0 - grow, y0 + t + gap - grow, t + 2 * grow, len + 2 * grow, c);
        }
    };
    auto dot = [&](Color c, float grow) { FillPx(x0 - grow, y0 - grow, t + 2 * grow, t + 2 * grow, c); };

    const bool drawCross = r.style != ReticleStyle::kDot;
    const bool drawDot = r.style != ReticleStyle::kCross || (gap <= 0 && len <= 0);

    // outline first, underneath
    if (o > 0) {
        if (drawCross) {
            cross(kOutline, o);
        }
        if (drawDot) {
            dot(kOutline, o);
        }
    }
    if (drawCross) {
        cross(r.color, 0);
    }
    if (drawDot) {
        dot(r.color, 0);
    }
}

}  // namespace skaar::ui
