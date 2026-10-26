#include "audio.hpp"
#include <cmath>
#include <cstring>
#include <random>

static const int RATE = 44100;
static const float PI2 = 6.2831853f;

// gooey blip: sine with pitch sweep, optional wobble and a soft square edge
static void tone(std::vector<float>& out, float start, float f0, float f1, float dur, float vol,
                 float wobbleHz = 0, float wobbleAmt = 0, float squareMix = 0, float attack = 0.004f) {
    size_t at = (size_t)(start * RATE), n = (size_t)(dur * RATE);
    if (out.size() < at + n) out.resize(at + n, 0.0f);
    float ph = 0;
    for (size_t i = 0; i < n; ++i) {
        float t = (float)i / RATE, u = (float)i / n;
        float f = f0 * std::pow(f1 / f0, u);
        if (wobbleHz > 0) f *= 1.0f + wobbleAmt * std::sin(PI2 * wobbleHz * t);
        ph += PI2 * f / RATE;
        float s = std::sin(ph);
        if (squareMix > 0) s = s * (1 - squareMix) + (s > 0 ? 1.f : -1.f) * squareMix * 0.5f;
        float env = std::fmin(1.0f, t / attack) * std::pow(1.0f - u, 1.6f);
        out[at + i] += s * env * vol;
    }
}

void Audio::build() {
    tone(clips[S_MOVE], 0, 900, 800, 0.03f, 0.18f, 0, 0, 0.3f);
    tone(clips[S_ROTATE], 0, 700, 1200, 0.05f, 0.2f);
    tone(clips[S_LAND], 0, 260, 180, 0.06f, 0.22f);
    tone(clips[S_LOCK], 0, 420, 140, 0.12f, 0.35f, 30, 0.08f);       // squelchy bloop
    tone(clips[S_HOLD], 0, 500, 900, 0.08f, 0.25f);
    tone(clips[S_HOLD], 0.06f, 900, 600, 0.06f, 0.15f);
    const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f};
    for (int k = 0; k < 4; ++k) {
        std::vector<float>& c = clips[S_CLEAR1 + k];
        for (int i = 0; i <= k + 1; ++i) tone(c, i * 0.055f, notes[i], notes[i] * 1.02f, 0.16f, 0.25f, 0, 0, 0.15f);
    }
    tone(clips[S_PUSH], 0, 160, 520, 0.32f, 0.45f, 9, 0.12f);         // boing
    tone(clips[S_PUSH], 0.05f, 320, 1040, 0.25f, 0.15f, 9, 0.12f);
    tone(clips[S_PUSHED], 0, 520, 120, 0.35f, 0.4f, 7, 0.15f);
    tone(clips[S_VANISH], 0, 1400, 200, 0.4f, 0.22f, 14, 0.05f);
    tone(clips[S_TICK], 0, 880, 880, 0.09f, 0.25f, 0, 0, 0.2f);
    tone(clips[S_GO], 0, 1175, 1175, 0.25f, 0.3f, 0, 0, 0.2f);
    tone(clips[S_GO], 0, 1760, 1760, 0.25f, 0.12f);
    const float win[] = {523.25f, 659.25f, 783.99f, 1046.5f, 783.99f, 1046.5f};
    for (int i = 0; i < 6; ++i) tone(clips[S_WIN], i * 0.11f, win[i], win[i], i == 5 ? 0.5f : 0.14f, 0.25f, 6, i == 5 ? 0.01f : 0, 0.2f);
    const float lose[] = {392, 370, 349.23f, 329.63f};
    for (int i = 0; i < 4; ++i) tone(clips[S_LOSE], i * 0.2f, lose[i], lose[i] * 0.97f, i == 3 ? 0.6f : 0.2f, 0.25f, 5, 0.02f, 0.25f);
    tone(clips[S_SELECT], 0, 660, 990, 0.07f, 0.25f);
    tone(clips[S_BACK], 0, 660, 440, 0.07f, 0.2f);
    tone(clips[S_JOIN], 0, 523, 523, 0.1f, 0.25f);
    tone(clips[S_JOIN], 0.1f, 784, 784, 0.18f, 0.25f);
}

void Audio::cb(void* ud, Uint8* stream, int len) {
    Audio* a = (Audio*)ud;
    float* out = (float*)stream;
    int frames = len / (int)(sizeof(float) * 2);
    std::memset(stream, 0, (size_t)len);
    for (Voice& v : a->voices) {
        if (v.clip < 0) continue;
        const std::vector<float>& c = a->clips[v.clip];
        for (int i = 0; i < frames && v.pos < c.size(); ++i, ++v.pos) {
            out[i * 2] += c[v.pos] * v.vl;
            out[i * 2 + 1] += c[v.pos] * v.vr;
        }
        if (v.pos >= c.size()) v.clip = -1;
    }
    for (int i = 0; i < frames * 2; ++i) out[i] = std::tanh(out[i]);
}

bool Audio::init() {
    build();
    SDL_AudioSpec want{}, have{};
    want.freq = RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512;
    want.callback = &Audio::cb; want.userdata = this;
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev) return false;
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void Audio::shutdown() { if (dev) SDL_CloseAudioDevice(dev); dev = 0; }

void Audio::play(Sfx s, float vol, float pan) {
    if (!dev || !enabled) return;
    SDL_LockAudioDevice(dev);
    Voice* slot = nullptr;
    for (Voice& v : voices) if (v.clip < 0) { slot = &v; break; }
    if (!slot) slot = &voices[0];
    slot->clip = s; slot->pos = 0;
    slot->vl = vol * std::fmin(1.0f, 1.0f - pan);
    slot->vr = vol * std::fmin(1.0f, 1.0f + pan);
    SDL_UnlockAudioDevice(dev);
}
