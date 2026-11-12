#include "ui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "proc.h"
#include "threads.h"

SDL_Color withAlpha(SDL_Color c, int a) { c.a = (Uint8)std::clamp(a, 0, 255); return c; }
SDL_Color mixColour(SDL_Color a, SDL_Color b, float t) {
    return SDL_Color{(Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t), (Uint8)(a.b + (b.b - a.b) * t), (Uint8)(a.a + (b.a - a.a) * t)};
}

float smoothstep(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3 - 2 * t); }
float easeOutCubic(float t) { t = std::clamp(t, 0.f, 1.f); const float u = 1 - t; return 1 - u * u * u; }

bool smoothTo(float& now, float target, float dtMs, float tauMs, float snap) {
    if (tauMs <= 0 || std::abs(target - now) < snap) { now = target; return false; }
    now += (target - now) * (1.f - std::exp(-dtMs / tauMs));
    return true;
}

bool systemAllowsTransparency() {
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return true;
    return v != 0;
}

bool systemAllowsAnimation() {
    BOOL on = TRUE;
    const UINT kGetClientAreaAnimation = 0x1042;  // SPI_GETCLIENTAREAANIMATION; MinGW hides it behind a version macro
    if (!SystemParametersInfoW(kGetClientAreaAnimation, 0, &on, 0)) return true;
    return on != FALSE;
}

// ------------------------------------------------------------ glass
// Per pixel, the signed distance to the panel's rounded edge gives a rim position and an outward
// normal. A squircle height curve through Snell's law gives how far light bends there, and the
// backdrop is sampled that far inward. The flat middle passes straight through and the rim
// compresses it. (From Sandscope, which took it from Dusk Reader.)
static const std::vector<float>& refractionCurve() {
    static const std::vector<float> curve = [] {
        const int n = 128;
        std::vector<float> c(n + 1, 0.f);
        auto height = [](double t) { return std::pow(1.0 - std::pow(1.0 - t, 4.0), 0.25); };
        double peak = 0;
        for (int i = 0; i <= n; ++i) {
            const double t = std::clamp(i / (double)n, 0.002, 0.998);
            const double slope = (height(std::min(t + 0.002, 1.0)) - height(t - 0.002)) / 0.004;
            const double incidence = std::atan(slope);
            const double bend = incidence - std::asin(std::sin(incidence) / 1.5);
            c[i] = (float)std::tan(bend);
            peak = std::max(peak, (double)c[i]);
        }
        for (auto& v : c) v = (float)(v / peak);
        return c;
    }();
    return curve;
}

void Glass::destroy() { if (tex) SDL_DestroyTexture(tex); tex = nullptr; texW = texH = 0; builtFor = 0; }

void Glass::setGeometry(SDL_Rect nr, int rad, bool surf, float scale) {
    if (SDL_RectEquals(&nr, &r) && rad == radius && surf == surface && !offR.empty()) return;
    r = nr; radius = rad; surface = surf; builtFor = 0;
    if (r.w <= 0 || r.h <= 0) return;
    const size_t n = (size_t)r.w * r.h;
    offR.assign(n, -1); offG.assign(n, -1); offB.assign(n, -1);
    bevel.assign(n, 0.f); specAdd.assign(n, 0.f); alpha.assign(n, 0.f);
    bent.resize(n); tmp.resize(n); out.resize(n);
    const std::vector<float>& curve = refractionCurve();
    const float bezel = std::min((float)radius, (surface ? 18.f : 13.f) * scale);
    const float strength = (surface ? 24.f : 11.f) * scale;
    const float rim = std::max(1.2f, 1.3f * scale);
    const float hx = r.w / 2.f, hy = r.h / 2.f, rd = (float)radius;
    const float lightAngle = -2.2f;  // upper left
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const size_t i = (size_t)y * r.w + x;
            const float px = x + 0.5f - hx, py = y + 0.5f - hy;
            const float qx = std::abs(px) - (hx - rd), qy = std::abs(py) - (hy - rd);
            const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
            const float outside = std::sqrt(ox * ox + oy * oy);
            const float d = rd - outside - std::min(std::max(qx, qy), 0.f);
            float nx = 0, ny = 0;
            if (outside > 0) { nx = ox / outside; ny = oy / outside; }
            else if (qx > qy) nx = 1; else ny = 1;
            if (px < 0) nx = -nx;
            if (py < 0) ny = -ny;
            alpha[i] = std::clamp(d + 0.5f, 0.f, 1.f);
            const float k = std::cos(std::atan2(ny, nx) - lightAngle);
            if (d < bezel && d > 0) {
                const float shift = curve[(size_t)(std::clamp(d / bezel, 0.f, 1.f) * (curve.size() - 1))] * strength;
                auto at = [&](float amount) {
                    const int sx = std::clamp((int)std::lround(x - nx * amount), 0, r.w - 1);
                    const int sy = std::clamp((int)std::lround(y - ny * amount), 0, r.h - 1);
                    return sy * r.w + sx;
                };
                offR[i] = at(shift * 0.98f); offG[i] = at(shift); offB[i] = at(shift * 1.02f);
                const float e = 1 - d / bezel;
                bevel[i] = e * e * k * 16.f;
            }
            if (d < rim) {
                const float e = 1 - std::max(d, 0.f) / rim;
                const float on = std::max(k, 0.f), off = std::max(-k, 0.f);
                specAdd[i] = e * (on * on * 0.55f + off * off * 0.22f + 0.05f);
            }
        }
    }
}

void Glass::boxBlur(std::vector<Uint32>& px, int blur) {
    if (blur < 1) return;
    const int w0 = r.w, h0 = r.h, window = 2 * blur + 1;
    auto pass = [&](const std::vector<Uint32>& src, std::vector<Uint32>& dst, int len, int lines, int along, int across) {
        parallelRange(lines, [&](int l0, int l1) {
            for (int l = l0; l < l1; ++l) {
                const Uint32* line = src.data() + (size_t)l * across;
                Uint32* outLine = dst.data() + (size_t)l * across;
                int cr = 0, cg = 0, cb = 0;
                auto at = [&](int i) { return line[(size_t)std::clamp(i, 0, len - 1) * along]; };
                for (int i = -blur; i <= blur; ++i) { const Uint32 v = at(i); cr += (v >> 16) & 255; cg += (v >> 8) & 255; cb += v & 255; }
                for (int i = 0; i < len; ++i) {
                    outLine[(size_t)i * along] = 0xFF000000u | ((Uint32)(cr / window) << 16) | ((Uint32)(cg / window) << 8) | (Uint32)(cb / window);
                    const Uint32 o = at(i - blur), n = at(i + blur + 1);
                    cr += (int)((n >> 16) & 255) - (int)((o >> 16) & 255);
                    cg += (int)((n >> 8) & 255) - (int)((o >> 8) & 255);
                    cb += (int)(n & 255) - (int)(o & 255);
                }
            }
        });
    };
    pass(px, tmp, w0, h0, 1, w0);
    pass(tmp, px, h0, w0, w0, 1);
}

void Glass::refresh(SDL_Renderer* ren, const std::vector<Uint32>& scene, int sceneW, int sceneH, float scale, unsigned version) {
    if (builtFor == version || r.w <= 0 || r.h <= 0 || scene.empty()) return;
    builtFor = version;
    parallelRange(r.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int sy = std::clamp(r.y + y, 0, sceneH - 1);
            for (int x = 0; x < r.w; ++x) tmp[(size_t)y * r.w + x] = scene[(size_t)sy * sceneW + std::clamp(r.x + x, 0, sceneW - 1)];
        }
    });
    parallelRange(r.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < r.w; ++x) {
                const size_t i = (size_t)y * r.w + x;
                if (offG[i] < 0) { bent[i] = tmp[i]; continue; }
                bent[i] = 0xFF000000u | (tmp[offR[i]] & 0xFF0000u) | (tmp[offG[i]] & 0x00FF00u) | (tmp[offB[i]] & 0x0000FFu);
            }
    });
    // Bigger glass is more frosted, so text on it stays readable.
    boxBlur(bent, (int)((surface ? 7.f : 2.f) * scale));
    if (surface) boxBlur(bent, (int)(4.f * scale));
    const float tint = surface ? 0.70f : 0.52f;
    parallelRange(r.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < r.w; ++x) {
                const size_t i = (size_t)y * r.w + x;
                if (alpha[i] <= 0) { out[i] = 0; continue; }
                float cr = (bent[i] >> 16) & 255, cg = (bent[i] >> 8) & 255, cb = bent[i] & 255;
                const float luma = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
                cr = luma + (cr - luma) * 1.5f; cg = luma + (cg - luma) * 1.5f; cb = luma + (cb - luma) * 1.5f;
                cr += (kBar.r - cr) * tint; cg += (kBar.g - cg) * tint; cb += (kBar.b - cb) * tint;
                cr += bevel[i]; cg += bevel[i]; cb += bevel[i];
                const float s = specAdd[i];
                cr += (255 - cr) * s; cg += (255 - cg) * s; cb += (255 - cb) * s;
                const Uint32 a = (Uint32)(alpha[i] * 255);
                out[i] = (a << 24) | ((Uint32)std::clamp(cr, 0.f, 255.f) << 16) | ((Uint32)std::clamp(cg, 0.f, 255.f) << 8) | (Uint32)std::clamp(cb, 0.f, 255.f);
            }
    });
    if (!tex || texW != r.w || texH != r.h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, r.w, r.h);
        texW = r.w; texH = r.h;
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    if (tex) SDL_UpdateTexture(tex, nullptr, out.data(), r.w * 4);
}

// ------------------------------------------------------------ painter
void Painter::fill(SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren, &r);
}

// SDL has no rounded rectangles, so one small alpha texture per corner size is stretched
// nine-patch style. A positive key is a crisp anti-aliased corner, a negative one a soft shadow.
SDL_Texture* Painter::maskTexture(int key) {
    auto it = masks.find(key);
    if (it != masks.end()) return it->second;
    const int c = std::abs(key), size = 2 * c + 1;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;
    for (int y = 0; y < size; ++y) {
        Uint32* row = (Uint32*)((Uint8*)surf->pixels + y * surf->pitch);
        for (int x = 0; x < size; ++x) {
            const float dx = std::abs(x - (float)c), dy = std::abs(y - (float)c), dist = std::sqrt(dx * dx + dy * dy);
            float a;
            if (key > 0) a = std::clamp(c + 0.5f - dist, 0.f, 1.f);
            else { const float t = std::clamp(1.f - dist / c, 0.f, 1.f); a = t * t * (3 - 2 * t); }
            row[x] = ((Uint32)(a * 255) << 24) | 0xFFFFFF;
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    masks[key] = t;
    return t;
}

void Painter::ninePatch(int key, SDL_Rect r, SDL_Color col) {
    SDL_Texture* t = maskTexture(key);
    if (!t) { fill(r, col); return; }
    const int c = std::abs(key);
    SDL_SetTextureColorMod(t, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(t, col.a);
    const int xs[4] = {r.x, r.x + c, r.x + r.w - c, r.x + r.w}, ys[4] = {r.y, r.y + c, r.y + r.h - c, r.y + r.h};
    const int sx[4] = {0, c, c + 1, 2 * c + 1};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            SDL_Rect src{sx[i], sx[j], sx[i + 1] - sx[i], sx[j + 1] - sx[j]};
            SDL_Rect dst{xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j]};
            if (dst.w > 0 && dst.h > 0) SDL_RenderCopy(ren, t, &src, &dst);
        }
}

void Painter::rounded(SDL_Rect r, int radius, SDL_Color col) {
    if (r.w <= 0 || r.h <= 0) return;
    radius = std::min({radius, r.w / 2, r.h / 2});
    if (radius < 1) fill(r, col); else ninePatch(radius, r, col);
}

void Painter::softShadow(SDL_Rect r, int radius, int spread, Uint8 alpha) {
    const int c = radius + spread;
    ninePatch(-c, SDL_Rect{r.x - spread, r.y - spread + spread / 3, r.w + 2 * spread, r.h + 2 * spread}, SDL_Color{0, 0, 0, alpha});
}

Painter::TextEntry& Painter::textEntry(const std::string& text, int font) {
    const std::string key = std::to_string(font) + "|" + text;
    auto it = textCache.find(key);
    if (it != textCache.end()) return it->second;
    // Time readouts and slider values produce new strings constantly, so the cache needs a ceiling.
    if (textCache.size() > 1500) {
        for (auto& e : textCache) if (e.second.tex) SDL_DestroyTexture(e.second.tex);
        textCache.clear();
    }
    TextEntry e{nullptr, 0, 0};
    if (!text.empty() && fonts[font]) {
        SDL_Surface* surf = TTF_RenderUTF8_Blended(fonts[font], text.c_str(), SDL_Color{255, 255, 255, 255});
        if (surf) {
            e.tex = SDL_CreateTextureFromSurface(ren, surf);
            e.w = surf->w; e.h = surf->h;
            SDL_FreeSurface(surf);
            if (e.tex) SDL_SetTextureBlendMode(e.tex, SDL_BLENDMODE_BLEND);
        }
    }
    return textCache[key] = e;
}

void Painter::text(const std::string& t, int x, int y, int font, SDL_Color c) {
    TextEntry& e = textEntry(t, font);
    if (!e.tex) return;
    SDL_SetTextureColorMod(e.tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e.tex, c.a);
    SDL_Rect d{x, y, e.w, e.h};
    SDL_RenderCopy(ren, e.tex, nullptr, &d);
}

void Painter::textCentred(const std::string& t, SDL_Rect r, int font, SDL_Color c) {
    TextEntry& e = textEntry(t, font);
    text(t, r.x + (r.w - e.w) / 2, r.y + (r.h - e.h) / 2, font, c);
}

static size_t prevCharStart(const std::string& s, size_t i) {
    while (i > 0) { --i; if (((unsigned char)s[i] & 0xC0) != 0x80) break; }
    return i;
}

std::string Painter::fit(const std::string& t, int font, int maxW) {
    if (textWidth(t, font) <= maxW) return t;
    std::string s = t;
    while (!s.empty() && textWidth(s + "\xE2\x80\xA6", font) > maxW) s.erase(prevCharStart(s, s.size()));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + "\xE2\x80\xA6";
}

std::vector<std::string> Painter::wrap(const std::string& t, int font, int maxW) {
    std::vector<std::string> lines;
    std::string line;
    size_t pos = 0;
    while (pos < t.size()) {
        size_t sp = t.find(' ', pos);
        if (sp == std::string::npos) sp = t.size();
        const std::string word = t.substr(pos, sp - pos);
        pos = sp + 1;
        if (word.empty()) continue;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && textWidth(candidate, font) > maxW) { lines.push_back(line); line = word; }
        else line = candidate;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

void Painter::ring(int cx, int cy, int radius, int thickness, float fraction, SDL_Color c) {
    fraction = std::clamp(fraction, 0.f, 1.f);
    const int segs = std::max(8, (int)(96 * fraction));
    if (fraction <= 0) return;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const float ro = (float)radius, ri = (float)(radius - thickness);
    const SDL_Color col = c;
    for (int i = 0; i <= segs; ++i) {
        const float a = -1.5707963f + 6.2831853f * fraction * i / segs;
        const float ca = std::cos(a), sa = std::sin(a);
        v.push_back(SDL_Vertex{{cx + ca * ro, cy + sa * ro}, col, {0, 0}});
        v.push_back(SDL_Vertex{{cx + ca * ri, cy + sa * ri}, col, {0, 0}});
        if (i > 0) {
            const int b = (i - 1) * 2;
            idx.insert(idx.end(), {b, b + 1, b + 2, b + 1, b + 3, b + 2});
        }
    }
    SDL_RenderGeometry(ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

void Painter::destroy() {
    for (auto& e : textCache) if (e.second.tex) SDL_DestroyTexture(e.second.tex);
    textCache.clear();
    for (auto& m : masks) if (m.second) SDL_DestroyTexture(m.second);
    masks.clear();
}

// ------------------------------------------------------------ pictures
static SDL_Surface* loadBmpArgb(const std::string& path) {
    // SDL_LoadBMP takes a UTF-8 path and opens it through the C runtime, which on Windows means the
    // ANSI code page. Reading the bytes ourselves keeps non-Latin folder names working.
    const std::string bytes = readTextFile(path);
    if (bytes.empty()) return nullptr;
    SDL_Surface* raw = SDL_LoadBMP_RW(SDL_RWFromConstMem(bytes.data(), (int)bytes.size()), 1);
    if (!raw) return nullptr;
    SDL_Surface* conv = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    return conv;
}

static Uint32 sampleBilinear(const SDL_Surface* s, float fx, float fy) {
    fx = std::clamp(fx, 0.f, (float)s->w - 1.001f);
    fy = std::clamp(fy, 0.f, (float)s->h - 1.001f);
    const int x = (int)fx, y = (int)fy;
    const float tx = fx - x, ty = fy - y;
    auto at = [&](int xx, int yy) { return ((const Uint32*)((const Uint8*)s->pixels + yy * s->pitch))[xx]; };
    const Uint32 a = at(x, y), b = at(x + 1, y), c = at(x, y + 1), d = at(x + 1, y + 1);
    Uint32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float top = ((a >> sh) & 255) * (1 - tx) + ((b >> sh) & 255) * tx;
        const float bottom = ((c >> sh) & 255) * (1 - tx) + ((d >> sh) & 255) * tx;
        out |= (Uint32)(top * (1 - ty) + bottom * ty + 0.5f) << sh;
    }
    return out;
}

SDL_Texture* loadRoundedPicture(SDL_Renderer* ren, const std::string& bmpPath, int size, int radius) {
    SDL_Surface* src = loadBmpArgb(bmpPath);
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!dst) { SDL_FreeSurface(src); return nullptr; }
    // A box of source pixels per target pixel, so a 512 px cover shrunk to 40 px doesn't shimmer.
    const float step = (float)src->w / size;
    const int taps = std::max(1, (int)std::ceil(step / 1.5f));
    const float hx = size / 2.f, rd = (float)radius;
    for (int y = 0; y < size; ++y) {
        Uint32* row = (Uint32*)((Uint8*)dst->pixels + y * dst->pitch);
        for (int x = 0; x < size; ++x) {
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < taps; ++j)
                for (int i = 0; i < taps; ++i) {
                    const Uint32 p = sampleBilinear(src, (x + (i + 0.5f) / taps) * step - 0.5f, (y + (j + 0.5f) / taps) * step * src->h / src->w - 0.5f);
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
                }
            const float n = (float)(taps * taps);
            const float qx = std::abs(x + 0.5f - hx) - (hx - rd), qy = std::abs(y + 0.5f - hx) - (hx - rd);
            const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
            const float d = rd - std::sqrt(ox * ox + oy * oy) - std::min(std::max(qx, qy), 0.f);
            const Uint32 a = (Uint32)(std::clamp(d + 0.5f, 0.f, 1.f) * 255);
            row[x] = (a << 24) | ((Uint32)(r / n) << 16) | ((Uint32)(g / n) << 8) | (Uint32)(b / n);
        }
    }
    SDL_FreeSurface(src);
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, dst);
    SDL_FreeSurface(dst);
    if (t) { SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear); }
    return t;
}

bool loadBlurredPixels(const std::string& bmpPath, int size, std::vector<float>& rgb) {
    SDL_Surface* src = loadBmpArgb(bmpPath);
    if (!src) return false;
    rgb.assign((size_t)size * size * 3, 0.f);
    const float step = (float)src->w / size;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float r = 0, g = 0, b = 0;
            const int taps = 4;
            for (int j = 0; j < taps; ++j)
                for (int i = 0; i < taps; ++i) {
                    const Uint32 p = sampleBilinear(src, (x + (i + 0.5f) / taps) * step, (y + (j + 0.5f) / taps) * step * src->h / src->w);
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
                }
            float* o = rgb.data() + ((size_t)y * size + x) * 3;
            o[0] = r / 16; o[1] = g / 16; o[2] = b / 16;
        }
    SDL_FreeSurface(src);
    std::vector<float> tmp(rgb.size());
    const int rad = std::max(1, size / 10);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                for (int c = 0; c < 3; ++c) {
                    float s = 0;
                    for (int k = -rad; k <= rad; ++k) s += rgb[((size_t)y * size + std::clamp(x + k, 0, size - 1)) * 3 + c];
                    tmp[((size_t)y * size + x) * 3 + c] = s / (2 * rad + 1);
                }
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                for (int c = 0; c < 3; ++c) {
                    float s = 0;
                    for (int k = -rad; k <= rad; ++k) s += tmp[((size_t)std::clamp(y + k, 0, size - 1) * size + x) * 3 + c];
                    rgb[((size_t)y * size + x) * 3 + c] = s / (2 * rad + 1);
                }
    }
    return true;
}
