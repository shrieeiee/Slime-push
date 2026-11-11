#include "catch.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sp {

uint8_t Catch::newId() { if (++idCounter == 0) idCounter = 1; return idCounter; }

void Catch::start(uint32_t seed) {
    *this = Catch();
    rng.seed(seed);
    uint8_t id = newId();
    // the core slime plus a little T of slime to start catching with
    mass.push_back({0, 0, 5, id, true, false});
    mass.push_back({-1, 0, 5, id, false, false});
    mass.push_back({1, 0, 5, id, false, false});
    mass.push_back({0, 1, 5, id, false, false});
}

bool Catch::massAt(int x, int y, int* index) const {
    for (size_t i = 0; i < mass.size(); ++i)
        if (cx + mass[i].dx == x && cy + mass[i].dy == y) { if (index) *index = (int)i; return true; }
    return false;
}

int Catch::fusedCount() const {
    int n = 0;
    for (auto& m : mass) if (m.fused) ++n;
    return n;
}

bool Catch::fitsMass(const std::vector<MCell>& m, int px, int py) const {
    for (auto& c : m) {
        int x = px + c.dx, y = py + c.dy;
        if (x < 0 || x >= CW || y < 0 || y >= CH) return false;
    }
    return true;
}

bool Catch::tryShift(int dx, int dy) {
    if (!fitsMass(mass, cx + dx, cy + dy)) return false;
    cx += dx; cy += dy;
    evMove++;
    return true;
}

bool Catch::tryRotate(int dir) {
    std::vector<MCell> r = mass;
    for (auto& c : r) {
        int x = c.dx, y = c.dy;
        if (dir > 0) { c.dx = (int8_t)-y; c.dy = (int8_t)x; }
        else { c.dx = (int8_t)y; c.dy = (int8_t)-x; }
    }
    static const int kicks[7][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-2, 0}, {2, 0}};
    for (auto& k : kicks)
        if (fitsMass(r, cx + k[0], cy + k[1])) {
            mass = r; cx += k[0]; cy += k[1];
            evRotate++;
            return true;
        }
    return false;
}

static void fallerCells(const Catch::Faller& f, int out[4][2], int* n) {
    if (f.urchin) { out[0][0] = 0; out[0][1] = 0; *n = 1; return; }
    auto c = cells(f.type, f.rot);
    for (int i = 0; i < 4; ++i) { out[i][0] = c[i][0]; out[i][1] = c[i][1]; }
    *n = 4;
}

void Catch::spawnFaller(bool urchin) {
    Faller f{};
    f.urchin = urchin;
    f.id = newId();
    if (urchin) {
        f.x = (int)(rng() % CW);
        f.y = -1;
    } else {
        f.type = 1 + (int)(rng() % 7);
        f.rot = (int)(rng() % 4);
        auto c = cells(f.type, f.rot);
        int minx = 9, maxx = -9, maxy = -9;
        for (int i = 0; i < 4; ++i) { minx = std::min(minx, (int)c[i][0]); maxx = std::max(maxx, (int)c[i][0]); maxy = std::max(maxy, (int)c[i][1]); }
        int lo = -minx, hi = CW - 1 - maxx;
        f.x = lo + (int)(rng() % (unsigned)(hi - lo + 1));
        f.y = (float)-(maxy + 1);
    }
    fallers.push_back(f);
}

void Catch::attach(Faller& f) {
    int c[4][2], n;
    fallerCells(f, c, &n);
    int iy = (int)std::floor(f.y);
    // never overlap the blob: slide the piece up until it sits on top
    for (int guard = 0; guard < 30; ++guard) {
        bool overlap = false;
        for (int i = 0; i < n; ++i) if (massAt(f.x + c[i][0], iy + c[i][1])) overlap = true;
        if (!overlap) break;
        --iy;
    }
    int fi = faceIndex(f.type);
    int added = 0;
    for (int i = 0; i < n; ++i) {
        int ax = f.x + c[i][0], ay = iy + c[i][1];
        if (ax < 0 || ax >= CW || ay < 0 || ay >= CH) continue;   // spills over the top
        mass.push_back({(int8_t)(ax - cx), (int8_t)(ay - cy), (uint8_t)f.type, f.id, i == fi, false});
        ++added;
    }
    if (added) { evCatch++; score += 10 * level; }
    findSquares();
}

void Catch::hitByUrchin(const Faller& f) {
    int ix = f.x, iy = (int)std::floor(f.y);
    lastHitX = (float)ix; lastHitY = (float)iy;
    mass.erase(std::remove_if(mass.begin() + 1, mass.end(), [&](const MCell& m) {
        int x = cx + m.dx, y = cy + m.dy;
        return std::abs(x - ix) + std::abs(y - iy) <= 2;
    }), mass.end());
    energy -= 20;
    evHit++;
    dropLoose();
    if (energy <= 0) { energy = 0; over = true; }
}

void Catch::findSquares() {
    int occ[CH][CW];
    std::memset(occ, -1, sizeof(occ));
    for (size_t i = 0; i < mass.size(); ++i) {
        int x = cx + mass[i].dx, y = cy + mass[i].dy;
        if (x >= 0 && x < CW && y >= 0 && y < CH) occ[y][x] = (int)i;
    }
    for (int y = 0; y + 4 <= CH; ++y)
        for (int x = 0; x + 4 <= CW; ++x) {
            bool full = true, fresh = false;
            for (int yy = 0; yy < 4 && full; ++yy)
                for (int xx = 0; xx < 4; ++xx) {
                    int k = occ[y + yy][x + xx];
                    if (k < 0) { full = false; break; }
                    if (!mass[(size_t)k].fused) fresh = true;
                }
            if (!full || !fresh) continue;
            for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) mass[(size_t)occ[y + yy][x + xx]].fused = true;
            evSquare++;
            if (fuse < 0) fuse = FUSE_FRAMES;
        }
}

void Catch::detonate() {
    int occ[CH][CW];
    std::memset(occ, -1, sizeof(occ));
    for (size_t i = 0; i < mass.size(); ++i) {
        int x = cx + mass[i].dx, y = cy + mass[i].dy;
        if (x >= 0 && x < CW && y >= 0 && y < CH && mass[i].fused) occ[y][x] = (int)i;
    }
    int squares = 0;
    for (int y = 0; y + 4 <= CH; ++y)
        for (int x = 0; x + 4 <= CW; ++x) {
            bool full = true;
            for (int yy = 0; yy < 4 && full; ++yy) for (int xx = 0; xx < 4; ++xx) if (occ[y + yy][x + xx] < 0) { full = false; break; }
            if (full) ++squares;
        }
    squares = std::max(1, squares);
    float sx = 0, sy = 0;
    int cellsGone = 0;
    for (size_t i = 1; i < mass.size(); ++i) if (mass[i].fused) { sx += (float)(cx + mass[i].dx); sy += (float)(cy + mass[i].dy); ++cellsGone; }
    if (cellsGone) { lastBoomX = sx / (float)cellsGone; lastBoomY = sy / (float)cellsGone; }
    mass.erase(std::remove_if(mass.begin() + 1, mass.end(), [](const MCell& m) { return m.fused; }), mass.end());
    mass[0].fused = false;
    int gained = cellsGone * 10 * squares * level;
    score += gained;
    lastBoomSquares = squares; lastBoomCells = cellsGone; lastBoomScore = gained;
    energy = std::min(100, energy + 8 * squares);
    booms++;
    squaresTotal += squares;
    int nl = std::min(15, 1 + booms / 3);
    if (nl != level) { level = nl; evLevel++; }
    fuse = -1;
    evBoom++;
    dropLoose();
}

void Catch::dropLoose() {
    std::vector<char> keep(mass.size(), 0);
    std::vector<int> stack{0};
    keep[0] = 1;
    while (!stack.empty()) {
        int i = stack.back(); stack.pop_back();
        for (size_t j = 0; j < mass.size(); ++j) {
            if (keep[j]) continue;
            int d = std::abs(mass[j].dx - mass[(size_t)i].dx) + std::abs(mass[j].dy - mass[(size_t)i].dy);
            if (d == 1) { keep[j] = 1; stack.push_back((int)j); }
        }
    }
    std::vector<MCell> kept;
    for (size_t i = 0; i < mass.size(); ++i) {
        if (keep[i]) { kept.push_back(mass[i]); continue; }
        debris.push_back({(float)(cx + mass[i].dx), (float)(cy + mass[i].dy), -0.05f, mass[i].type});
        score += 5;
    }
    mass = kept;
}

float Catch::fallSpeed() const { return 0.035f + 0.008f * (float)(level - 1); }

void Catch::step(const Input& in, int dirsHeld, int dirsPressed) {
    frames++;
    for (auto& d : debris) { d.vy += 0.02f; d.y += d.vy; }
    debris.erase(std::remove_if(debris.begin(), debris.end(), [](const Debris& d) { return d.y > CH + 3; }), debris.end());
    if (over) { overTimer++; return; }
    if (readyTimer > 0) { --readyTimer; return; }

    // steering: tap to nudge, hold to keep going (a big blob moves a bit slower than a piece)
    static const int dirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (int i = 0; i < 4; ++i) {
        bool pressed = dirsPressed & (1 << i), held = dirsHeld & (1 << i);
        if (pressed) { tryShift(dirs[i][0], dirs[i][1]); dasT[i] = 0; }
        else if (held) { if (++dasT[i] >= 10 && (dasT[i] - 10) % 4 == 0) tryShift(dirs[i][0], dirs[i][1]); }
        else dasT[i] = 0;
    }
    if (in.pressed & A_CW) tryRotate(+1);
    if (in.pressed & A_CCW) tryRotate(-1);
    if ((in.pressed & A_HARD) && fuse >= 0) detonate();
    if (fuse > 0 && --fuse == 0) detonate();

    // new pieces and the occasional urchin
    int normal = 0;
    for (auto& f : fallers) if (!f.urchin) ++normal;
    int want = level >= 5 ? 2 : 1;
    if (--spawnTimer <= 0 && normal < want) { spawnFaller(false); spawnTimer = std::max(20, 70 - level * 5); }
    if (--urchinTimer <= 0) { spawnFaller(true); urchinTimer = std::max(180, 720 - level * 50) + (int)(rng() % 240); }

    float sp = fallSpeed();
    for (size_t k = 0; k < fallers.size();) {
        Faller& f = fallers[k];
        f.y += f.urchin ? sp * 0.85f : sp;
        int c[4][2], n;
        fallerCells(f, c, &n);
        int iy = (int)std::floor(f.y);
        bool touch = false, gone = true;
        for (int i = 0; i < n; ++i) {
            int x = f.x + c[i][0], y = iy + c[i][1];
            if (y < CH) gone = false;
            if (massAt(x, y) || massAt(x, y + 1)) touch = true;
        }
        if (touch) {
            Faller hit = f;
            hit.y = (float)iy;
            fallers.erase(fallers.begin() + (long)k);
            if (hit.urchin) hitByUrchin(hit); else attach(hit);
            if (over) return;
            continue;
        }
        if (gone) {
            if (!f.urchin) { energy -= 3; evMiss++; if (energy <= 0) { energy = 0; over = true; } }
            fallers.erase(fallers.begin() + (long)k);
            if (over) return;
            continue;
        }
        ++k;
    }
}

} // namespace sp

// ------------------------------------------------------------------ self-test
namespace sp {
int catchSelfTest() {
    int fails = 0, totalBooms = 0, totalSquares = 0, bestScore = 0, broken = 0;
    for (int game = 0; game < 6; ++game) {
        Catch c;
        c.start(100 + (uint32_t)game);
        std::mt19937 r(7 + (uint32_t)game);
        int frames = 0;
        while (!c.over && frames < 60 * 60 * 8) {
            // drift under the lowest falling piece, spin now and then, set squares off early sometimes
            Input in;
            int held = 0, pressed = 0;
            const Catch::Faller* target = nullptr;
            for (auto& f : c.fallers) if (!f.urchin && (!target || f.y > target->y)) target = &f;
            for (auto& f : c.fallers) if (f.urchin && std::abs(f.x - c.cx) <= 2 && f.y > c.cy - 8) { held |= f.x <= c.cx ? 8 : 4; target = nullptr; }
            if (target && (frames % 6) == 0) pressed |= target->x + 1 < c.cx ? 4 : target->x + 1 > c.cx ? 8 : 0;
            if (r() % 240 == 0) in.pressed |= A_CW;
            if (c.fuse >= 0 && c.fuse < 300 && r() % 60 == 0) in.pressed |= A_HARD;
            c.step(in, held, pressed);
            ++frames;
            // invariants: core first at (0,0), no two cells in one spot, everything inside the box
            if (c.mass.empty() || c.mass[0].dx || c.mass[0].dy) ++broken;
            for (size_t i = 0; i < c.mass.size(); ++i) {
                int x = c.cx + c.mass[i].dx, y = c.cy + c.mass[i].dy;
                if (x < 0 || x >= Catch::CW || y < 0 || y >= Catch::CH) ++broken;
                for (size_t j = i + 1; j < c.mass.size(); ++j) if (c.mass[i].dx == c.mass[j].dx && c.mass[i].dy == c.mass[j].dy) ++broken;
            }
            if (c.energy < 0 || c.energy > 100) ++broken;
        }
        totalBooms += c.booms; totalSquares += c.squaresTotal; bestScore = std::max(bestScore, c.score);
        printf("      catch game %d: %ds, score %d, %d booms, %d squares, level %d, energy %d, blob %zu cells%s\n",
               game, frames / 60, c.score, c.booms, c.squaresTotal, c.level, c.energy, c.mass.size(), c.over ? " (out of energy)" : "");
    }
    bool ok = broken == 0;
    printf("%s  catch: blob stays whole and inside the box (%d problems)\n", ok ? "ok  " : "FAIL", broken); fails += !ok;
    // squares: build a solid 4x4 with the core in the corner plus one loose bit hanging off it
    auto build = [](Catch& c) {
        c.start(5);
        c.mass.clear();
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) c.mass.push_back({(int8_t)x, (int8_t)y, 3, 9, x == 0 && y == 0, false});
        c.mass.push_back({4, 0, 6, 10, false, false});   // only joined through the square, so it falls off after
        c.readyTimer = 0;
        c.findSquares();
    };
    Catch c1; build(c1);
    ok = c1.fuse == Catch::FUSE_FRAMES && c1.fusedCount() == 16 && c1.evSquare == 1;
    printf("%s  catch: a solid 4x4 lights up with a 10 second fuse\n", ok ? "ok  " : "FAIL"); fails += !ok;
    Input boom; boom.pressed = A_HARD; boom.held = A_HARD;
    c1.step(boom, 0, 0);
    ok = c1.evBoom == 1 && c1.mass.size() == 1 && c1.lastBoomCells == 15 && c1.score > 0 && c1.debris.size() == 1;
    printf("%s  catch: pressing drop sets it off early, the core survives, loose bits fall away (score %d)\n", ok ? "ok  " : "FAIL", c1.score); fails += !ok;
    Catch c2; build(c2);
    int f2 = 0;
    while (c2.evBoom == 0 && f2 < Catch::FUSE_FRAMES + 5) { c2.step(Input{}, 0, 0); ++f2; }
    ok = c2.evBoom == 1 && f2 >= Catch::FUSE_FRAMES - 1;
    printf("%s  catch: left alone, it goes off by itself after %.1f seconds\n", ok ? "ok  " : "FAIL", f2 / 60.0); fails += !ok;
    (void)totalSquares; (void)totalBooms; (void)bestScore;
    return fails;
}
} // namespace sp
