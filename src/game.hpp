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
    uint8_t level;                     // this player's own speed level (gravity + score multiplier)
    uint8_t startLevel;                // handicap: the level they started the round on
};

struct State {
    uint32_t frame;
    uint8_t  phase;
    uint16_t phaseTimer;
    int8_t   winner;      // -1 none, 0/1 player, 2 draw
    uint8_t  level;       // highest of the two players' levels (drives the music tempo)
    int8_t   center;      // global row of the push line (boundary above this row)
    uint8_t  reason;      // 0 normal, 1 opponent left, 2 forfeit
    uint8_t  setWins[2];  // rounds won in the current set
    uint8_t  setTarget;   // rounds needed to win the set (1 = single game, 2 = best of 3, 3 = best of 5)
    uint8_t  round;       // 1-based round number in the set
    uint8_t  setOver;     // the last round decided the set
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
    // brand new set; start0/start1 are each player's starting speed (1-9)
    void reset(uint32_t seed, int start0 = 1, int start1 = 1, int setTarget = 1);
    // next round of the same set: keeps the tally and the starting speeds
    void nextRound(uint32_t seed);
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

// CPU opponent. Plays at human speed: thinks, taps the piece across, then drops it.
class Bot {
public:
    int level = 1;              // 0 easy, 1 normal, 2 hard, 3 instant (tests / demo only)
    Input think(const Game& g, int p);
    // rows the CPU's planned spot will clear for the current piece (0 if nothing planned)
    int intent() const { return planLines; }
private:
    enum Stage { THINK, MOVE, SETTLE, DROP_SOFT, DROP_WAIT };
    uint8_t lastSpawn = 255;
    Stage stage = THINK;
    int tRot = 0, timer = 0, stuck = 0, lastX = -99, lastRot = -1;
    int waypoints[3] = {0, 0, 0}, nWay = 0, way = 0;
    bool softHeld = false;
    int planLines = 0;
    std::mt19937 rng{1234};
    void plan(const Game& g, int p);
    int range(int lo, int hi) { return lo + (int)(rng() % (unsigned)(hi - lo + 1)); }
    bool chance(int pct) { return (int)(rng() % 100) < pct; }
};

} // namespace sp
