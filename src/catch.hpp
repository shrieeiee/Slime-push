// Catch mode: steer a growing slime blob, catch falling pieces, make 4x4 squares that go off.
#pragma once
#include <cstdint>
#include <random>
#include <vector>
#include "game.hpp"

namespace sp {

class Catch {
public:
    static constexpr int CW = 14, CH = 20;
    static constexpr int FUSE_FRAMES = 600;          // 10 seconds before squares go off on their own

    struct MCell { int8_t dx, dy; uint8_t type, id; bool face, fused; };
    struct Faller { int type, rot, x; float y; uint8_t id; bool urchin; };
    struct Debris { float x, y, vy; uint8_t type; };

    std::vector<MCell> mass;                         // relative to the core; the core is (0,0)
    int cx = CW / 2, cy = CH - 6;                    // core position
    std::vector<Faller> fallers;
    std::vector<Debris> debris;
    int fuse = -1;                                   // frames until detonation, -1 when nothing is lit
    int energy = 100, score = 0, level = 1, booms = 0, squaresTotal = 0;
    bool over = false;
    int overTimer = 0, frames = 0, readyTimer = 90;

    // events for the UI
    int evCatch = 0, evMiss = 0, evHit = 0, evSquare = 0, evBoom = 0, evMove = 0, evRotate = 0, evLevel = 0;
    int lastBoomSquares = 0, lastBoomCells = 0, lastBoomScore = 0;
    float lastHitX = 0, lastHitY = 0, lastBoomX = 0, lastBoomY = 0;

    void start(uint32_t seed);
    // in: rotate (A_CW / A_CCW) and detonate (A_HARD); dirsHeld/dirsPressed: bit 0 up, 1 down, 2 left, 3 right
    void step(const Input& in, int dirsHeld, int dirsPressed);
    bool massAt(int x, int y, int* index = nullptr) const;
    int fusedCount() const;
    void findSquares();     // lights any new solid 4x4 squares in the blob

private:
    std::mt19937 rng;
    uint8_t idCounter = 0;
    int dasT[4] = {}, spawnTimer = 30, urchinTimer = 600;
    uint8_t newId();
    bool fitsMass(const std::vector<MCell>& m, int px, int py) const;
    bool tryShift(int dx, int dy);
    bool tryRotate(int dir);
    void spawnFaller(bool urchin);
    void attach(Faller& f);
    void hitByUrchin(const Faller& f);
    void detonate();
    void dropLoose();
    float fallSpeed() const;
};

} // namespace sp
