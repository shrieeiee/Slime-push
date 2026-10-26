#include "music.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <sstream>
#include <random>
#include <SDL.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO_SKIP
#include "../third_party/stb_vorbis.c"
#undef L
#undef C
#undef R
#define DR_MP3_IMPLEMENTATION
#include "../third_party/dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav.h"
#pragma GCC diagnostic pop

namespace {

const int RATE = 44100;
const float TAU = 6.2831853f;

// ------------------------------------------------------------------ song description
struct SongDef {
    const char* name;
    float bpm;
    const char* chords;   // one entry per bar, "C", "Am", "F#m", "Bb/C" = two chords in one bar
    const char* lead;     // eighth-note steps; note, "-" hold, "." rest, "|" ignored
    const char* drums;    // 16 sixteenths per bar: k kick, s snare, h hat, o open hat, b kick+hat, n snare+hat
    const char* fill;     // drum bar used every 4th bar
    float leadDuty;
};

float noteFreq(const std::string& n) {
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    if (n.empty()) return 0;
    int semi = base[n[0] - 'A'];
    size_t i = 1;
    if (i < n.size() && n[i] == '#') { semi++; i++; }
    else if (i < n.size() && n[i] == 'b') { semi--; i++; }
    int oct = std::atoi(n.c_str() + i);
    int midi = (oct + 1) * 12 + semi;
    return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
}

struct Chord { float root; bool minor; };
Chord parseChord(std::string c) {
    Chord ch{};
    std::string r(1, c[0]);
    size_t i = 1;
    if (i < c.size() && (c[i] == '#' || c[i] == 'b')) { r += c[i]; i++; }
    ch.minor = (i < c.size() && c[i] == 'm');
    ch.root = noteFreq(r + "2");
    return ch;
}

std::vector<std::string> tokens(const char* s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (ss >> t) if (t != "|") out.push_back(t);
    return out;
}

// ------------------------------------------------------------------ tiny chip synth
struct Mix {
    std::vector<float> L, R;
    explicit Mix(size_t n) : L(n, 0), R(n, 0) {}
    void add(size_t i, float v, float pan) {   // pan -1..1
        if (i >= L.size()) return;
        L[i] += v * std::min(1.0f, 1.0f - pan);
        R[i] += v * std::min(1.0f, 1.0f + pan);
    }
};

float env(float t, float gate, float a, float d, float sus, float rel) {
    if (t < a) return t / a;
    if (t < a + d) return 1.0f - (1.0f - sus) * (t - a) / d;
    if (t < gate) return sus;
    float r = 1.0f - (t - gate) / rel;
    return r > 0 ? sus * r : 0;
}

void pulse(Mix& m, double start, float f, float gate, float duty, float vol, float pan, bool vibrato) {
    size_t s0 = (size_t)(start * RATE), n = (size_t)((gate + 0.05f) * RATE);
    double ph = 0;
    for (size_t i = 0; i < n; ++i) {
        float t = (float)i / RATE;
        float ff = f;
        if (vibrato && t > 0.14f) ff *= 1.0f + 0.006f * std::sin(TAU * 5.5f * t);
        ph += ff / RATE;
        float p = (float)(ph - std::floor(ph));
        float v = p < duty ? 1.0f : -1.0f;
        m.add(s0 + i, v * vol * env(t, gate, 0.003f, 0.08f, 0.65f, 0.04f), pan);
    }
}

void triangle(Mix& m, double start, float f, float gate, float vol) {
    size_t s0 = (size_t)(start * RATE), n = (size_t)((gate + 0.02f) * RATE);
    double ph = 0;
    for (size_t i = 0; i < n; ++i) {
        float t = (float)i / RATE;
        ph += f / RATE;
        float p = (float)(ph - std::floor(ph));
        float v = p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
        v = std::round(v * 7.5f) / 7.5f;   // 4-bit stepped, like the NES
        m.add(s0 + i, v * vol * env(t, gate, 0.002f, 0.01f, 1.0f, 0.015f), 0);
    }
}

struct Noise {
    uint16_t lfsr = 1;
    float next(bool shortMode) {
        int bit = ((lfsr >> 0) ^ (lfsr >> (shortMode ? 6 : 1))) & 1;
        lfsr = (uint16_t)((lfsr >> 1) | (bit << 14));
        return (lfsr & 1) ? 1.0f : -1.0f;
    }
};

void drum(Mix& m, double start, char k, Noise& nz) {
    size_t s0 = (size_t)(start * RATE);
    auto kick = [&]() {
        double ph = 0;
        for (size_t i = 0; i < (size_t)(0.14f * RATE); ++i) {
            float t = (float)i / RATE;
            float f = 45 + 120 * std::exp(-t * 30);
            ph += f / RATE;
            m.add(s0 + i, std::sin(TAU * (float)ph) * 0.55f * std::exp(-t * 18), 0);
        }
    };
    auto snare = [&]() {
        for (size_t i = 0; i < (size_t)(0.14f * RATE); ++i) {
            float t = (float)i / RATE;
            float v = nz.next(false) * 0.22f * std::exp(-t * 26) + std::sin(TAU * 185 * t) * 0.18f * std::exp(-t * 40);
            m.add(s0 + i, v, 0.05f);
        }
    };
    auto hat = [&](float len) {
        float prev = 0;
        for (size_t i = 0; i < (size_t)(len * RATE); ++i) {
            float t = (float)i / RATE;
            float n = nz.next(true);
            float hp = n - prev; prev = n;          // crude high-pass for a crisp tick
            m.add(s0 + i, hp * 0.05f * std::exp(-t * (len < 0.05f ? 90 : 22)), 0.25f);
        }
    };
    switch (k) {
        case 'k': kick(); break;
        case 's': snare(); break;
        case 'h': hat(0.035f); break;
        case 'o': hat(0.14f); break;
        case 'b': kick(); hat(0.035f); break;
        case 'n': snare(); hat(0.035f); break;
        default: break;
    }
}

TrackPtr render(const SongDef& d) {
    auto chords = tokens(d.chords);
    auto lead = tokens(d.lead);
    const int bars = (int)chords.size();
    const double eighth = 60.0 / d.bpm / 2, sixteenth = eighth / 2, bar = eighth * 8;
    size_t loopLen = (size_t)(bars * bar * RATE);
    if ((int)lead.size() != bars * 8)
        fprintf(stderr, "music: %s lead has %d steps, expected %d\n", d.name, (int)lead.size(), bars * 8);
    Mix m(loopLen + RATE * 2);

    // lead + echo
    for (size_t i = 0; i < lead.size(); ++i) {
        const std::string& tk = lead[i];
        if (tk == "-" || tk == ".") continue;
        size_t len = 1;
        while (i + len < lead.size() && lead[i + len] == "-") ++len;
        float f = noteFreq(tk);
        float gate = (float)(len == 1 ? eighth * 0.55 : len * eighth * 0.9);
        double st = i * eighth;
        pulse(m, st, f, gate, d.leadDuty, 0.17f, 0.0f, len > 1);
        pulse(m, st + eighth * 1.5, f, gate, d.leadDuty, 0.055f, 0.6f, false);   // dotted-eighth echo
    }

    // chords: arpeggio (16ths) + bouncy octave bass (8ths)
    for (int b = 0; b < bars; ++b) {
        std::string c = chords[(size_t)b];
        std::vector<std::string> parts;
        size_t sl = c.find('/');
        if (sl == std::string::npos) parts = {c, c};
        else parts = {c.substr(0, sl), c.substr(sl + 1)};
        for (int half = 0; half < 2; ++half) {
            Chord ch = parseChord(parts[(size_t)half]);
            float third = ch.minor ? 1.189207f : 1.259921f, fifth = 1.498307f;
            float arp[4] = {ch.root * 4, ch.root * 4 * third, ch.root * 4 * fifth, ch.root * 8};
            static const int order[8] = {0, 1, 2, 3, 2, 1, 0, 2};
            for (int k = 0; k < 8; ++k) {
                double st = b * bar + half * bar / 2 + k * sixteenth;
                pulse(m, st, arp[order[k]], (float)(sixteenth * 0.6), 0.125f, 0.045f, -0.45f, false);
            }
            for (int k = 0; k < 4; ++k) {
                double st = b * bar + half * bar / 2 + k * eighth;
                float f = (k % 2 == 0) ? ch.root : ch.root * 2;
                if (half == 1 && k == 3) f = ch.root * fifth;    // little walk-up into the next bar
                triangle(m, st, f, (float)(eighth * 0.7), 0.30f);
            }
        }
        // drums
        Noise nz; nz.lfsr = (uint16_t)(0x5a5 + b);
        const char* pat = ((b % 4) == 3) ? d.fill : d.drums;
        for (int k = 0; k < 16; ++k) drum(m, b * bar + k * sixteenth, pat[k], nz);
    }

    // fold the tail (echo, release) back onto the start so the loop is seamless
    auto t = std::make_shared<Track>();
    t->name = d.name;
    t->pcm.resize(loopLen * 2);
    for (size_t i = 0; i < loopLen; ++i) {
        float l = m.L[i], r = m.R[i];
        if (i + loopLen < m.L.size()) { l += m.L[i + loopLen]; r += m.R[i + loopLen]; }
        t->pcm[i * 2] = std::tanh(l * 1.1f) * 0.8f;
        t->pcm[i * 2 + 1] = std::tanh(r * 1.1f) * 0.8f;
    }
    return t;
}

// ------------------------------------------------------------------ the tunes (original)
const SongDef TITLE = {
    "Bloop Bounce", 132,
    "C Am F G  C Am F G  F G Em Am  F G C C",
    "E5 . G5 . C6 - B5 G5 | A5 . C6 . E5 - G5 . | F5 A5 C6 A5 G5 - F5 E5 | D5 - G5 - B5 - . . |"
    "E5 . G5 . C6 - D6 C6 | A5 - E5 - C6 - B5 A5 | A5 G5 F5 . A5 . C6 . | B5 - D6 - G5 . . . |"
    "C6 . A5 . F5 . A5 C6 | D6 . B5 . G5 . B5 D6 | E6 - D6 - B5 - G5 . | C6 - B5 - A5 - E5 . |"
    "F5 A5 C6 F6 E6 - C6 . | D6 . B5 . G5 B5 D6 G6 | E6 - C6 - G5 - E5 - | C6 . G5 . C5 . . . |",
    "k.h.s.h.k.k.s.h.", "k.h.s.h.k.h.snss", 0.25f};

const SongDef BATTLE = {
    "Jelly Jam", 156,
    "Bm G D A  Bm G D A  G A F#m Bm  G A D A",
    "F#5 . F#5 D5 B4 - D5 F#5 | G5 . G5 F#5 D5 - B4 . | A5 . A5 F#5 D5 - F#5 A5 | E5 - C#5 - E5 . A5 . |"
    "B5 . A5 F#5 D5 . F#5 . | G5 . B5 . D6 - B5 G5 | F#5 . A5 . D6 . C#6 A5 | E5 - - . C#5 . E5 . |"
    "D6 . B5 . G5 . B5 . | C#6 . A5 . E5 . A5 . | C#6 - A5 - F#5 - A5 C#6 | D6 - C#6 - B5 . F#5 . |"
    "G5 B5 D6 B5 G5 B5 D6 G6 | E6 . C#6 . A5 . E5 . | F#5 A5 D6 F#6 E6 D6 C#6 A5 | E5 . E6 . C#6 . A5 . |",
    "b.hhn.hhb.hbn.hh", "b.hhn.hhb.n.nnnn", 0.25f};

const SongDef BATTLE2 = {
    "Wobble Rush", 168,
    "F C Dm Bb  F C Bb/C F  Bb C Am Dm  Bb C Gm/C F",
    "F5 . A5 . C6 . A5 C6 | E5 . G5 . C6 - G5 . | F5 . A5 . D6 . A5 F5 | D5 - F5 - Bb5 - A5 G5 |"
    "A5 . C6 . F6 - E6 D6 | C6 . G5 . E5 - G5 C6 | D6 - Bb5 - E6 - C6 - | F6 - - . C6 . F5 . |"
    "D6 . D6 C6 Bb5 . F5 . | E6 . E6 D6 C6 . G5 . | C6 . E6 . A5 . C6 E6 | F6 - E6 - D6 - A5 . |"
    "Bb5 D6 F6 D6 Bb5 D6 F6 Bb6 | G6 - E6 - C6 - G5 . | Bb5 . G5 . E5 . G5 C6 | F5 - A5 - C6 - F6 . |",
    "b.h.n.hkb.hkn.hh", "b.h.n.hkn.nnn.nn", 0.5f};

// ------------------------------------------------------------------ files
std::vector<float> toStereo44(const float* in, size_t frames, int ch, int rate) {
    std::vector<float> st(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        float l = in[i * (size_t)ch], r = ch > 1 ? in[i * (size_t)ch + 1] : l;
        st[i * 2] = l; st[i * 2 + 1] = r;
    }
    if (rate == RATE || rate <= 0) return st;
    double step = (double)rate / RATE;
    size_t outFrames = (size_t)(frames / step);
    std::vector<float> out(outFrames * 2);
    for (size_t i = 0; i < outFrames; ++i) {
        double p = i * step; size_t a = (size_t)p; double fr = p - a;
        size_t b = std::min(a + 1, frames - 1);
        for (int c = 0; c < 2; ++c) out[i * 2 + c] = (float)(st[a * 2 + c] * (1 - fr) + st[b * 2 + c] * fr);
    }
    return out;
}

TrackPtr loadFile(const std::string& path, const std::string& name) {
    std::string lower = path;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    auto ends = [&](const char* e) { size_t n = strlen(e); return lower.size() > n && lower.compare(lower.size() - n, n, e) == 0; };
    auto t = std::make_shared<Track>();
    t->name = name;
    if (ends(".ogg")) {
        int ch = 0, rate = 0; short* data = nullptr;
        int frames = stb_vorbis_decode_filename(path.c_str(), &ch, &rate, &data);
        if (frames <= 0 || !data) return nullptr;
        std::vector<float> f((size_t)frames * ch);
        for (size_t i = 0; i < f.size(); ++i) f[i] = data[i] / 32768.0f;
        free(data);
        t->pcm = toStereo44(f.data(), (size_t)frames, ch, rate);
    } else if (ends(".mp3")) {
        drmp3_config cfg; drmp3_uint64 frames = 0;
        float* data = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &cfg, &frames, nullptr);
        if (!data) return nullptr;
        t->pcm = toStereo44(data, (size_t)frames, (int)cfg.channels, (int)cfg.sampleRate);
        drmp3_free(data, nullptr);
    } else if (ends(".wav")) {
        unsigned int ch = 0, rate = 0; drwav_uint64 frames = 0;
        float* data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
        if (!data) return nullptr;
        t->pcm = toStereo44(data, (size_t)frames, (int)ch, (int)rate);
        drwav_free(data, nullptr);
    } else {
        return nullptr;
    }
    return t->pcm.empty() ? nullptr : t;
}

} // namespace

namespace music {

TrackPtr titleTheme() { static TrackPtr t = render(TITLE); return t; }
TrackPtr battleTheme() { static TrackPtr t = render(BATTLE); return t; }
TrackPtr battleTheme2() { static TrackPtr t = render(BATTLE2); return t; }

std::vector<std::string> listDir(const std::string& dir);

void loadFolder(const std::string& dir, std::vector<TrackPtr>& title, std::vector<TrackPtr>& battle) {
    for (const std::string& f : listDir(dir)) {
        std::string lower = f;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        TrackPtr t = loadFile(dir + "/" + f, f);
        if (!t) continue;
        if (lower.rfind("title", 0) == 0 || lower.rfind("menu", 0) == 0) title.push_back(t);
        else battle.push_back(t);
    }
}

} // namespace music

// directory listing, kept apart from the decoders
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
std::vector<std::string> music::listDir(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}
#else
#include <dirent.h>
std::vector<std::string> music::listDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') out.push_back(e->d_name);
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}
#endif
