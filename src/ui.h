// The drawing layer shared by every screen: Dusk Reader's dark theme with a white accent, its
// Liquid Glass panels, rounded shapes, cached text, and easing.
#pragma once
#include <SDL.h>
#include <SDL_ttf.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Dusk Reader's "Dark" theme. The accent is white instead of coral, so anything drawn on top of
// the accent (the play glyph, a selected pill's label) uses kOnAccent to stay readable.
const SDL_Color kBase{30, 30, 30, 255}, kText{226, 226, 226, 255}, kBar{52, 52, 52, 255}, kMuted{158, 158, 158, 255};
const SDL_Color kAccent{255, 255, 255, 255}, kOnAccent{22, 22, 22, 255};
const SDL_Color kWarn{240, 180, 120, 255};  // only for errors, which must not look like normal text

SDL_Color withAlpha(SDL_Color c, int a);
SDL_Color mixColour(SDL_Color a, SDL_Color b, float t);

float smoothstep(float t);
float easeOutCubic(float t);
// Moves now toward target with a time constant, frame-rate independent. Returns true while moving.
bool smoothTo(float& now, float target, float dtMs, float tauMs, float snap = 0.5f);

// Windows' own switches: Settings > Accessibility > Visual effects.
bool systemAllowsTransparency();
bool systemAllowsAnimation();

struct Glass {
    SDL_Rect r{0, 0, 0, 0};
    int radius = 0;
    bool surface = true;
    SDL_Texture* tex = nullptr;
    int texW = 0, texH = 0;
    std::vector<int> offR, offG, offB;
    std::vector<float> bevel, specAdd, alpha;
    std::vector<Uint32> bent, tmp, out;
    unsigned builtFor = 0;

    void destroy();
    void setGeometry(SDL_Rect nr, int rad, bool surf, float scale);
    void refresh(SDL_Renderer* ren, const std::vector<Uint32>& scene, int sceneW, int sceneH, float scale, unsigned version);

private:
    void boxBlur(std::vector<Uint32>& px, int blur);
};

enum FontId { F_BODY = 0, F_SMALL, F_LABEL, F_TITLE, F_ICON, F_ICON_BIG, F_HERO, F_LYRIC, F_COUNT };

struct Painter {
    SDL_Renderer* ren = nullptr;
    TTF_Font* fonts[F_COUNT] = {};
    float scale = 1.f;

    struct TextEntry { SDL_Texture* tex; int w, h; };
    std::unordered_map<std::string, TextEntry> textCache;
    std::map<int, SDL_Texture*> masks;

    int px(float v) const { return (int)(v * scale + 0.5f); }
    void fill(SDL_Rect r, SDL_Color c);
    void rounded(SDL_Rect r, int radius, SDL_Color c);
    void softShadow(SDL_Rect r, int radius, int spread, Uint8 alpha);
    TextEntry& textEntry(const std::string& text, int font);
    int textWidth(const std::string& t, int font) { return textEntry(t, font).w; }
    int fontHeight(int font) const { return fonts[font] ? TTF_FontHeight(fonts[font]) : 16; }
    void text(const std::string& t, int x, int y, int font, SDL_Color c);
    void textCentred(const std::string& t, SDL_Rect r, int font, SDL_Color c);
    // Cuts at a character boundary and adds an ellipsis so the text fits in maxW.
    std::string fit(const std::string& t, int font, int maxW);
    // Breaks at spaces into lines no wider than maxW.
    std::vector<std::string> wrap(const std::string& t, int font, int maxW);
    // A ring of a given thickness, filled from 12 o'clock clockwise by fraction.
    void ring(int cx, int cy, int radius, int thickness, float fraction, SDL_Color c);
    void destroy();

private:
    SDL_Texture* maskTexture(int key);
    void ninePatch(int key, SDL_Rect r, SDL_Color c);
};

// Loads a BMP and returns a texture of it scaled to size x size with rounded corners baked into
// its alpha, so pictures get the same corners as the panels.
SDL_Texture* loadRoundedPicture(SDL_Renderer* ren, const std::string& bmpPath, int size, int radius);
// The same picture tiny and blurred, as ARGB pixels, for the backdrop.
bool loadBlurredPixels(const std::string& bmpPath, int size, std::vector<float>& rgb);
