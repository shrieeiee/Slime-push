// Music: an original built-in chiptune soundtrack (synthesized at startup, no files needed)
// plus optional drop-in tracks from a "music" folder (OGG, MP3 or WAV).
#pragma once
#include <memory>
#include <string>
#include <vector>

// stereo interleaved float samples at 44.1 kHz, ready to loop
struct Track {
    std::string name;
    std::vector<float> pcm;
};
using TrackPtr = std::shared_ptr<const Track>;

namespace music {
TrackPtr titleTheme();    // "Bloop Bounce"   C major, 132 bpm
TrackPtr battleTheme();   // "Jelly Jam"      D major / B minor, 156 bpm
TrackPtr battleTheme2();  // "Wobble Rush"    F major, 168 bpm
// Loads every .ogg/.mp3/.wav in dir. Files whose name starts with "title" or "menu" go to
// `title`, everything else to `battle`.
void loadFolder(const std::string& dir, std::vector<TrackPtr>& title, std::vector<TrackPtr>& battle);
}
