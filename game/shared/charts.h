// Chart primitives for the insights screens and the end-of-run summary:
// lines, curves, dots, rings, bars. Built on draw2d's canvas (virtual units,
// 1080 tall) but drawn straight in physical pixels with one colour set per
// primitive, so a full chart stays a few thousand draw calls.
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <string>
#include <vector>

#include "draw2d.h"

namespace skaar::ui::chart {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Call after ui::Init with the same backend.
void Init(const Backend& backend);

// Square dots (x/y pairs), centred, size in virtual units: one colour change for the lot.
void Dots(const std::vector<float>& xy, float size, Color color);
// Filled disc / circle outline, centred, in virtual units.
void Disc(float x, float y, float radius, Color color);
void Ring(float x, float y, float radius, Color color, float thickness = 1.0f);

// y = values[i] evenly spaced across r (first at the left edge, last at the right),
// mapped from [lo, hi] to bottom..top. One draw call per pixel column.
// fill: also shade the area under the curve with fillColor.
void Curve(const Rect& r, const std::vector<float>& values, float lo, float hi, Color color,
           float thickness = 1.5f, Color fillColor = {0, 0, 0, 0});

// x/y pairs in virtual units, joined in order (one colour change for the whole line).
// Draws one rectangle per pixel step, so keep it to a few hundred points.
void Polyline(const std::vector<float>& xy, Color color, float thickness = 1.0f);

// Horizontal progress bar: track + fill fraction 0..1.
void Progress(const Rect& r, float fraction, Color track, Color fill);

// "Nice" axis range and step for lo..hi with about `ticks` divisions.
void NiceRange(float lo, float hi, int ticks, float& outLo, float& outHi, float& outStep);

// Axis labels and faint gridlines for a chart area (labels drawn left of r in mono).
void YAxis(const Rect& r, float lo, float hi, float step, const char* format = "%.0f");

// Short number for labels: 950, 1.2k, 12k, 1.4M
std::string Compact(float value);

}  // namespace skaar::ui::chart
