# onceagain

An in car style music player written in C++20 with raylib :) made to play TWICE songs. (but i guess TECHNICALLY you could play any song on it...)
Songs you love get played TWICE :), because once is never enough!

![screenshot](docs/screenshot.png)

## Features

- Now Playing screen with album art, theme color pulled from the cover art (avg color), live spectrum visualizer
- Love a song and it plays one more time when it ends, w/ "ONCE AGAIN" indicator (ONCE is the TWICE fan name)
- Queue panel with thumbnails, click to play, scroll wheel etc.
- Keyboard controls that map to steering wheel style buttons
- Debug overlay showing buffer fill and audio underruns

## How it works

Three threads:

```
main (ui) --commands--> streamer thread --lock-free ring buffer--> audio thread
                                                                        |
main (ui) <---------- visualizer ring buffer (mono samples) ------------+
```

- **UI thread** renders at 60 fps and sends play / seek commands under a mutex
- **Streamer thread** decodes the file and fills a lock-free SPSC ring buffer
- **Audio thread** (raylib callback) pulls samples from the ring. It never locks or allocates, so a slow UI frame can't cause audio glitches
- Seeking or switching songs asks the audio thread to drain the ring, since only the consumer is allowed to move the read index
- The ring buffer has a 5 million item 2 thread stress test (`tests/ring_test.cpp`), clean under ThreadSanitizer

## Build

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
./build/onceagain
```

Run it from the repo root so it finds `music/` and `assets/`.

## Adding music

Put your own audio files in `music/` named `Artist - Title.mp3` (mp3, wav, ogg).
For cover art, add a PNG with the same name: `Artist - Title.png`. (you can also rename .jpg to .png and it works)
The music folder is gitignored so no songs end up in the repo. (aka no TWICE copyright!)

## Controls

| Key | Action |
| --- | --- |
| Space | play / pause |
| Left / Right | seek 5s |
| N / P | next / previous |
| L | love (plays twice) |
| Up / Down | volume |
| D | debug overlay |
