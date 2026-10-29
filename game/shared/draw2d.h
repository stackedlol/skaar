// 2D drawing kit shared by cgame and ui: fills, hairlines, images and
// proportional text from the font atlases tools/gen_assets.py generates.
//
// Coordinates are virtual units on a canvas 1080 units tall; the width follows
// the window's aspect (Width()). Everything is snapped to physical pixels.
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "q3.h"
#include "theme.h"

namespace skaar::ui {

// Engine services the kit needs; each module fills these from its syscalls.
struct Backend {
    qhandle_t (*registerShader)(const char* name);  // mipmapped (fonts, images)
    void (*setColor)(const float* rgba);
    void (*drawStretchPic)(float x, float y, float w, float h, float s1, float t1, float s2, float t2,
                           qhandle_t shader);
    bool (*readFile)(const char* path, std::string& out);
};

enum class Align { kLeft, kCenter, kRight };

class Font {
public:
    bool Load(const Backend& backend, const char* name);
    bool Loaded() const { return shader_ != 0; }

    // y is the baseline. Returns the advance (width) drawn, in virtual units.
    float Draw(float x, float y, float size, std::string_view text, Color color, float tracking = 0.0f,
               Align align = Align::kLeft) const;
    float Measure(float size, std::string_view text, float tracking = 0.0f) const;
    float Ascent(float size) const { return ascent_ * size / pixelSize_; }
    float CapHeight(float size) const { return capHeight_ * size / pixelSize_; }

private:
    struct Glyph {
        float x, y, w, h;         // atlas rect, pixels
        float xoff, yoff, advance; // relative to pen on the baseline, pixels
    };

    const Glyph* Find(uint32_t codepoint) const;

    qhandle_t shader_ = 0;
    float pixelSize_ = 1, atlasW_ = 1, atlasH_ = 1, ascent_ = 0, capHeight_ = 0;
    std::unordered_map<uint32_t, Glyph> glyphs_;
};

struct Fonts {
    Font display;  // Montserrat 200: statements, titles
    Font text;     // Montserrat 300: items, body
    Font mono;     // JetBrains Mono 400: metadata, numbers, HUD
};

// Call once per module init (and again after vid_restart).
bool Init(const Backend& backend, int vidWidth, int vidHeight);
// The window's drawable size changed (resize, fullscreen toggle; r_vidSize).
void Resize(int vidWidth, int vidHeight);
const Fonts& GetFonts();

float Width();   // canvas width in virtual units
float Height();  // always 1080
float Scale();   // physical pixels per virtual unit

void Fill(float x, float y, float w, float h, Color color);
void HLine(float x, float y, float w, Color color);  // 1 physical pixel tall
void VLine(float x, float y, float h, Color color);
void Image(float x, float y, float w, float h, qhandle_t shader, Color color);
void Background(bool grid = true);  // stage fill + faint arena grid

// Uppercase copy (ASCII), for labels.
std::string Upper(std::string_view text);

}  // namespace skaar::ui
