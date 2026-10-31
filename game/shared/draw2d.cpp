// This file is part of Skaar. GPLv2 or later.
#include "draw2d.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace skaar::ui {
namespace {

Backend g_backend{};
Fonts g_fonts;
qhandle_t g_white = 0;
float g_scale = 1.0f;
float g_width = 1920.0f;

constexpr float kHeight = 1080.0f;
constexpr int kMaxGridLines = 512;  // a grid never needs more, whatever the aspect

float Snap(float v) { return std::round(v * g_scale); }

void SetColor(Color c) {
    const float rgba[4] = {c.r, c.g, c.b, c.a};
    g_backend.setColor(rgba);
}

// Minimal UTF-8 decoder: returns the next codepoint and advances i
uint32_t NextCodepoint(std::string_view s, size_t& i) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) {
        return c;
    }
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size()) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

}  // namespace

bool Font::Load(const Backend& backend, const char* name) {
    std::string info;
    const std::string infoPath = std::string("fonts/") + name + ".fontinfo";
    if (!backend.readFile(infoPath.c_str(), info)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: font %s missing\n", infoPath.c_str());
        return false;
    }

    std::istringstream in(info);
    std::string key, image;
    while (in >> key) {
        if (key == "image") {
            in >> image;
        } else if (key == "size") {
            in >> pixelSize_;
        } else if (key == "atlas") {
            in >> atlasW_ >> atlasH_;
        } else if (key == "metrics") {
            float descent, line;
            in >> ascent_ >> descent >> line;
        } else if (key == "glyph") {
            uint32_t cp = 0;
            Glyph g;
            if (!(in >> cp >> g.x >> g.y >> g.w >> g.h >> g.xoff >> g.yoff >> g.advance)) {
                // a short record (an interrupted write) leaves the remaining fields at
                // their defaults; keep them out of the atlas rather than use half a glyph
                Com_Printf(S_COLOR_YELLOW "WARNING: font %s: truncated glyph record\n", infoPath.c_str());
                in.clear();
                std::getline(in, key);  // drop the rest of the line
                continue;
            }
            glyphs_[cp] = g;
        } else {
            std::string rest;
            std::getline(in, rest);  // header line or unknown key
        }
    }

    if (const Glyph* h = Find('H')) {
        capHeight_ = -(h->yoff) - 6.0f;  // atlas pad is 6 px
    }
    shader_ = backend.registerShader(image.c_str());
    DEV_ASSERT(shader_ != 0, "font atlas shader failed to register");
    return shader_ != 0;
}

const Font::Glyph* Font::Find(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) {
        return &it->second;
    }
    it = glyphs_.find('?');
    return it != glyphs_.end() ? &it->second : nullptr;
}

float Font::Measure(float size, std::string_view text, float tracking) const {
    const float k = size / pixelSize_;
    float w = 0;
    int count = 0;
    for (size_t i = 0; i < text.size();) {
        if (const Glyph* g = Find(NextCodepoint(text, i))) {
            w += g->advance * k;
            count++;
        }
    }
    return w + tracking * size * std::max(0, count - 1);
}

float Font::Draw(float x, float y, float size, std::string_view text, Color color, float tracking,
                 Align align) const {
    if (!shader_) {
        return 0;
    }
    const float width = Measure(size, text, tracking);
    if (align == Align::kCenter) {
        x -= width / 2;
    } else if (align == Align::kRight) {
        x -= width;
    }

    const float k = size / pixelSize_;
    SetColor(color);
    float pen = x;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = NextCodepoint(text, i);
        const Glyph* g = Find(cp);
        if (!g) {
            continue;
        }
        if (cp != ' ') {
            // snap the glyph origin to whole pixels so text stays crisp
            const float px = std::round((pen + g->xoff * k) * g_scale);
            const float py = std::round((y + g->yoff * k) * g_scale);
            g_backend.drawStretchPic(px, py, g->w * k * g_scale, g->h * k * g_scale, g->x / atlasW_, g->y / atlasH_,
                                     (g->x + g->w) / atlasW_, (g->y + g->h) / atlasH_, shader_);
        }
        pen += g->advance * k + tracking * size;
    }
    g_backend.setColor(nullptr);
    return width;
}

bool Init(const Backend& backend, int vidWidth, int vidHeight) {
    // a zero viewport would make Scale() 0 and Width() infinite, which Background()'s
    // grid loop cannot survive. Both callers only warn on a false return, so clamp the
    // canvas rather than leave the backend uninitialised for everything that draws.
    if (vidWidth <= 0 || vidHeight <= 0) {
        Com_Printf(S_COLOR_YELLOW "WARNING: draw2d: bad viewport %dx%d, using 1x1\n", vidWidth, vidHeight);
    }
    g_backend = backend;
    g_scale = std::max(vidHeight, 1) / kHeight;
    g_width = std::max(vidWidth, 1) / g_scale;
    g_white = backend.registerShader("white");

    bool ok = g_fonts.display.Load(backend, "manrope-200");
    ok = g_fonts.text.Load(backend, "manrope-300") && ok;
    ok = g_fonts.mono.Load(backend, "jbmono-400") && ok;
    return ok && g_white;
}

const Fonts& GetFonts() { return g_fonts; }
float Width() { return g_width; }
float Height() { return kHeight; }
float Scale() { return g_scale; }

void Fill(float x, float y, float w, float h, Color color) {
    SetColor(color);
    const float x0 = Snap(x), y0 = Snap(y);
    g_backend.drawStretchPic(x0, y0, Snap(x + w) - x0, Snap(y + h) - y0, 0, 0, 1, 1, g_white);
    g_backend.setColor(nullptr);
}

void HLine(float x, float y, float w, Color color) {
    SetColor(color);
    g_backend.drawStretchPic(Snap(x), Snap(y), Snap(x + w) - Snap(x), 1, 0, 0, 1, 1, g_white);
    g_backend.setColor(nullptr);
}

void VLine(float x, float y, float h, Color color) {
    SetColor(color);
    g_backend.drawStretchPic(Snap(x), Snap(y), 1, Snap(y + h) - Snap(y), 0, 0, 1, 1, g_white);
    g_backend.setColor(nullptr);
}

void Image(float x, float y, float w, float h, qhandle_t shader, Color color) {
    SetColor(color);
    g_backend.drawStretchPic(Snap(x), Snap(y), Snap(x + w) - Snap(x), Snap(y + h) - Snap(y), 0, 0, 1, 1, shader);
    g_backend.setColor(nullptr);
}

void Background(bool grid) {
    Fill(0, 0, g_width, kHeight, theme::kStage);
    if (!grid) {
        return;
    }
    // Faint arena grid, every 4th line a touch stronger. Both loops are bounded: a
    // very short viewport makes Width() huge and would queue a line per pixel.
    int i = 0;
    for (float x = 0; x < g_width && i < kMaxGridLines; x += theme::kGridStep, i++) {
        VLine(x, 0, kHeight, i % 4 ? theme::kGrid : theme::kGrid.Alpha(2.0f));
    }
    i = 0;
    for (float y = 0; y < kHeight && i < kMaxGridLines; y += theme::kGridStep, i++) {
        HLine(0, y, g_width, i % 4 ? theme::kGrid : theme::kGrid.Alpha(2.0f));
    }
}

std::string Upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace skaar::ui
