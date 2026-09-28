#include "raylib.h"

#include "audio_engine.h"
#include "fft.h"
#include "library.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr const char* kMusicDir = "music";
constexpr const char* kLovedFile = "loved.txt";
constexpr const char* kFontFile = "assets/font.ttf";
constexpr int kBars = 48;
constexpr int kFftSize = 1024;

const Color kBg = {10, 10, 14, 255};
const Color kText = {245, 245, 250, 255};
const Color kDim = {150, 150, 165, 255};

// ---------- small helpers ----------

Color Mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(x + (y - x) * t);
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
}

Color Alpha(Color c, unsigned char a) {
    c.a = a;
    return c;
}

std::string FormatTime(double s) {
    const int t = std::max(0, static_cast<int>(s));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}

// pick a theme color from the album art, boosted so it isn't muddy
Color AccentFromImage(const Image& img) {
    Color* px = LoadImageColors(img);
    long r = 0, g = 0, b = 0, n = 0;
    const int step = std::max(1, img.width / 32);
    for (int y = 0; y < img.height; y += step)
        for (int x = 0; x < img.width; x += step) {
            const Color c = px[y * img.width + x];
            r += c.r; g += c.g; b += c.b; ++n;
        }
    UnloadImageColors(px);
    if (n == 0) return {255, 120, 170, 255};
    const Color avg = {static_cast<unsigned char>(r / n), static_cast<unsigned char>(g / n),
                       static_cast<unsigned char>(b / n), 255};
    const Vector3 hsv = ColorToHSV(avg);
    return ColorFromHSV(hsv.x, std::max(hsv.y, 0.45f), std::max(hsv.z, 0.85f));
}

// no cover art: stable color from the title
Color AccentFromString(const std::string& s) {
    unsigned h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return ColorFromHSV(static_cast<float>(h % 360), 0.55f, 0.95f);
}

// ---------- text ----------

Font gFont;

void Text(const std::string& s, float x, float y, float size, Color c) {
    DrawTextEx(gFont, s.c_str(), {x, y}, size, 1.0f, c);
}

float TextWidth(const std::string& s, float size) {
    return MeasureTextEx(gFont, s.c_str(), size, 1.0f).x;
}

// cut text with "..." so it fits maxW (utf-8 safe)
std::string Fit(std::string s, float size, float maxW) {
    if (TextWidth(s, size) <= maxW) return s;
    while (!s.empty() && TextWidth(s + "...", size) > maxW) {
        while (!s.empty()) {
            const unsigned char c = static_cast<unsigned char>(s.back());
            s.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
    }
    return s + "...";
}

// ---------- icons (drawn from shapes, no image files) ----------

void DrawPlayIcon(Vector2 c, float s, Color col) {
    DrawTriangle({c.x - s * 0.3f, c.y - s * 0.5f}, {c.x - s * 0.3f, c.y + s * 0.5f},
                 {c.x + s * 0.55f, c.y}, col);
}

void DrawPauseIcon(Vector2 c, float s, Color col) {
    DrawRectangleRounded({c.x - s * 0.38f, c.y - s * 0.5f, s * 0.26f, s}, 0.5f, 4, col);
    DrawRectangleRounded({c.x + s * 0.12f, c.y - s * 0.5f, s * 0.26f, s}, 0.5f, 4, col);
}

void DrawSkipIcon(Vector2 c, float s, Color col, bool forward) {
    if (forward) {
        DrawTriangle({c.x - s * 0.5f, c.y - s * 0.45f}, {c.x - s * 0.5f, c.y + s * 0.45f},
                     {c.x + s * 0.3f, c.y}, col);
        DrawRectangleRounded({c.x + s * 0.3f, c.y - s * 0.45f, s * 0.15f, s * 0.9f}, 0.5f, 4, col);
    } else {
        DrawTriangle({c.x + s * 0.5f, c.y - s * 0.45f}, {c.x - s * 0.3f, c.y},
                     {c.x + s * 0.5f, c.y + s * 0.45f}, col);
        DrawRectangleRounded({c.x - s * 0.45f, c.y - s * 0.45f, s * 0.15f, s * 0.9f}, 0.5f, 4, col);
    }
}

void DrawHeart(Vector2 c, float s, Color col) {
    const float r = s * 0.28f;
    DrawCircleV({c.x - r, c.y - r * 0.4f}, r, col);
    DrawCircleV({c.x + r, c.y - r * 0.4f}, r, col);
    DrawTriangle({c.x - r * 1.95f, c.y - r * 0.15f}, {c.x, c.y + s * 0.5f},
                 {c.x + r * 1.95f, c.y - r * 0.15f}, col);
}

void DrawEqIcon(Vector2 p, float t, Color col) {
    for (int k = 0; k < 3; ++k) {
        const float h = 6.0f + 10.0f * (0.5f + 0.5f * std::sin(t * 7.0f + k * 1.7f));
        DrawRectangleRounded({p.x + k * 6.0f, p.y - h, 4.0f, h}, 1.0f, 4, col);
    }
}

struct Hit {
    bool hover;
    bool clicked;
};

Hit CircleHit(Vector2 c, float r) {
    const bool h = CheckCollisionPointCircle(GetMousePosition(), c, r);
    return {h, h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)};
}

Hit RectHit(Rectangle rect) {
    const bool h = CheckCollisionPointRec(GetMousePosition(), rect);
    return {h, h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)};
}

// ---------- spectrum visualizer ----------

struct Spectrum {
    std::vector<float> window = std::vector<float>(kFftSize, 0.0f);
    std::vector<std::complex<float>> fft = std::vector<std::complex<float>>(kFftSize);
    size_t writePos = 0;
    float bars[kBars] = {};

    void feed(AudioEngine& engine) {
        float tmp[1024];
        size_t n;
        while ((n = engine.readVisualizer(tmp, 1024)) > 0)
            for (size_t i = 0; i < n; ++i) {
                window[writePos] = tmp[i];
                writePos = (writePos + 1) % kFftSize;
            }
    }

    void update(float dt, bool active) {
        for (int i = 0; i < kFftSize; ++i) {
            const float hann = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / (kFftSize - 1)));
            fft[i] = window[(writePos + i) % kFftSize] * hann;
        }
        Fft(fft);

        // log-spaced bins from ~86 hz to ~16 khz
        for (int b = 0; b < kBars; ++b) {
            const int lo = static_cast<int>(2.0f * std::pow(186.0f, static_cast<float>(b) / kBars));
            const int hi = std::max(lo + 1, static_cast<int>(2.0f * std::pow(186.0f, static_cast<float>(b + 1) / kBars)));
            float mag = 0.0f;
            for (int k = lo; k < hi && k < kFftSize / 2; ++k) mag = std::max(mag, std::abs(fft[k]));
            const float db = 20.0f * std::log10(mag + 1e-6f);
            const float target = active ? std::clamp((db - 5.0f) / 40.0f, 0.0f, 1.0f) : 0.0f;
            // fast attack, slow fall
            if (target > bars[b]) bars[b] += (target - bars[b]) * std::min(1.0f, dt * 25.0f);
            else bars[b] = std::max(target, bars[b] - dt * 1.8f);
        }
    }
};

// ---------- the app ----------

class App {
public:
    void init() {
        engine_.start();
        engine_.setVolume(volume_);

        lib_.scan(kMusicDir);
        lib_.loadLoved(kLovedFile);

        for (const auto& t : lib_.tracks) {
            Texture2D tex{};
            Color acc = AccentFromString(t.title);
            if (!t.coverPath.empty()) {
                Image img = LoadImage(t.coverPath.c_str());
                if (img.data) {
                    acc = AccentFromImage(img);
                    ImageResize(&img, 320, 320);
                    tex = LoadTextureFromImage(img);
                    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
                    UnloadImage(img);
                }
            }
            covers_.push_back(tex);
            accents_.push_back(acc);
        }

        if (!lib_.tracks.empty()) {
            current_ = 0;
            accent_ = accents_[0];
            engine_.play(lib_.tracks[0].path);  // loaded but paused until space
        }
    }

    void shutdown() {
        engine_.stop();
        for (auto& t : covers_)
            if (t.id != 0) UnloadTexture(t);
    }

    void update(float dt) {
        time_ += dt;
        spectrum_.feed(engine_);
        spectrum_.update(dt, !engine_.paused());
        if (current_ < 0) return;

        accent_ = Mix(accent_, accents_[current_], dt * 3.0f);
        toast_ = std::max(0.0f, toast_ - dt);

        // keyboard, think steering wheel buttons
        if (IsKeyPressed(KEY_SPACE)) engine_.setPaused(!engine_.paused());
        if (IsKeyPressed(KEY_RIGHT)) engine_.seek(engine_.position() + 5.0);
        if (IsKeyPressed(KEY_LEFT)) engine_.seek(engine_.position() - 5.0);
        if (IsKeyPressed(KEY_N)) next();
        if (IsKeyPressed(KEY_P)) prev();
        if (IsKeyPressed(KEY_L)) toggleLove(current_);
        if (IsKeyPressed(KEY_D)) debug_ = !debug_;
        if (IsKeyPressed(KEY_UP)) changeVolume(0.1f);
        if (IsKeyPressed(KEY_DOWN)) changeVolume(-0.1f);

        // end of track: loved songs get played twice
        if (!engine_.loading()) {
            if (engine_.finished()) {
                failStreak_ = 0;
                if (lib_.tracks[current_].loved && !encore_) playIndex(current_, true);
                else next();
            } else if (engine_.loadFailed() && failStreak_ < static_cast<int>(lib_.tracks.size())) {
                ++failStreak_;
                next();
            }
        }
    }

    void draw() {
        ClearBackground(kBg);
        DrawRectangleGradientV(0, 0, kWidth, kHeight, Mix(kBg, accent_, 0.35f), kBg);

        if (current_ < 0) {
            drawEmpty();
            return;
        }
        drawNowPlaying();
        drawProgress();
        drawSpectrum();
        drawQueue();
        drawHints();
        if (toast_ > 0.0f) drawToast();
        if (debug_) drawDebug();
    }

private:
    void playIndex(int i, bool encore = false) {
        const int n = static_cast<int>(lib_.tracks.size());
        if (n == 0) return;
        current_ = ((i % n) + n) % n;
        encore_ = encore;
        engine_.play(lib_.tracks[current_].path);
        engine_.setPaused(false);
    }

    void next() { playIndex(current_ + 1); }

    void prev() {
        if (engine_.position() > 3.0) engine_.seek(0.0);  // like every car: restart first
        else playIndex(current_ - 1);
    }

    void toggleLove(int i) {
        lib_.tracks[i].loved = !lib_.tracks[i].loved;
        lib_.saveLoved(kLovedFile);
    }

    void changeVolume(float d) {
        volume_ = std::clamp(volume_ + d, 0.0f, 1.0f);
        engine_.setVolume(volume_);
        toast_ = 1.2f;
    }

    void drawEmpty() {
        const char* a = "No songs yet";
        const char* b = "Put files named \"Artist - Title.mp3\" in the music/ folder, then restart.";
        Text(a, (kWidth - TextWidth(a, 40)) / 2, 300, 40, kText);
        Text(b, (kWidth - TextWidth(b, 20)) / 2, 360, 20, kDim);
    }

    void drawNowPlaying() {
        const Track& t = lib_.tracks[current_];
        const Rectangle art = {80, 90, 320, 320};

        // glow + shadow behind the art
        DrawCircleGradient(240, 250, 330, Alpha(accent_, 70), Alpha(accent_, 0));
        DrawRectangleRounded({art.x + 6, art.y + 12, art.width, art.height}, 0.05f, 8, {0, 0, 0, 110});

        const Texture2D& tex = covers_[current_];
        if (tex.id != 0) {
            DrawTexturePro(tex, {0, 0, (float)tex.width, (float)tex.height}, art, {0, 0}, 0, WHITE);
        } else {
            DrawRectangleRounded(art, 0.05f, 8, Mix(accent_, kBg, 0.35f));
            const std::string initial = t.title.empty() ? "?" : t.title.substr(0, 1);
            Text(initial, art.x + (art.width - TextWidth(initial, 160)) / 2, art.y + 70, 160, Alpha(kText, 200));
        }

        const float x = 440;
        Text(Fit(t.title, 44, 300), x, 120, 44, kText);
        Text(Fit(t.artist, 26, 300), x, 172, 26, kDim);

        if (encore_) {
            const float pulse = 0.75f + 0.25f * std::sin(time_ * 4.0f);
            const char* label = "ONCE AGAIN";
            const float w = TextWidth(label, 16) + 28;
            DrawRectangleRounded({x, 216, w, 30}, 1.0f, 12, Alpha(accent_, static_cast<unsigned char>(255 * pulse)));
            Text(label, x + 14, 223, 16, kBg);
        } else if (t.loved) {
            Text("loved, plays twice", x, 222, 18, Alpha(accent_, 220));
        }

        // transport controls
        const float cy = 320;
        const Vector2 prevC = {480, cy}, playC = {565, cy}, nextC = {650, cy}, heartC = {725, cy + 2};

        Hit hp = CircleHit(prevC, 26);
        DrawSkipIcon(prevC, 30, hp.hover ? kText : Alpha(kText, 190), false);
        if (hp.clicked) prev();

        Hit hy = CircleHit(playC, 38);
        DrawCircleV(playC, hy.hover ? 40 : 38, kText);
        if (engine_.paused()) DrawPlayIcon(playC, 30, kBg);
        else DrawPauseIcon(playC, 28, kBg);
        if (hy.clicked) engine_.setPaused(!engine_.paused());

        Hit hn = CircleHit(nextC, 26);
        DrawSkipIcon(nextC, 30, hn.hover ? kText : Alpha(kText, 190), true);
        if (hn.clicked) next();

        Hit hh = CircleHit(heartC, 22);
        const Color heartCol = t.loved ? accent_ : Alpha(kText, hh.hover ? 120 : 60);
        DrawHeart(heartC, hh.hover ? 34 : 30, heartCol);
        if (hh.clicked) toggleLove(current_);
    }

    void drawProgress() {
        const Rectangle bar = {80, 466, 660, 8};
        const Rectangle hitArea = {bar.x, bar.y - 12, bar.width, bar.height + 24};
        const double dur = engine_.duration();
        const double pos = engine_.loading() ? 0.0 : std::min(engine_.position(), dur);
        float frac = dur > 0 ? static_cast<float>(pos / dur) : 0.0f;

        const Hit h = RectHit(hitArea);
        const float mouseFrac = std::clamp((GetMousePosition().x - bar.x) / bar.width, 0.0f, 1.0f);
        if (h.clicked) dragging_ = true;
        if (dragging_) {
            frac = mouseFrac;
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                engine_.seek(mouseFrac * dur);
                dragging_ = false;
            }
        }

        DrawRectangleRounded(bar, 1.0f, 8, Alpha(kText, 40));
        DrawRectangleRounded({bar.x, bar.y, std::max(bar.height, bar.width * frac), bar.height}, 1.0f, 8, accent_);
        const bool big = h.hover || dragging_;
        DrawCircleV({bar.x + bar.width * frac, bar.y + bar.height / 2}, big ? 10.0f : 7.0f, kText);

        const std::string left = FormatTime(dragging_ ? frac * dur : pos);
        const std::string right = FormatTime(dur);
        Text(left, bar.x, 488, 18, kDim);
        Text(right, bar.x + bar.width - TextWidth(right, 18), 488, 18, kDim);
    }

    void drawSpectrum() {
        const float x0 = 80, base = 680, maxH = 140, total = 660;
        const float slot = total / kBars;
        const float w = slot - 4;
        for (int b = 0; b < kBars; ++b) {
            const float h = std::max(4.0f, spectrum_.bars[b] * maxH);
            const Color c = Mix(accent_, kText, spectrum_.bars[b] * 0.35f);
            DrawRectangleRounded({x0 + b * slot, base - h, w, h}, 1.0f, 6, Alpha(c, 225));
        }
    }

    void drawQueue() {
        const Rectangle panel = {800, 60, 420, 600};
        DrawRectangleRounded(panel, 0.04f, 12, {255, 255, 255, 12});
        Text("Up next", panel.x + 24, panel.y + 20, 24, kText);
        const std::string count = std::to_string(lib_.tracks.size()) + " songs";
        Text(count, panel.x + panel.width - 24 - TextWidth(count, 16), panel.y + 26, 16, kDim);

        const float rowH = 56;
        const Rectangle list = {panel.x, panel.y + 64, panel.width, panel.height - 76};
        const float contentH = rowH * lib_.tracks.size();
        if (CheckCollisionPointRec(GetMousePosition(), list))
            scroll_ -= GetMouseWheelMove() * 40.0f;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH - list.height));

        BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
        for (int i = 0; i < static_cast<int>(lib_.tracks.size()); ++i) {
            const Track& t = lib_.tracks[i];
            const Rectangle row = {panel.x + 10, list.y + i * rowH - scroll_, panel.width - 20, rowH - 4};
            if (row.y + row.height < list.y || row.y > list.y + list.height) continue;

            const bool inList = CheckCollisionPointRec(GetMousePosition(), list);
            const Hit h = RectHit(row);
            if (i == current_) DrawRectangleRounded(row, 0.3f, 8, Alpha(accent_, 55));
            else if (h.hover && inList) DrawRectangleRounded(row, 0.3f, 8, {255, 255, 255, 14});
            if (h.clicked && inList) playIndex(i);

            const Rectangle thumb = {row.x + 8, row.y + 6, 40, 40};
            if (covers_[i].id != 0)
                DrawTexturePro(covers_[i], {0, 0, (float)covers_[i].width, (float)covers_[i].height}, thumb, {0, 0}, 0, WHITE);
            else
                DrawRectangleRounded(thumb, 0.2f, 6, Mix(accents_[i], kBg, 0.4f));

            Text(Fit(t.title, 19, 270), row.x + 60, row.y + 7, 19, i == current_ ? kText : Alpha(kText, 220));
            Text(Fit(t.artist, 15, 270), row.x + 60, row.y + 30, 15, kDim);

            if (i == current_ && !engine_.paused()) DrawEqIcon({row.x + row.width - 58, row.y + 36}, time_, accent_);
            if (t.loved) DrawHeart({row.x + row.width - 18, row.y + 26}, 16, accents_[i]);
        }
        EndScissorMode();
    }

    void drawHints() {
        const char* hints = "space play   left/right seek   N/P skip   L love   up/down volume   D debug";
        Text(hints, 800, 676, 14, Alpha(kDim, 180));
    }

    void drawToast() {
        const std::string s = "Volume " + std::to_string(static_cast<int>(std::lround(volume_ * 100))) + "%";
        const float w = TextWidth(s, 18) + 32;
        const unsigned char a = static_cast<unsigned char>(255 * std::min(1.0f, toast_ * 3.0f));
        DrawRectangleRounded({(80 + 740 - w) / 2, 30, w, 36}, 1.0f, 12, Alpha({0, 0, 0, 255}, a / 2));
        Text(s, (80 + 740 - w) / 2 + 16, 39, 18, Alpha(kText, a));
    }

    void drawDebug() {
        char buf[160];
        std::snprintf(buf, sizeof buf, "ring %3.0f%%   underruns %d   fps %d   %s",
                      engine_.bufferFill() * 100.0f, engine_.underruns(), GetFPS(),
                      engine_.loading() ? "loading" : "ready");
        DrawRectangleRounded({16, 12, 420, 26}, 0.5f, 8, {0, 0, 0, 150});
        Text(buf, 26, 17, 16, {120, 255, 160, 255});
        DrawRectangle(16, 40, static_cast<int>(420 * engine_.bufferFill()), 3, {120, 255, 160, 255});
    }

    AudioEngine engine_;
    Library lib_;
    std::vector<Texture2D> covers_;
    std::vector<Color> accents_;
    Spectrum spectrum_;

    int current_ = -1;
    bool encore_ = false;
    int failStreak_ = 0;
    Color accent_ = {255, 120, 170, 255};
    float volume_ = 0.8f;
    float toast_ = 0.0f;
    float scroll_ = 0.0f;
    float time_ = 0.0f;
    bool dragging_ = false;
    bool debug_ = false;
};

}  // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(kWidth, kHeight, "onceagain");
    SetTargetFPS(60);
    InitAudioDevice();

    const bool customFont = FileExists(kFontFile);
    if (customFont) {
        gFont = LoadFontEx(kFontFile, 96, nullptr, 0);
        GenTextureMipmaps(&gFont.texture);
        SetTextureFilter(gFont.texture, TEXTURE_FILTER_TRILINEAR);
    } else {
        gFont = GetFontDefault();
    }

    App app;
    app.init();

    while (!WindowShouldClose()) {
        app.update(GetFrameTime());
        BeginDrawing();
        app.draw();
        EndDrawing();
    }

    app.shutdown();
    if (customFont) UnloadFont(gFont);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
