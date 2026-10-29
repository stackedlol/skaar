// This file is part of Skaar. GPLv2 or later.
#include "charts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace skaar::ui::chart {
namespace {

Backend g_backend{};
qhandle_t g_white = 0;

void SetColor(Color c) {
    const float rgba[4] = {c.r, c.g, c.b, c.a};
    g_backend.setColor(rgba);
}

void ResetColor() { g_backend.setColor(nullptr); }

// physical-pixel rectangle, colour already set
void Px(float x, float y, float w, float h) {
    if (w > 0 && h > 0) {
        g_backend.drawStretchPic(x, y, w, h, 0, 0, 1, 1, g_white);
    }
}

float ToPx(float v) { return v * Scale(); }

}  // namespace

void Init(const Backend& backend) {
    g_backend = backend;
    g_white = backend.registerShader("white");
}

namespace {

// Line with the colour already set. stride > 1 draws every stride-th pixel
// step with a slightly bigger square: a dotted line that costs 1/stride.
void LineCore(float x0, float y0, float x1, float y1, float thickness, int stride = 1) {
    const float t = std::max(1.0f, std::round(ToPx(thickness)));
    const float ax = ToPx(x0), ay = ToPx(y0), bx = ToPx(x1), by = ToPx(y1);
    const float dx = bx - ax, dy = by - ay;
    // axis-aligned lines are one rectangle
    if (std::fabs(dy) < 0.5f) {
        Px(std::round(std::min(ax, bx)), std::round(ay - t / 2), std::round(std::fabs(dx)) + 1, t);
    } else if (std::fabs(dx) < 0.5f) {
        Px(std::round(ax - t / 2), std::round(std::min(ay, by)), t, std::round(std::fabs(dy)) + 1);
    } else if (std::fabs(dx) >= std::fabs(dy)) {
        // x-major: one vertical span per column
        const int steps = static_cast<int>(std::ceil(std::fabs(dx)));
        const float sx = dx > 0 ? 1 : -1, slope = dy / std::fabs(dx);
        const float w = stride > 1 ? std::max(t, stride * 0.6f) : 1;
        for (int i = 0; i <= steps; i += stride) {
            Px(std::round(ax + sx * i), std::round(ay + slope * i - t / 2), w, t);
        }
    } else {
        const int steps = static_cast<int>(std::ceil(std::fabs(dy)));
        const float sy = dy > 0 ? 1 : -1, slope = dx / std::fabs(dy);
        const float h = stride > 1 ? std::max(t, stride * 0.6f) : 1;
        for (int i = 0; i <= steps; i += stride) {
            Px(std::round(ax + slope * i - t / 2), std::round(ay + sy * i), t, h);
        }
    }
}

}  // namespace

void Dots(const std::vector<float>& xy, float size, Color color) {
    if (!g_white || xy.size() < 2) {
        return;
    }
    const float s = std::max(1.0f, std::round(ToPx(size)));
    SetColor(color);
    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        Px(std::round(ToPx(xy[i]) - s / 2), std::round(ToPx(xy[i + 1]) - s / 2), s, s);
    }
    ResetColor();
}

void Disc(float x, float y, float radius, Color color) {
    if (!g_white) {
        return;
    }
    const float cx = ToPx(x), cy = ToPx(y), r = std::max(0.5f, ToPx(radius));
    SetColor(color);
    // one span per pixel row
    for (float py = std::floor(cy - r); py <= std::ceil(cy + r); py++) {
        const float dy = py + 0.5f - cy;
        if (std::fabs(dy) > r) {
            continue;
        }
        const float half = std::sqrt(r * r - dy * dy);
        const float x0 = std::round(cx - half), x1 = std::round(cx + half);
        Px(x0, py, std::max(1.0f, x1 - x0), 1);
    }
    ResetColor();
}

void Ring(float x, float y, float radius, Color color, float thickness) {
    const int segments = std::clamp(static_cast<int>(ToPx(radius) * 0.8f), 16, 96);
    std::vector<float> xy;
    for (int i = 0; i <= segments; i++) {
        const float a = 6.2831853f * i / segments;
        xy.push_back(x + radius * std::cos(a));
        xy.push_back(y + radius * std::sin(a));
    }
    Polyline(xy, color, thickness);
}

void Polyline(const std::vector<float>& xy, Color color, float thickness) {
    if (!g_white || xy.size() < 4) {
        return;
    }
    // The 2D queue holds ~10k rectangles a frame for everything on screen, so a
    // long path gets at most ~2500 of them: beyond that it turns into dots.
    float length = 0;
    for (size_t i = 2; i + 1 < xy.size(); i += 2) {
        length += std::max(std::fabs(xy[i] - xy[i - 2]), std::fabs(xy[i + 1] - xy[i - 1]));
    }
    const int stride = std::max(1, static_cast<int>(std::ceil(ToPx(length) / 2500.0f)));
    SetColor(color);
    for (size_t i = 2; i + 1 < xy.size(); i += 2) {
        LineCore(xy[i - 2], xy[i - 1], xy[i], xy[i + 1], thickness, stride);
    }
    ResetColor();
}

void Curve(const Rect& r, const std::vector<float>& values, float lo, float hi, Color color, float thickness,
           Color fillColor) {
    if (!g_white || values.empty() || hi <= lo) {
        return;
    }
    const float left = std::round(ToPx(r.x)), right = std::round(ToPx(r.Right()));
    const float top = ToPx(r.y), bottom = ToPx(r.Bottom());
    const float t = std::max(1.0f, std::round(ToPx(thickness)));
    const int n = static_cast<int>(values.size());

    auto yAt = [&](float px) {
        if (n == 1) {
            return values[0];
        }
        const float f = (px - left) / std::max(1.0f, right - left) * (n - 1);
        const int i = std::clamp(static_cast<int>(f), 0, n - 2);
        const float k = std::clamp(f - i, 0.0f, 1.0f);
        return values[i] + (values[i + 1] - values[i]) * k;
    };
    auto toY = [&](float v) { return bottom - (std::clamp(v, lo, hi) - lo) / (hi - lo) * (bottom - top); };

    if (fillColor.a > 0) {
        // a faint wash: 2 px columns on dense displays halve its cost
        const float col = Scale() > 1.4f ? 2 : 1;
        SetColor(fillColor);
        for (float px = left; px <= right; px += col) {
            const float y = std::round(toY(yAt(px)));
            Px(px, y, col, std::round(bottom) - y);
        }
    }

    SetColor(color);
    float prev = toY(yAt(left));
    for (float px = left; px <= right; px++) {
        const float y = toY(yAt(px));
        // span from the previous column's y to this one's, so steep parts stay joined
        const float y0 = std::round(std::min(prev, y) - t / 2), y1 = std::round(std::max(prev, y) + t / 2);
        Px(px, y0, 1, std::max(t, y1 - y0));
        prev = y;
    }
    ResetColor();
}

void Progress(const Rect& r, float fraction, Color track, Color fill) {
    Fill(r.x, r.y, r.w, r.h, track);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0) {
        Fill(r.x, r.y, r.w * f, r.h, fill);
    }
}

void NiceRange(float lo, float hi, int ticks, float& outLo, float& outHi, float& outStep) {
    if (hi <= lo) {
        hi = lo + 1;
    }
    const float raw = (hi - lo) / std::max(1, ticks);
    const float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    const float norm = raw / mag;
    const float nice = norm < 1.5f ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
    outStep = nice * mag;
    outLo = std::floor(lo / outStep) * outStep;
    outHi = std::ceil(hi / outStep) * outStep;
    if (outHi <= outLo) {
        outHi = outLo + outStep;
    }
}

void YAxis(const Rect& r, float lo, float hi, float step, const char* format) {
    const Font& mono = GetFonts().mono;
    if (step <= 0 || hi <= lo) {
        return;
    }
    for (float v = lo; v <= hi + step * 0.01f; v += step) {
        const float y = r.Bottom() - (v - lo) / (hi - lo) * r.h;
        HLine(r.x, y, r.w, theme::kHairline);
        char label[32];
        if (std::string(format) == "compact") {
            std::snprintf(label, sizeof(label), "%s", Compact(v).c_str());
        } else {
            std::snprintf(label, sizeof(label), format, v);
        }
        mono.Draw(r.x - 14, y + 5, 14, label, theme::kCreamFaint, 0.04f, Align::kRight);
    }
}

std::string Compact(float value) {
    char buf[32];
    const float a = std::fabs(value);
    if (a >= 1e6f) {
        std::snprintf(buf, sizeof(buf), "%.1fM", value / 1e6f);
    } else if (a >= 1e4f) {
        std::snprintf(buf, sizeof(buf), "%.0fk", value / 1e3f);
    } else if (a >= 1e3f) {
        std::snprintf(buf, sizeof(buf), "%.1fk", value / 1e3f);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", value);
    }
    return buf;
}

}  // namespace skaar::ui::chart
