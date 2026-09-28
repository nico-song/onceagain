#include "raylib.h"

#include "audio_engine.h"
#include "fft.h"
#include "library.h"

#include <algorithm>
#include <array>
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
constexpr int kFftSize = 1024;

// visualizer tuning, tweak these
constexpr int kBars = 32;                  // bars per side (mirrored, so 64 on screen)
constexpr float kAttack = 30.0f;           // how fast bars jump up (higher = snappier)
constexpr float kRelease = 10.0f;          // how fast bars fall (higher = snappier)
constexpr float kBeatSensitivity = 1.35f;  // bass has to be this much above average to count as a beat
constexpr float kBeatPulse = 0.035f;       // how much the album art grows on a beat

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

// heart built from one curve instead of circles + triangle, so there's no
// darker spot where shapes overlap. points[0] is the center for a triangle fan.
constexpr int kHeartSegments = 48;
std::array<Vector2, kHeartSegments + 2> HeartPoints(Vector2 c, float s) {
    std::array<Vector2, kHeartSegments + 2> pts{};
    const float k = s / 32.0f;  // the curve is 32 units wide
    auto map = [&](float x, float y) { return Vector2{c.x + x * k, c.y - (y + 2.5f) * k}; };
    pts[0] = map(0, 0);
    for (int i = 0; i <= kHeartSegments; ++i) {
        const float t = 2.0f * std::numbers::pi_v<float> * i / kHeartSegments;
        const float x = -16.0f * std::pow(std::sin(t), 3.0f);
        const float y = 13.0f * std::cos(t) - 5.0f * std::cos(2 * t) - 2.0f * std::cos(3 * t) - std::cos(4 * t);
        pts[i + 1] = map(x, y);
    }
    return pts;
}

void DrawHeart(Vector2 c, float s, Color col) {
    auto pts = HeartPoints(c, s);
    DrawTriangleFan(pts.data(), static_cast<int>(pts.size()), col);
}

void DrawHeartOutline(Vector2 c, float s, float thick, Color col) {
    auto pts = HeartPoints(c, s);
    DrawSplineLinear(pts.data() + 1, static_cast<int>(pts.size()) - 1, thick, col);
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
    float peak = 0.5f;          // loudest recent bar, used for auto gain
    float bassAvg = 0.0f;       // running average of bass energy
    float beat = 0.0f;          // jumps to 1 on a beat, fades to 0
    float beatCooldown = 0.0f;

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

        // 1. raw bar heights: average of log-spaced bins, ~86 hz to ~11 khz
        float raw[kBars];
        float loudest = 0.0f;
        for (int b = 0; b < kBars; ++b) {
            const int lo = static_cast<int>(2.0f * std::pow(128.0f, static_cast<float>(b) / kBars));
            const int hi = std::max(lo + 1, static_cast<int>(2.0f * std::pow(128.0f, static_cast<float>(b + 1) / kBars)));
            float sum = 0.0f;
            for (int k = lo; k < hi; ++k) sum += std::abs(fft[k]);
            float mag = sum / (hi - lo);
            mag *= 1.0f + 2.0f * b / kBars;  // highs are naturally quieter, tilt them up
            raw[b] = std::sqrt(mag);         // compress so loud and quiet parts both show
            loudest = std::max(loudest, raw[b]);
        }

        // 2. auto gain: scale to the loudest recent bar so quiet and loud songs both fill the space
        peak = std::max({loudest, peak * std::exp(-dt * 0.5f), 0.5f});

        // 3. blend neighbors so it reads as a shape instead of noise, then smooth over time
        for (int b = 0; b < kBars; ++b) {
            const float l = raw[std::max(0, b - 1)];
            const float r = raw[std::min(kBars - 1, b + 1)];
            const float smooth = 0.5f * raw[b] + 0.25f * (l + r);
            const float target = active ? std::clamp(smooth / peak, 0.0f, 1.0f) : 0.0f;
            const float rate = target > bars[b] ? kAttack : kRelease;
            bars[b] += (target - bars[b]) * (1.0f - std::exp(-rate * dt));
        }

        // 4. beat detection: bass energy (~43-215 hz) spiking above its recent average
        float bass = 0.0f;
        for (int k = 1; k <= 5; ++k) bass += std::norm(fft[k]);
        beatCooldown -= dt;
        if (active && beatCooldown <= 0.0f && bass > bassAvg * kBeatSensitivity && bassAvg > 1e-4f) {
            beat = 1.0f;
            beatCooldown = 0.18f;  // no more than ~5 beats per second
        }
        bassAvg += (bass - bassAvg) * (1.0f - std::exp(-dt * 2.0f));
        beat *= std::exp(-dt * 6.0f);
    }
};

// ---------- layout ----------
// everything is positioned from the window size each frame, so resizing just works.
// u = scale compared to the original 1280x720 design, used for sizes and fonts.

struct Layout {
    float w = 0, h = 0, u = 1;
    bool showQueue = true;
    Rectangle queue{};
    float left = 0, right = 0;
    Rectangle art{};
    float infoX = 0, infoW = 0;
    float titleY = 0, artistY = 0, badgeY = 0, controlsY = 0, volumeY = 0;
    Rectangle progress{};
    float timesY = 0, specTop = 0, specBase = 0, hintY = 0;
};

Layout ComputeLayout(float w, float h) {
    Layout L;
    L.w = w;
    L.h = h;
    L.u = std::clamp(std::min(w / kWidth, h / kHeight), 0.6f, 2.0f);
    const float u = L.u;
    const float m = 56 * u;

    // queue panel on the right, hidden when the window gets narrow
    L.showQueue = w >= 960;
    const float qw = std::clamp(w * 0.32f, 320.0f, 520.0f);
    L.queue = {w - m - qw, m, qw, h - 2 * m - 16 * u};

    L.left = m;
    L.right = L.showQueue ? L.queue.x - m : w - m;
    const float mainW = L.right - L.left;

    // album art is as big as it can be without crowding the rest
    const float a = std::min(mainW * 0.45f, h * 0.44f);
    L.art = {L.left, m + 30 * u, a, a};

    L.infoX = L.art.x + a + 40 * u;
    L.infoW = L.right - L.infoX;
    L.titleY = L.art.y + a * 0.09f;
    L.artistY = L.titleY + 52 * u;
    L.badgeY = L.artistY + 46 * u;
    L.controlsY = L.art.y + a * 0.72f;
    L.volumeY = L.art.y + a * 0.95f;

    L.progress = {L.left, L.art.y + a + 56 * u, mainW, 8 * u};
    L.timesY = L.progress.y + 22 * u;
    L.specTop = L.timesY + 40 * u;
    L.specBase = h - m - 20 * u;
    L.hintY = L.queue.y + L.queue.height + 12 * u;
    return L;
}

// ---------- the app ----------

class App {
public:
    void init() {
        engine_.start();
        engine_.setVolume(volume_);

        lib_.scan(kMusicDir);
        lib_.loadLoved(kLovedFile);

        for (const auto& t : lib_.tracks) {
            Texture2D tex{}, ambient{};
            Color acc = AccentFromString(t.title);
            if (!t.coverPath.empty()) {
                Image img = LoadImage(t.coverPath.c_str());
                if (img.data) {
                    acc = AccentFromImage(img);
                    ImageResize(&img, 640, 640);  // sharp even on big windows
                    tex = LoadTextureFromImage(img);
                    GenTextureMipmaps(&tex);
                    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);

                    // tiny blurred copy, stretched over the whole window = free soft background
                    ImageResize(&img, 24, 24);
                    ImageBlurGaussian(&img, 2);
                    ambient = LoadTextureFromImage(img);
                    SetTextureFilter(ambient, TEXTURE_FILTER_BILINEAR);
                    UnloadImage(img);
                }
            }
            covers_.push_back(tex);
            ambient_.push_back(ambient);
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
        for (auto& t : ambient_)
            if (t.id != 0) UnloadTexture(t);
    }

    void update(float dt) {
        time_ += dt;
        spectrum_.feed(engine_);
        spectrum_.update(dt, !engine_.paused());
        if (current_ < 0) return;

        accent_ = Mix(accent_, accents_[current_], dt * 3.0f);
        ambientFade_ = std::min(1.0f, ambientFade_ + dt * 1.5f);

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
        L_ = ComputeLayout(static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()));
        drawBackground();

        if (current_ < 0) {
            drawEmpty();
            return;
        }
        drawNowPlaying();
        drawProgress();
        drawVolume();
        drawSpectrum();
        if (L_.showQueue) {
            drawQueue();
            drawHints();
        }
        if (debug_) drawDebug();
    }

private:
    void playIndex(int i, bool encore = false) {
        const int n = static_cast<int>(lib_.tracks.size());
        if (n == 0) return;
        const int target = ((i % n) + n) % n;
        if (target != current_) {
            prevTrack_ = current_;
            ambientFade_ = 0.0f;
        }
        current_ = target;
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
    }

    void drawBackground() {
        ClearBackground(kBg);
        const float w = L_.w, h = L_.h;

        // blurred album art behind everything, crossfades when the song changes
        auto drawAmbient = [&](int idx, float alpha) {
            if (idx < 0 || alpha <= 0.0f) return;
            const Texture2D& t = ambient_[idx];
            if (t.id == 0) return;
            const float side = std::max(w, h) * 1.1f;
            DrawTexturePro(t, {0, 0, (float)t.width, (float)t.height},
                           {(w - side) / 2, (h - side) / 2, side, side}, {0, 0}, 0,
                           Alpha(WHITE, static_cast<unsigned char>(255 * alpha * 0.6f)));
        };
        if (ambientFade_ < 1.0f) drawAmbient(prevTrack_, 1.0f - ambientFade_);
        drawAmbient(current_, ambientFade_);

        // dark overlay so text stays readable, accent tint at the top
        DrawRectangleGradientV(0, 0, (int)w, (int)h, Alpha(Mix(kBg, accent_, 0.35f), 150), Alpha(kBg, 235));
    }

    void drawEmpty() {
        const float u = L_.u;
        const char* a = "No songs yet";
        const char* b = "Put files named \"Artist - Title.mp3\" in the music/ folder, then restart.";
        Text(a, (L_.w - TextWidth(a, 40 * u)) / 2, L_.h / 2 - 60 * u, 40 * u, kText);
        Text(b, (L_.w - TextWidth(b, 20 * u)) / 2, L_.h / 2, 20 * u, kDim);
    }

    void drawNowPlaying() {
        const Track& t = lib_.tracks[current_];
        const float u = L_.u;
        const float beat = spectrum_.beat;
        const Rectangle base = L_.art;
        const float grow = base.width * kBeatPulse * beat;
        const Rectangle art = {base.x - grow / 2, base.y - grow / 2, base.width + grow, base.height + grow};
        const Vector2 artCenter = {base.x + base.width / 2, base.y + base.height / 2};

        // glow + shadow behind the art, both kick on the beat
        DrawCircleGradient((int)artCenter.x, (int)artCenter.y, base.width * (1.0f + 0.12f * beat),
                           Alpha(accent_, static_cast<unsigned char>(70 + 80 * beat)), Alpha(accent_, 0));
        DrawRectangleRounded({art.x + 6 * u, art.y + 12 * u, art.width, art.height}, 0.05f, 8, {0, 0, 0, 110});

        const Texture2D& tex = covers_[current_];
        if (tex.id != 0) {
            DrawTexturePro(tex, {0, 0, (float)tex.width, (float)tex.height}, art, {0, 0}, 0, WHITE);
        } else {
            DrawRectangleRounded(art, 0.05f, 8, Mix(accent_, kBg, 0.35f));
            const std::string initial = t.title.empty() ? "?" : t.title.substr(0, 1);
            const float fs = art.width * 0.5f;
            Text(initial, art.x + (art.width - TextWidth(initial, fs)) / 2, art.y + (art.height - fs) / 2, fs, Alpha(kText, 200));
        }

        const float x = L_.infoX;
        Text(Fit(t.title, 44 * u, L_.infoW), x, L_.titleY, 44 * u, kText);
        Text(Fit(t.artist, 26 * u, L_.infoW), x, L_.artistY, 26 * u, kDim);

        if (encore_) {
            const float pulse = 0.75f + 0.25f * std::sin(time_ * 4.0f);
            const char* label = "ONCE AGAIN";
            const float w = TextWidth(label, 16 * u) + 28 * u;
            DrawRectangleRounded({x, L_.badgeY, w, 30 * u}, 1.0f, 12, Alpha(accent_, static_cast<unsigned char>(255 * pulse)));
            Text(label, x + 14 * u, L_.badgeY + 7 * u, 16 * u, kBg);
        } else if (t.loved) {
            Text("loved, plays twice", x, L_.badgeY + 6 * u, 18 * u, Alpha(accent_, 220));
        }

        // transport controls
        const float cy = L_.controlsY;
        const Vector2 prevC = {x + 40 * u, cy}, playC = {x + 125 * u, cy};
        const Vector2 nextC = {x + 210 * u, cy}, heartC = {x + 285 * u, cy + 2 * u};

        Hit hp = CircleHit(prevC, 26 * u);
        DrawSkipIcon(prevC, 30 * u, hp.hover ? kText : Alpha(kText, 190), false);
        if (hp.clicked) prev();

        Hit hy = CircleHit(playC, 38 * u);
        DrawCircleV(playC, (hy.hover ? 40 : 38) * u, kText);
        if (engine_.paused()) DrawPlayIcon(playC, 30 * u, kBg);
        else DrawPauseIcon(playC, 28 * u, kBg);
        if (hy.clicked) engine_.setPaused(!engine_.paused());

        Hit hn = CircleHit(nextC, 26 * u);
        DrawSkipIcon(nextC, 30 * u, hn.hover ? kText : Alpha(kText, 190), true);
        if (hn.clicked) next();

        Hit hh = CircleHit(heartC, 22 * u);
        const float heartSize = (hh.hover ? 34 : 30) * u;
        if (t.loved) DrawHeart(heartC, heartSize, accent_);
        else DrawHeartOutline(heartC, heartSize, 2.5f * u, Mix(kBg, kText, hh.hover ? 0.85f : 0.5f));
        if (hh.clicked) toggleLove(current_);
    }

    void drawProgress() {
        const float u = L_.u;
        const Rectangle bar = L_.progress;
        const Rectangle hitArea = {bar.x, bar.y - 12 * u, bar.width, bar.height + 24 * u};
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
        DrawCircleV({bar.x + bar.width * frac, bar.y + bar.height / 2}, (big ? 10.0f : 7.0f) * u, kText);

        const std::string left = FormatTime(dragging_ ? frac * dur : pos);
        const std::string right = FormatTime(dur);
        Text(left, bar.x, L_.timesY, 18 * u, kDim);
        Text(right, bar.x + bar.width - TextWidth(right, 18 * u), L_.timesY, 18 * u, kDim);
    }

    void drawVolume() {
        const float u = L_.u;
        const float x = L_.infoX, cy = L_.volumeY;
        const float barW = std::min(210 * u, L_.infoW - 90 * u);
        const Rectangle bar = {x + 38 * u, cy - 3 * u, barW, 6 * u};
        const Rectangle hitArea = {x - 10 * u, cy - 14 * u, barW + 90 * u, 28 * u};
        const Color iconCol = Mix(kBg, kText, 0.8f);

        // speaker icon, waves show how loud it is
        DrawRectangleRec({x + 2 * u, cy - 4 * u, 6 * u, 8 * u}, iconCol);
        DrawTriangle({x + 16 * u, cy - 9 * u}, {x + 4 * u, cy}, {x + 16 * u, cy + 9 * u}, iconCol);
        if (volume_ > 0.0f) DrawRing({x + 18 * u, cy}, 4 * u, 6 * u, -50, 50, 12, iconCol);
        if (volume_ > 0.5f) DrawRing({x + 18 * u, cy}, 9 * u, 11 * u, -50, 50, 12, iconCol);

        // click, drag, or scroll to change it
        const Hit h = RectHit(hitArea);
        if (h.clicked && GetMousePosition().x >= bar.x - 8 * u) volDragging_ = true;
        if (volDragging_) {
            const float f = std::clamp((GetMousePosition().x - bar.x) / bar.width, 0.0f, 1.0f);
            changeVolume(f - volume_);
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) volDragging_ = false;
        }
        if (h.hover) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) changeVolume(wheel * 0.05f);
        }

        DrawRectangleRounded(bar, 1.0f, 8, Mix(kBg, kText, 0.2f));
        DrawRectangleRounded({bar.x, bar.y, std::max(bar.height, bar.width * volume_), bar.height}, 1.0f, 8, Mix(kBg, kText, 0.85f));
        DrawCircleV({bar.x + bar.width * volume_, cy}, ((h.hover || volDragging_) ? 8.0f : 6.0f) * u, kText);

        const std::string pct = std::to_string(static_cast<int>(std::lround(volume_ * 100))) + "%";
        Text(pct, bar.x + bar.width + 16 * u, cy - 9 * u, 18 * u, kDim);
    }

    void drawSpectrum() {
        // mirrored from the center: bass in the middle, highs toward the edges
        const float x0 = L_.left, total = L_.right - L_.left;
        const float base = L_.specBase, maxH = std::max(20.0f, L_.specBase - L_.specTop);
        const float cx = x0 + total / 2;
        const float slot = total / (kBars * 2);
        const float w = slot * 0.6f;
        for (int b = 0; b < kBars; ++b) {
            const float v = spectrum_.bars[b];
            const float h = std::max(3.0f, v * maxH);
            const Color c = Mix(accent_, kText, v * 0.4f + spectrum_.beat * 0.25f);
            const float off = (b + 0.5f) * slot;
            for (float x : {cx + off - w / 2, cx - off - w / 2}) {
                DrawRectangleRounded({x, base - h, w, h}, 1.0f, 6, c);
                DrawRectangleRounded({x, base + 4 * L_.u, w, h * 0.3f}, 1.0f, 6, Alpha(c, 45));  // reflection
            }
        }
    }

    void drawQueue() {
        const float u = L_.u;
        const Rectangle panel = L_.queue;
        DrawRectangleRounded(panel, 0.04f, 12, {255, 255, 255, 12});
        Text("Up next", panel.x + 24 * u, panel.y + 20 * u, 24 * u, kText);
        const std::string count = std::to_string(lib_.tracks.size()) + " songs";
        Text(count, panel.x + panel.width - 24 * u - TextWidth(count, 16 * u), panel.y + 26 * u, 16 * u, kDim);

        const float rowH = 56 * u;
        const Rectangle list = {panel.x, panel.y + 64 * u, panel.width, panel.height - 76 * u};
        const float contentH = rowH * lib_.tracks.size();
        const bool inList = CheckCollisionPointRec(GetMousePosition(), list);
        if (inList) scroll_ -= GetMouseWheelMove() * 40.0f * u;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH - list.height));

        BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
        for (int i = 0; i < static_cast<int>(lib_.tracks.size()); ++i) {
            const Track& t = lib_.tracks[i];
            const Rectangle row = {panel.x + 10 * u, list.y + i * rowH - scroll_, panel.width - 20 * u, rowH - 4 * u};
            if (row.y + row.height < list.y || row.y > list.y + list.height) continue;

            const Hit h = RectHit(row);
            if (i == current_) DrawRectangleRounded(row, 0.3f, 8, Alpha(accent_, 55));
            else if (h.hover && inList) DrawRectangleRounded(row, 0.3f, 8, {255, 255, 255, 14});
            if (h.clicked && inList) playIndex(i);

            const Rectangle thumb = {row.x + 8 * u, row.y + 6 * u, 40 * u, 40 * u};
            if (covers_[i].id != 0)
                DrawTexturePro(covers_[i], {0, 0, (float)covers_[i].width, (float)covers_[i].height}, thumb, {0, 0}, 0, WHITE);
            else
                DrawRectangleRounded(thumb, 0.2f, 6, Mix(accents_[i], kBg, 0.4f));

            const float textW = row.width - 150 * u;
            Text(Fit(t.title, 19 * u, textW), row.x + 60 * u, row.y + 7 * u, 19 * u, i == current_ ? kText : Alpha(kText, 220));
            Text(Fit(t.artist, 15 * u, textW), row.x + 60 * u, row.y + 30 * u, 15 * u, kDim);

            if (i == current_ && !engine_.paused()) DrawEqIcon({row.x + row.width - 58 * u, row.y + 36 * u}, time_, accent_);
            if (t.loved) DrawHeart({row.x + row.width - 18 * u, row.y + 26 * u}, 16 * u, accents_[i]);
        }
        EndScissorMode();
    }

    void drawHints() {
        const char* hints = "space play   left/right seek   N/P skip   L love   up/down volume   D debug";
        const float fs = 14 * L_.u;
        Text(Fit(hints, fs, L_.queue.width), L_.queue.x, L_.hintY, fs, Alpha(kDim, 180));
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
    std::vector<Texture2D> ambient_;
    std::vector<Color> accents_;
    Spectrum spectrum_;

    Layout L_;
    int current_ = -1;
    int prevTrack_ = -1;
    float ambientFade_ = 1.0f;
    bool encore_ = false;
    int failStreak_ = 0;
    Color accent_ = {255, 120, 170, 255};
    float volume_ = 0.8f;
    bool volDragging_ = false;
    float scroll_ = 0.0f;
    float time_ = 0.0f;
    bool dragging_ = false;
    bool debug_ = false;
};

}  // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWidth, kHeight, "onceagain");
    SetWindowMinSize(800, 500);
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
