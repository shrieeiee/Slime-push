#include "gfx.hpp"
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb_truetype.h"
#pragma GCC diagnostic pop
#include "fonts_data.h"

struct Gfx::Atlas {
    SDL_Texture* tex = nullptr;
    stbtt_packedchar pc[95];
    float cap = 0;      // capital height in pixels at this size
    float ascent = 0;
};


Col mix(Col a, Col b, float t) {
    auto L = [&](uint8_t x, uint8_t y) { return (uint8_t)(x + (y - x) * t); };
    return Col{L(a.r, b.r), L(a.g, b.g), L(a.b, b.b), L(a.a, b.a)};
}

// palette lifted from Pocket Bloop
const Theme& themeLight() {
    static Theme t{hex(0xe4f3ef), hex(0xd3ebe5), hex(0xffe4ef), hex(0xfffafd), hex(0x3b2b4f), hex(0x7a6790),
                   hex(0x3b2b4f), hex(0xf3fbff), hex(0xe7d4ee), hex(0x3b2b4f), hex(0xffcf4d), hex(0xff6f9f)};
    return t;
}
const Theme& themeDark() {
    static Theme t{hex(0x191326), hex(0x221a33), hex(0x2b2141), hex(0x231b36), hex(0xf6ecff), hex(0xb4a3cc),
                   hex(0x6a579e), hex(0x1e172f), hex(0x33284c), hex(0x0b0714), hex(0xffcf4d), hex(0xff6f9f)};
    return t;
}
Col pieceColor(int type) {
    static const uint32_t c[9] = {0xffffff, 0x6fd0dc, 0xffcf4d, 0x9c8af0, 0x46b98a, 0xff7aa8, 0x4fbfd8, 0xffa94d, 0xc9b6d8};
    return hex(c[(type < 0 || type > 8) ? 0 : type]);
}

static SDL_Texture* makeTex(SDL_Renderer* r, int w, int h, float (*cov)(float, float, int, int)) {
    std::vector<uint32_t> px((size_t)w * h);
    const int SS = 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx)
                    a += cov(x + (sx + 0.5f) / SS, y + (sy + 0.5f) / SS, w, h);
            a /= SS * SS;
            uint32_t A = (uint32_t)(a * 255.0f + 0.5f);
            px[(size_t)y * w + x] = (A << 24) | 0x00ffffffu;
        }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}

bool Gfx::init(SDL_Renderer* ren) {
    r = ren;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    auto disc = [](float x, float y, int w, int h) {
        float dx = x - w / 2.0f, dy = y - h / 2.0f;
        return (dx * dx + dy * dy <= (w / 2.0f - 1) * (w / 2.0f - 1)) ? 1.0f : 0.0f;
    };
    circ = makeTex(r, 512, 512, disc);      // big shapes stay crisp
    circSmall = makeTex(r, 64, 64, disc);   // small dots don't shimmer
    // smile arc: lower part of a ring, round ends
    arc = makeTex(r, 128, 64, [](float x, float y, int, int) {
        float cx = 64, cy = 0, R = 50, th = 11;
        float dx = x - cx, dy = y - cy;
        float d = std::sqrt(dx * dx + dy * dy);
        float ang = std::atan2(dy, dx);                    // 0..pi in lower half
        const float a0 = 0.45f, a1 = 3.14159f - 0.45f;
        if (ang >= a0 && ang <= a1 && std::fabs(d - R) <= th / 2) return 1.0f;
        for (float a : {a0, a1}) {                          // round caps
            float ex = cx + std::cos(a) * R, ey = cy + std::sin(a) * R;
            if ((x - ex) * (x - ex) + (y - ey) * (y - ey) <= th * th / 4) return 1.0f;
        }
        return 0.0f;
    });
    return circ && arc;
}

void Gfx::destroy() {
    for (auto& kv : atlases) { if (kv.second->tex) SDL_DestroyTexture(kv.second->tex); delete kv.second; }
    atlases.clear();
    if (circ) SDL_DestroyTexture(circ);
    if (circSmall) SDL_DestroyTexture(circSmall);
    if (arc) SDL_DestroyTexture(arc);
    circ = arc = nullptr;
}

void Gfx::clear(Col c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255); SDL_RenderClear(r); }

void Gfx::rect(float x, float y, float w, float h, Col c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_FRect f{x, y, w, h};
    SDL_RenderFillRectF(r, &f);
}

void Gfx::ellipse(float cx, float cy, float rx, float ry, Col c) {
    if (rx <= 0 || ry <= 0 || c.a == 0) return;
    bool big = std::max(rx, ry) > 20;
    SDL_Texture* t = big ? circ : circSmall;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    // the texture disc has a 1px transparent margin: scale a touch
    float k = big ? 256.0f / 255.0f : 32.0f / 31.0f;
    SDL_FRect d{cx - rx * k, cy - ry * k, rx * 2 * k, ry * 2 * k};
    SDL_RenderCopyF(r, t, nullptr, &d);
}

void Gfx::circle(float cx, float cy, float rad, Col c) { ellipse(cx, cy, rad, rad, c); }

void Gfx::rrect(float x, float y, float w, float h, float rad, Col c) {
    if (w <= 0 || h <= 0) return;
    rad = std::fmin(rad, std::fmin(w, h) / 2);
    if (c.a == 255) {
        rect(x + rad, y, w - 2 * rad, h, c);
        rect(x, y + rad, rad, h - 2 * rad, c);
        rect(x + w - rad, y + rad, rad, h - 2 * rad, c);
        float cr = rad + 0.25f;  // AA edge sits inside the hard rect edges; split the difference
        circle(x + rad, y + rad, cr, c);
        circle(x + w - rad, y + rad, cr, c);
        circle(x + rad, y + h - rad, cr, c);
        circle(x + w - rad, y + h - rad, cr, c);
    } else {
        // translucent: avoid overlapping regions doubling up
        rect(x + rad, y, w - 2 * rad, rad, c);
        rect(x, y + rad, w, h - 2 * rad, c);
        rect(x + rad, y + h - rad, w - 2 * rad, rad, c);
        SDL_Rect clip;
        SDL_bool had = SDL_RenderIsClipEnabled(r);
        if (had) SDL_RenderGetClipRect(r, &clip);
        auto corner = [&](float cx, float cy, float qx, float qy) {
            SDL_Rect q{(int)std::floor(qx), (int)std::floor(qy), (int)std::ceil(rad), (int)std::ceil(rad)};
            SDL_RenderSetClipRect(r, &q);
            circle(cx, cy, rad, c);
        };
        corner(x + rad, y + rad, x, y);
        corner(x + w - rad, y + rad, x + w - rad, y);
        corner(x + rad, y + h - rad, x, y + h - rad);
        corner(x + w - rad, y + h - rad, x + w - rad, y + h - rad);
        if (had) SDL_RenderSetClipRect(r, &clip); else SDL_RenderSetClipRect(r, nullptr);
    }
}

void Gfx::smile(float cx, float cy, float w, float h, Col c) {
    SDL_SetTextureColorMod(arc, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(arc, c.a);
    SDL_FRect d{cx - w / 2, cy - h / 2, w, h};
    SDL_RenderCopyF(r, arc, nullptr, &d);
}

void Gfx::line(float x1, float y1, float x2, float y2, float th, Col c) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    int n = (int)(len / (th * 0.35f)) + 1;
    for (int i = 0; i <= n; ++i) {
        float t = (float)i / n;
        circle(x1 + dx * t, y1 + dy * t, th / 2, c);
    }
}

// ------------------------------------------------------------------ font

static stbtt_fontinfo* fontInfo(int f) {
    static stbtt_fontinfo info[2];
    static bool ready[2] = {false, false};
    if (!ready[f]) {
        const unsigned char* d = f == F_DISPLAY ? FONT_MOCHIY : FONT_MPLUS;
        stbtt_InitFont(&info[f], d, stbtt_GetFontOffsetForIndex(d, 0));
        ready[f] = true;
    }
    return &info[f];
}

// pixel height that gives a capital height of `cap` pixels
static float pxForCap(int f, float cap) {
    stbtt_fontinfo* fi = fontInfo(f);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    float capUnits = (float)y1;
    float hUnits = (float)(asc - desc);
    return cap * hUnits / capUnits;
}

Gfx::Atlas* Gfx::atlas(int font, float size) {
    int key = font * 1000 + (int)std::lround(size);
    auto it = atlases.find(key);
    if (it != atlases.end()) return it->second;
    Atlas* a = new Atlas();
    float px = pxForCap(font, (float)(key % 1000));
    const unsigned char* data = font == F_DISPLAY ? FONT_MOCHIY : FONT_MPLUS;
    int W = 256, H = 256;
    std::vector<unsigned char> bmp;
    for (;;) {
        bmp.assign((size_t)W * H, 0);
        stbtt_pack_context pc{};
        stbtt_PackBegin(&pc, bmp.data(), W, H, 0, 2, nullptr);
        stbtt_PackSetOversampling(&pc, px < 40 ? 2 : 1, px < 40 ? 2 : 1);
        int ok = stbtt_PackFontRange(&pc, data, 0, px, 32, 95, a->pc);
        stbtt_PackEnd(&pc);
        if (ok) break;
        if (W <= H) W *= 2; else H *= 2;
        if (W > 4096) break;
    }
    std::vector<uint32_t> px32((size_t)W * H);
    for (size_t i = 0; i < px32.size(); ++i) px32[i] = ((uint32_t)bmp[i] << 24) | 0x00ffffffu;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px32.data(), W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
    a->tex = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(a->tex, SDL_ScaleModeLinear);
    stbtt_fontinfo* fi = fontInfo(font);
    float sc = stbtt_ScaleForPixelHeight(fi, px);
    int asc, desc, gap; stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    int x0, y0, x1, y1; stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1);
    a->ascent = asc * sc;
    a->cap = y1 * sc;
    atlases[key] = a;
    return a;
}

float Gfx::measure(const char* s, float size, int font) {
    Atlas* a = atlas(font, size);
    float w = 0;
    for (const char* p = s; *p; ++p) {
        int c = (unsigned char)*p;
        if (c < 32 || c > 126) c = '?';
        w += a->pc[c - 32].xadvance;
    }
    return w;
}

void Gfx::run(Atlas* a, float x, float y, const char* s, Col c) {
    SDL_SetTextureColorMod(a->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(a->tex, c.a);
    float cx = x, cy = y + a->cap;    // baseline
    int tw, th; SDL_QueryTexture(a->tex, nullptr, nullptr, &tw, &th);
    for (const char* p = s; *p; ++p) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) ch = '?';
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(a->pc, tw, th, ch - 32, &cx, &cy, &q, 0);
        if (ch == ' ') continue;
        SDL_Rect src{(int)std::lround(q.s0 * tw), (int)std::lround(q.t0 * th),
                     (int)std::lround((q.s1 - q.s0) * tw), (int)std::lround((q.t1 - q.t0) * th)};
        SDL_FRect dst{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        SDL_RenderCopyF(r, a->tex, &src, &dst);
    }
}

float Gfx::print(float x, float y, float size, const TextStyle& st, const char* s, Align al) {
    if (!s || !*s) return 0;
    Atlas* a = atlas(st.font, size);
    float w = measure(s, size, st.font);
    if (al == AL_CENTER) x -= w / 2; else if (al == AL_RIGHT) x -= w;
    auto ring = [&](float ox, float oy, Col c) {
        float R = st.outlineW;
        int n = std::max(8, (int)std::ceil(6.2832f * R / 1.4f));
        for (int i = 0; i < n; ++i) {
            float an = 6.2832f * i / n;
            run(a, x + ox + std::cos(an) * R, y + oy + std::sin(an) * R, s, c);
        }
        if (R > 2.5f)
            for (int i = 0; i < 8; ++i) {
                float an = 6.2832f * i / 8 + 0.39f;
                run(a, x + ox + std::cos(an) * R * 0.55f, y + oy + std::sin(an) * R * 0.55f, s, c);
            }
    };
    if (st.outline.a && st.outlineW > 0) {
        if (st.shadow > 0) ring(0, st.shadow, st.outline);
        ring(0, 0, st.outline);
    } else if (st.shadow > 0) {
        run(a, x, y + st.shadow, s, st.outline.a ? st.outline : Col{0, 0, 0, 120});
    }
    run(a, x, y, s, st.fill);
    return w;
}

float Gfx::text(float x, float y, float size, Col c, const char* s, Align al, int font) {
    TextStyle st; st.font = font; st.fill = c;
    return print(x, y, size, st, s, al);
}

// ------------------------------------------------------------------ slime
// Shape follows the Pocket Bloop mascot (viewBox 200x170, body 30..170 wide, 52..164 tall).
void Gfx::slime(float cx, float by, float w, Col body, Col ink, float squash, float blink, int mood, float t) {
    float s = w / 140.0f;
    float sx = s * (1.0f + squash * 0.18f), sy = s * (1.0f - squash * 0.18f);
    auto X = [&](float vx) { return cx + (vx - 100) * sx; };
    auto Y = [&](float vy) { return by + (vy - 164) * sy; };
    float o = 8.0f * s;
    // ink outline (expanded body), then body
    for (int pass = 0; pass < 2; ++pass) {
        float e = pass == 0 ? o : 0;
        Col c = pass == 0 ? ink : body;
        ellipse(X(100), Y(110), 70 * sx + e, 58 * sy + e, c);
        rrect(X(30) - e, Y(104) - e, 140 * sx + 2 * e, 60 * sy + 2 * e, 30 * s + e, c);
    }
    // shine
    ellipse(X(66), Y(80), 13 * sx, 7.5f * sy, Col{255, 255, 255, 150});
    ellipse(X(84), Y(70), 4 * sx, 3 * sy, Col{255, 255, 255, 120});
    // eyes
    float eyeY = Y(106);
    if (mood == 3) {                       // sad: closed curved eyes
        smile(X(78), eyeY, 18 * sx, 7 * sy, ink);
        smile(X(122), eyeY, 18 * sx, 7 * sy, ink);
    } else {
        float ry = 9.5f * sy * (1.0f - 0.9f * blink);
        float look = (mood == 1) ? std::sin(t * 9) * 2.5f * sx : 0;
        ellipse(X(78) + look, eyeY, 7.5f * sx, ry, ink);
        ellipse(X(122) + look, eyeY, 7.5f * sx, ry, ink);
        if (blink < 0.5f) {
            circle(X(80.5f) + look, Y(102), 2.8f * s, Col{255, 255, 255, 255});
            circle(X(124.5f) + look, Y(102), 2.8f * s, Col{255, 255, 255, 255});
        }
    }
    // blush
    ellipse(X(60), Y(125), 10 * sx, 5.5f * sy, hex(0xff6f9f, 128));
    ellipse(X(140), Y(125), 10 * sx, 5.5f * sy, hex(0xff6f9f, 128));
    // mouth
    if (mood == 2) {                       // excited: open mouth with tongue
        ellipse(X(100), Y(126), 9 * sx, 7.5f * sy, ink);
        ellipse(X(100), Y(130), 5 * sx, 3 * sy, hex(0xff8fb3));
    } else if (mood == 1) {                // worried: small o
        ellipse(X(100), Y(127), 4.5f * sx, 5 * sy, ink);
    } else if (mood == 3) {                // sad: flat wobbly line
        rect(X(92), Y(126), 16 * sx, 3.2f * sy, ink);
    } else {
        smile(X(100), Y(126), 20 * sx, 10 * sy, ink);
    }
}
