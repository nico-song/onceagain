#pragma once
#include "raylib.h"
#include "spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// three threads:
//   main (ui)     -> sends commands (play / seek / pause) under a mutex
//   streamer      -> decodes the file and pushes samples into ring_
//   audio device  -> raylib callback pops samples from ring_ (never locks)
class AudioEngine {
public:
    static constexpr int kSampleRate = 44100;
    static constexpr int kChannels = 2;

    AudioEngine() = default;
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void start();  // call after InitAudioDevice()
    void stop();   // call before CloseAudioDevice()

    void play(const std::string& path);
    void seek(double seconds);
    void setPaused(bool p) { paused_.store(p, std::memory_order_relaxed); }
    bool paused() const { return paused_.load(std::memory_order_relaxed); }
    void setVolume(float v);

    double position() const;
    double duration() const;
    bool loading() const;     // a command hasn't been handled yet
    bool finished() const;    // track fully played out
    bool loadFailed() const;  // file couldn't be decoded
    float bufferFill() const;
    int underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // mono samples the audio thread just played, for the visualizer
    size_t readVisualizer(float* out, size_t max) { return vis_.pop(out, max); }

private:
    void streamerLoop();
    void flushRing();
    static void audioCallback(void* buffer, unsigned int frames);
    static std::atomic<AudioEngine*> instance_;  // raylib callbacks have no user pointer

    AudioStream stream_{};
    bool started_ = false;
    std::thread streamer_;
    std::atomic<bool> running_{false};

    SpscRing<float> ring_{1u << 16};  // ~0.74s of stereo audio
    SpscRing<float> vis_{1u << 13};

    std::atomic<bool> paused_{true};
    std::atomic<bool> flushRequested_{false};
    std::atomic<bool> eof_{true};
    std::atomic<uint64_t> framesPlayed_{0};
    std::atomic<uint64_t> totalFrames_{0};
    std::atomic<int> underruns_{0};

    std::mutex cmdMutex_;
    std::string pendingPath_;
    bool hasPendingPath_ = false;
    int64_t pendingSeek_ = -1;
    std::atomic<uint64_t> requested_{0};
    std::atomic<uint64_t> handled_{0};
};
