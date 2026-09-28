#include "audio_engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>

using namespace std::chrono_literals;

std::atomic<AudioEngine*> AudioEngine::instance_{nullptr};

AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::start() {
    if (started_) return;
    instance_.store(this, std::memory_order_release);
    stream_ = LoadAudioStream(kSampleRate, 32, kChannels);  // 32 bit = float samples
    SetAudioStreamCallback(stream_, &AudioEngine::audioCallback);
    PlayAudioStream(stream_);  // stream always runs, pause = output silence
    running_ = true;
    streamer_ = std::thread(&AudioEngine::streamerLoop, this);
    started_ = true;
}

void AudioEngine::stop() {
    if (!started_) return;
    running_ = false;
    if (streamer_.joinable()) streamer_.join();
    StopAudioStream(stream_);
    UnloadAudioStream(stream_);
    instance_.store(nullptr, std::memory_order_release);
    started_ = false;
}

void AudioEngine::play(const std::string& path) {
    std::lock_guard lock(cmdMutex_);
    pendingPath_ = path;
    hasPendingPath_ = true;
    pendingSeek_ = -1;
    requested_.fetch_add(1);
}

void AudioEngine::seek(double seconds) {
    std::lock_guard lock(cmdMutex_);
    pendingSeek_ = static_cast<int64_t>(std::max(0.0, seconds) * kSampleRate);
    requested_.fetch_add(1);
}

void AudioEngine::setVolume(float v) { SetAudioStreamVolume(stream_, v); }

double AudioEngine::position() const {
    return static_cast<double>(framesPlayed_.load(std::memory_order_relaxed)) / kSampleRate;
}

double AudioEngine::duration() const {
    return static_cast<double>(totalFrames_.load(std::memory_order_relaxed)) / kSampleRate;
}

bool AudioEngine::loading() const { return requested_.load() != handled_.load(); }

bool AudioEngine::finished() const {
    return !loading() && totalFrames_.load() > 0 && eof_.load() && ring_.size() == 0;
}

bool AudioEngine::loadFailed() const {
    return !loading() && totalFrames_.load() == 0 && requested_.load() > 0;
}

float AudioEngine::bufferFill() const {
    return static_cast<float>(ring_.size()) / static_cast<float>(ring_.capacity());
}

// producer can't clear the ring itself (only the consumer may move tail),
// so it asks the audio thread to drain it and waits.
void AudioEngine::flushRing() {
    flushRequested_.store(true, std::memory_order_release);
    while (flushRequested_.load(std::memory_order_acquire) && running_.load())
        std::this_thread::sleep_for(1ms);
}

void AudioEngine::streamerLoop() {
    Wave wave{};
    uint64_t cursor = 0;  // next frame to push

    while (running_.load()) {
        std::string path;
        bool newTrack = false;
        int64_t seekTo = -1;
        uint64_t req = 0;
        {
            std::lock_guard lock(cmdMutex_);
            req = requested_.load();
            if (hasPendingPath_) {
                path = std::move(pendingPath_);
                hasPendingPath_ = false;
                newTrack = true;
            }
            seekTo = pendingSeek_;
            pendingSeek_ = -1;
        }

        if (newTrack || seekTo >= 0) {
            flushRing();
            eof_.store(true);
        }
        if (newTrack) {
            if (wave.data) UnloadWave(wave);
            wave = LoadWave(path.c_str());  // decode happens here, off the ui thread
            if (wave.data) WaveFormat(&wave, kSampleRate, 32, kChannels);
            cursor = 0;
            totalFrames_.store(wave.data ? wave.frameCount : 0);
            framesPlayed_.store(0);
        }
        if (seekTo >= 0 && wave.data) {
            cursor = std::min<uint64_t>(static_cast<uint64_t>(seekTo), wave.frameCount);
            framesPlayed_.store(cursor);
        }
        if (newTrack || seekTo >= 0) eof_.store(!wave.data || cursor >= wave.frameCount);
        handled_.store(req);

        if (wave.data && cursor < wave.frameCount) {
            const float* samples = static_cast<const float*>(wave.data);
            const size_t frames = std::min<uint64_t>(4096, wave.frameCount - cursor);
            const size_t pushed = ring_.push(samples + cursor * kChannels, frames * kChannels);
            cursor += pushed / kChannels;
            if (cursor >= wave.frameCount) eof_.store(true);
            if (pushed == 0) std::this_thread::sleep_for(2ms);  // ring full, let audio catch up
        } else {
            std::this_thread::sleep_for(5ms);
        }
    }
    if (wave.data) UnloadWave(wave);
}

// runs on the audio device thread. no locks, no allocation.
void AudioEngine::audioCallback(void* buffer, unsigned int frames) {
    float* out = static_cast<float*>(buffer);
    const size_t n = static_cast<size_t>(frames) * kChannels;
    AudioEngine* self = instance_.load(std::memory_order_acquire);
    if (!self) {
        std::memset(out, 0, n * sizeof(float));
        return;
    }

    if (self->flushRequested_.load(std::memory_order_acquire)) {
        float scratch[512];
        while (self->ring_.pop(scratch, 512) > 0) {}
        self->flushRequested_.store(false, std::memory_order_release);
        std::memset(out, 0, n * sizeof(float));
        return;
    }

    if (self->paused_.load(std::memory_order_relaxed)) {
        std::memset(out, 0, n * sizeof(float));
        return;
    }

    const size_t got = self->ring_.pop(out, n);
    if (got < n) {
        std::memset(out + got, 0, (n - got) * sizeof(float));
        if (!self->eof_.load(std::memory_order_relaxed) && !self->loading())
            self->underruns_.fetch_add(1, std::memory_order_relaxed);
    }

    const size_t framesGot = got / kChannels;
    self->framesPlayed_.fetch_add(framesGot, std::memory_order_relaxed);

    float mono[256];
    for (size_t i = 0; i < framesGot; i += 256) {
        const size_t m = std::min<size_t>(256, framesGot - i);
        for (size_t k = 0; k < m; ++k)
            mono[k] = 0.5f * (out[(i + k) * 2] + out[(i + k) * 2 + 1]);
        self->vis_.push(mono, m);  // if the ui is behind, we just drop some
    }
}
