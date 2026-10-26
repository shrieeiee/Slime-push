// Tiny synthesized sound effects (no asset files).
#pragma once
#include <SDL.h>
#include <vector>
#include "music.hpp"

enum Sfx {
    S_MOVE, S_ROTATE, S_LAND, S_LOCK, S_HOLD, S_CLEAR1, S_CLEAR2, S_CLEAR3, S_CLEAR4,
    S_PUSH, S_PUSHED, S_VANISH, S_TICK, S_GO, S_WIN, S_LOSE, S_SELECT, S_BACK, S_JOIN, S_COUNT
};

class Audio {
public:
    bool enabled = true;
    bool init();
    void shutdown();
    void play(Sfx s, float vol = 1.0f, float pan = 0.0f);
    // music: crossfades to the track (no-op if it is already playing); nullptr fades out
    bool musicOn = true;
    float musicVolume = 0.5f;
    void playMusic(TrackPtr t);
    void setMusicRate(float r);     // speeds the battle tune up with the level
    TrackPtr currentMusic() const { return wanted; }
private:
    struct MusicState { TrackPtr cur, next; double pos = 0; float gain = 0, target = 0, rate = 1; bool switching = false; };
    MusicState ms;
    TrackPtr wanted;
    SDL_AudioDeviceID dev = 0;
    std::vector<float> clips[S_COUNT];
    struct Voice { int clip = -1; size_t pos = 0; float vl = 0, vr = 0; };
    Voice voices[16];
    static void cb(void* ud, Uint8* stream, int len);
    void build();
};
