// Drawing helpers: anti-aliased circles/ellipses, rounded rects, outlined TrueType text,
// and the Pocket Bloop style slime.
#pragma once
#include <SDL.h>
#include <cstdint>
#include <map>

struct Col { uint8_t r, g, b, a; };
inline Col hex(uint32_t v, uint8_t a = 255) { return Col{(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, a}; }
inline Col withA(Col c, uint8_t a) { c.a = a; return c; }
Col mix(Col a, Col b, float t);

struct Theme {
    Col bg, bgDot, shell, panel, text, muted, line, field, fieldDot, slimeInk, pop, danger;
};
const Theme& themeLight();
const Theme& themeDark();
Col pieceColor(int type);

enum Align { AL_LEFT, AL_CENTER, AL_RIGHT };
enum FontId { F_BODY = 0, F_DISPLAY = 1 };   // M PLUS Rounded ExtraBold, Mochiy Pop One

struct TextStyle {
    int font = F_BODY;
    Col fill{255, 255, 255, 255};
    Col outline{0, 0, 0, 0};
    float outlineW = 0;      // thickness in pixels
    float shadow = 0;        // drop shadow offset (uses outline colour)
};

class Gfx {
public:
    SDL_Renderer* r = nullptr;
    bool init(SDL_Renderer* ren);
    void destroy();
    void clear(Col c);
    void rect(float x, float y, float w, float h, Col c);
    void circle(float cx, float cy, float rad, Col c);
    void ellipse(float cx, float cy, float rx, float ry, Col c);
    void rrect(float x, float y, float w, float h, float rad, Col c);
    void smile(float cx, float cy, float w, float h, Col c);         // arc mouth
    void line(float x1, float y1, float x2, float y2, float th, Col c);
    // y is the top of the capital letters, size is the capital-letter height in pixels
    float print(float x, float y, float size, const TextStyle& st, const char* s, Align al = AL_LEFT);
    float measure(const char* s, float size, int font = F_BODY);
    // shorthand: plain fill, no outline
    float text(float x, float y, float size, Col c, const char* s, Align al = AL_LEFT, int font = F_BODY);
    // Pocket Bloop slime: body centred at cx, bottom at by, width w
    void slime(float cx, float by, float w, Col body, Col ink, float squash, float blink, int mood, float t);
private:
    SDL_Texture* circ = nullptr;
    SDL_Texture* circSmall = nullptr;
    SDL_Texture* arc = nullptr;
    struct Atlas;
    std::map<int, Atlas*> atlases;     // key: font * 1000 + pixel size
    Atlas* atlas(int font, float size);
    void run(Atlas* a, float x, float y, const char* s, Col c);
};
