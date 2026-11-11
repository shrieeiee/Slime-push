// Single-player modes on a standard 10x20 well: Marathon, Mission and Puzzle.
#pragma once
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "game.hpp"

namespace sp {

enum SoloMode { SM_MARATHON = 0, SM_MISSION = 1, SM_PUZZLE = 2 };

// ---- missions
enum MissionKind : uint8_t { MK_AT_ONCE, MK_WITH_PIECE, MK_TSPIN, MK_TOTAL };
struct Mission {
    uint8_t kind = MK_AT_ONCE, n = 1, piece = 0;
    int progress = 0;          // MK_TOTAL: lines so far
    int timeLeft = 0, timeMax = 0;   // frames
    std::string text() const;
};

// ---- puzzles: a few rows of slime at the bottom and a fixed set of pieces; clear the lot
struct PuzzleDef {
    uint8_t rows;              // how many bottom rows the layout uses
    uint16_t cells[6];         // bottom-up, bit c = column c filled
    uint8_t n;                 // piece count
    uint8_t pieces[7];
    uint16_t solutions;        // how many ways to solve it (fewer = harder)
};
extern const PuzzleDef PUZZLES[];
extern const int PUZZLE_COUNT;

// one placement in a solution: optionally press hold first, then rotate, move and hard drop
struct PuzzleMove { bool hold; int8_t rot, x; };
// counts solutions (up to cap) and optionally returns one of them
int solvePuzzle(const PuzzleDef& p, std::vector<PuzzleMove>* solution, int cap);
// builds a batch of puzzles and prints them as C++ (used to make puzzles_data.h)
void generatePuzzles(FILE* out, uint32_t seed);
class Solo;
// simple placement picker (used by the demo player and the self-test)
PuzzleMove soloGreedyMove(const Solo& s);

class Solo {
public:
    static constexpr int SW = 10, SH = 24, HIDDEN = 4;   // rows 0-3 are above the visible well
    int mode = SM_MARATHON;
    int puzzle = -1;

    Cell grid[SH][SW] = {};
    int type = 0, rot = 0, x = 3, y = 0;
    bool active = false;
    uint8_t pieceId = 0;
    int hold = -1;
    bool holdUsed = false;
    std::vector<uint8_t> queue;    // upcoming pieces (front = next)

    int level = 1, lines = 0, score = 0;
    bool over = false, won = false;
    int overTimer = 0;             // frames since the game ended
    int readyTimer = 0;            // "ready" pause before the first piece
    int frames = 0;

    // mission mode
    Mission mission;
    int missionsDone = 0, missionLevel = 1;
    int missionFlash = 0;          // >0: just completed (frames), <0: just failed

    // event counters (the UI diffs these for sounds and effects)
    int evLock = 0, evClear = 0, evMove = 0, evRotate = 0, evHold = 0, evLand = 0, evSpawn = 0;
    int evMissionOk = 0, evMissionFail = 0, evLevel = 0, evGarbage = 0;
    int lastClearN = 0;
    int lastClearRows[4] = {};
    uint8_t lastClearTypes[4][SW] = {};
    bool lastTSpin = false;
    std::string lastClearName;

    void start(int mode, uint32_t seed, int puzzleIndex = -1);
    void step(const Input& in);

    bool collides(int t, int r, int px, int py) const;
    int ghostY() const;
    bool boardEmpty() const;
    int stackHeight() const;       // rows from the floor to the highest block

private:
    std::mt19937 rng;
    uint8_t bag[7] = {};
    int bagPos = 7;
    int dasT = 0, dasDir = 0;
    float grav = 0;
    int lockT = 0, lockResets = 0, lowestY = -99;
    bool lastWasRotate = false;
    uint8_t idCounter = 0;

    uint8_t newId();
    uint8_t fromBag();
    void refill();
    bool spawn(int t);
    bool tryMove(int dx, int dy);
    bool tryRotate(int dir);
    void groundedReset();
    void lockPiece();
    bool isTSpin() const;
    void clearLines(int lockedType, bool tspin);
    void addGarbage(int rows);
    void newMission();
    void missionCheck(int n, int lockedType, bool tspin);
    void finish(bool win);
    float gravityPerFrame() const;
};

} // namespace sp
