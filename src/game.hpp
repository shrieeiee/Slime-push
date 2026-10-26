// Slime Push - Push-mode falling-block versus game
// Game simulation (host authoritative) + CPU bot.
#pragma once
#include <cstdint>
#include <random>

namespace sp {

constexpr int W = 10;
constexpr int H = 38;          // tall shared field (twice a normal well)
constexpr int NEXT_SHOWN = 5;
constexpr int ROCK = 8;        // starter blocks in the middle

enum Act : uint16_t {
    A_LEFT = 1, A_RIGHT = 2, A_SOFT = 4, A_HARD = 8,
    A_CW = 16, A_CCW = 32, A_HOLD = 64, A_READY = 128, A_FORFEIT = 256
};

struct Input { uint16_t held = 0; uint16_t pressed = 0; };

enum Phase : uint8_t { PH_COUNTDOWN = 0, PH_PLAY = 1, PH_OVER = 2 };

#pragma pack(push, 1)
struct Cell {
    uint8_t tf = 0;  // low nibble: type (0 empty, 1-7 piece, 8 rock); bit 4: face cell
    uint8_t id = 0;  // piece id, used to draw a piece as one gooey blob
    int type() const { return tf & 15; }
    bool face() const { return (tf & 16) != 0; }
};

struct PlayerView {
    int8_t type, rot, x, y;   // active piece, y in this player's LOCAL rows (0 = own end)
    uint8_t active;
    int8_t hold; uint8_t holdUsed;
    uint8_t next[NEXT_SHOWN];
    int32_t score; int16_t lines; uint8_t alive;
    // event counters: the renderer diffs these to play sounds / effects
    uint8_t evLock, evClear, evPush, evMove, evRotate, evHold, evVanish, evSpawn, evLand;
    uint8_t lastClearN;
    int8_t  lastClearRows[4];          // global rows
    uint8_t lastClearTypes[4][W];
    uint8_t lastPush;
    uint8_t ready;                     // wants a rematch
    uint8_t pieceId;                   // id used to draw the active piece
};

struct State {
    uint32_t frame;
    uint8_t  phase;
    uint16_t phaseTimer;
    int8_t   winner;      // -1 none, 0/1 player, 2 draw
    uint8_t  level;
    int8_t   center;      // global row of the push line (boundary above this row)
    uint8_t  reason;      // 0 normal, 1 opponent left, 2 forfeit
    Cell     grid[H][W];  // global rows: row 0 = player 0's end, row H-1 = player 1's end
    PlayerView pl[2];
};
#pragma pack(pop)

// piece geometry
const int8_t (*cells(int type, int rot))[2];   // 4 cells of (x,y), y down, in local coords
int faceIndex(int type);

inline int toGlobal(int p, int localRow) { return p == 0 ? localRow : H - 1 - localRow; }

class Game {
public:
    State s{};
    void reset(uint32_t seed);
    void step(const Input in[2]);
    void forfeit(int p);
    void disconnect(int p);   // p left the game
    bool collides(int p, int type, int rot, int x, int y) const;
    int  dropDistance(int p, bool* fallsOff) const;
    static int pushRows(int lines) { static const int t[5] = {0, 0, 1, 2, 4}; return t[lines < 0 ? 0 : (lines > 4 ? 4 : lines)]; }

private:
    struct Priv {
        std::mt19937 rng;
        uint8_t bag[7]; int bagPos = 7;
        uint8_t queue[NEXT_SHOWN];
        int dasT = 0, dasDir = 0;
        float grav = 0;
        int lockT = 0, lockResets = 0, lowestY = -99;
    } pv[2];
    uint8_t idCounter = 0;
    uint32_t playFrames = 0;

    uint8_t nextFromBag(int p);
    uint8_t takeNext(int p);
    bool spawn(int p, int type);
    bool tryMove(int p, int dx, int dy);
    bool tryRotate(int p, int dir);
    void groundedReset(int p);
    void lock(int p);
    void vanish(int p);
    void clearLines(int p);
    void push(int p, int k);
    void resolveOverlap(int q);
    bool offFar(int p, int type, int rot, int x, int y) const;
    void checkEnd();
    uint8_t newId();
};

// CPU opponent
class Bot {
public:
    int level = 1;              // 0 easy, 1 normal, 2 hard
    Input think(const Game& g, int p);
private:
    uint8_t lastSpawn = 255;
    int tRot = 0, tX = 3, timer = 0, stuck = 0, lastX = -99, lastRot = -1;
    bool planned = false;
    std::mt19937 rng{1234};
    void plan(const Game& g, int p);
};

} // namespace sp
