#include "solo.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace sp {

static const int LOCK_FRAMES = 30, LOCK_RESETS = 15, DAS = 10, ARR = 2;

// ------------------------------------------------------------------ missions

std::string Mission::text() const {
    char b[96];
    switch (kind) {
        case MK_AT_ONCE:
            if (n == 1) snprintf(b, sizeof(b), "CLEAR A LINE");
            else snprintf(b, sizeof(b), "CLEAR %d LINES AT ONCE", n);
            break;
        case MK_WITH_PIECE:
            snprintf(b, sizeof(b), n == 1 ? "CLEAR A LINE WITH" : "CLEAR %d LINES WITH", n);
            break;
        case MK_TSPIN: snprintf(b, sizeof(b), "CLEAR A LINE WITH A T-SPIN"); break;
        default: snprintf(b, sizeof(b), "CLEAR %d LINES IN TOTAL", n); break;
    }
    return b;
}

// ------------------------------------------------------------------ engine
uint8_t Solo::newId() { if (++idCounter == 0) idCounter = 1; return idCounter; }

uint8_t Solo::fromBag() {
    if (bagPos >= 7) {
        for (int i = 0; i < 7; ++i) bag[i] = (uint8_t)(i + 1);
        for (int i = 6; i > 0; --i) { int j = (int)(rng() % (unsigned)(i + 1)); std::swap(bag[i], bag[j]); }
        bagPos = 0;
    }
    return bag[bagPos++];
}

void Solo::refill() {
    if (mode == SM_PUZZLE) return;
    while (queue.size() < 6) queue.push_back(fromBag());
}

void Solo::start(int m, uint32_t seed, int puzzleIndex) {
    *this = Solo();
    mode = m;
    puzzle = puzzleIndex;
    rng.seed(seed);
    if (mode == SM_PUZZLE) {
        const PuzzleDef& p = PUZZLES[std::max(0, std::min(PUZZLE_COUNT - 1, puzzleIndex))];
        for (int r = 0; r < p.rows; ++r) {
            int row = SH - 1 - r;
            uint8_t id = newId();
            for (int c = 0; c < SW; ++c)
                if (p.cells[r] & (1u << c)) {
                    grid[row][c].tf = (uint8_t)(ROCK | (((c * 7 + r * 3) % 5 == 0) ? 16 : 0));
                    grid[row][c].id = id;
                }
        }
        queue.assign(p.pieces, p.pieces + p.n);
    } else if (mode == SM_MISSION) {
        addGarbage(5);
        evGarbage = 0;
        newMission();
    }
    refill();
    readyTimer = 90;
}

bool Solo::collides(int t, int r, int px, int py) const {
    auto c = cells(t, r);
    for (int i = 0; i < 4; ++i) {
        int cx = px + c[i][0], cy = py + c[i][1];
        if (cx < 0 || cx >= SW || cy >= SH) return true;
        if (cy < 0) continue;
        if (grid[cy][cx].tf & 15) return true;
    }
    return false;
}

int Solo::ghostY() const {
    int gy = y;
    while (!collides(type, rot, x, gy + 1)) ++gy;
    return gy;
}

bool Solo::boardEmpty() const {
    for (int r = 0; r < SH; ++r) for (int c = 0; c < SW; ++c) if (grid[r][c].tf & 15) return false;
    return true;
}

int Solo::stackHeight() const {
    for (int r = 0; r < SH; ++r) for (int c = 0; c < SW; ++c) if (grid[r][c].tf & 15) return SH - r;
    return 0;
}

bool Solo::spawn(int t) {
    type = t; rot = 0; x = 3;
    int y0 = HIDDEN - 1;
    for (int dy = 0; dy <= 2; ++dy) {
        if (!collides(t, 0, x, y0 - dy)) {
            y = y0 - dy;
            active = true;
            pieceId = newId();
            grav = 0; lockT = 0; lockResets = 0; lowestY = y; lastWasRotate = false;
            evSpawn++;
            return true;
        }
    }
    finish(false);   // blocked at the top
    return false;
}

void Solo::groundedReset() {
    if (collides(type, rot, x, y + 1) && lockResets < LOCK_RESETS) { lockT = 0; lockResets++; }
}

bool Solo::tryMove(int dx, int dy) {
    if (collides(type, rot, x + dx, y + dy)) return false;
    x += dx; y += dy;
    if (dx) { evMove++; lastWasRotate = false; groundedReset(); }
    if (dy > 0) { lastWasRotate = false; if (y > lowestY) { lowestY = y; lockResets = 0; } }
    return true;
}

bool Solo::tryRotate(int dir) {
    if (type == 2) { evRotate++; return true; }
    int to = (rot + (dir > 0 ? 1 : 3)) & 3;
    auto k = srsKicks(type, rot, to);
    for (int i = 0; i < 5; ++i) {
        int nx = x + k[i][0], ny = y - k[i][1];
        if (!collides(type, to, nx, ny)) {
            x = nx; y = ny; rot = to;
            evRotate++;
            lastWasRotate = true;
            groundedReset();
            return true;
        }
    }
    return false;
}

bool Solo::isTSpin() const {
    if (type != 3 || !lastWasRotate) return false;
    int cx = x + 1, cy = y + 1, filled = 0;
    const int d[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (auto& o : d) {
        int px = cx + o[0], py = cy + o[1];
        if (px < 0 || px >= SW || py >= SH) { ++filled; continue; }
        if (py >= 0 && (grid[py][px].tf & 15)) ++filled;
    }
    return filled >= 3;
}

void Solo::lockPiece() {
    bool tspin = isTSpin();
    auto c = cells(type, rot);
    int fi = faceIndex(type);
    bool allHidden = true;
    for (int i = 0; i < 4; ++i) {
        int cx = x + c[i][0], cy = y + c[i][1];
        if (cy >= HIDDEN) allHidden = false;
        if (cy < 0) continue;
        grid[cy][cx].tf = (uint8_t)(type | (i == fi ? 16 : 0));
        grid[cy][cx].id = pieceId;
    }
    int locked = type;
    active = false;
    holdUsed = false;
    evLock++;
    if (allHidden && mode != SM_PUZZLE) { finish(false); return; }
    clearLines(locked, tspin);
    if (over) return;
    if (mode == SM_PUZZLE && boardEmpty()) { finish(true); return; }
    // next piece
    if (queue.empty()) {
        if (hold >= 0) { int t = hold; hold = -1; spawn(t); return; }
        finish(mode == SM_PUZZLE ? boardEmpty() : false);
        return;
    }
    int t = queue.front();
    queue.erase(queue.begin());
    refill();
    spawn(t);
}

void Solo::clearLines(int lockedType, bool tspin) {
    bool full[SH];
    int n = 0;
    for (int r = 0; r < SH; ++r) {
        full[r] = true;
        for (int c = 0; c < SW; ++c) if (!(grid[r][c].tf & 15)) { full[r] = false; break; }
        if (full[r]) {
            if (n < 4) {
                lastClearRows[n] = r;
                for (int c = 0; c < SW; ++c) lastClearTypes[n][c] = (uint8_t)grid[r][c].type();
            }
            ++n;
        }
    }
    lastTSpin = tspin;
    static const char* names[5] = {"", "SINGLE", "DOUBLE", "TRIPLE", "QUAD SPLAT!"};
    lastClearName = tspin ? (n ? std::string("T-SPIN ") + names[std::min(n, 3)] : std::string("T-SPIN")) : names[std::min(n, 4)];
    if (n) {
        Cell tmp[SH][SW];
        std::memset(tmp, 0, sizeof(tmp));
        int w = SH - 1;
        for (int r = SH - 1; r >= 0; --r) if (!full[r]) { std::memcpy(tmp[w], grid[r], sizeof(tmp[w])); --w; }
        std::memcpy(grid, tmp, sizeof(tmp));
        lastClearN = std::min(n, 4);
        evClear++;
    }
    if (mode != SM_PUZZLE) {
        static const int base[5] = {0, 100, 300, 500, 800}, tbase[4] = {400, 800, 1200, 1600};
        score += (tspin ? tbase[std::min(n, 3)] : base[std::min(n, 4)]) * level;
    }
    lines += n;
    if (mode == SM_MARATHON) {
        int nl = std::min(20, 1 + lines / 10);
        if (nl != level) { level = nl; evLevel++; }
        if (lines >= 200) finish(true);
    } else if (mode == SM_MISSION) {
        missionCheck(n, lockedType, tspin);
    }
}

void Solo::addGarbage(int k) {
    for (int r = 0; r < k; ++r) for (int c = 0; c < SW; ++c) if (grid[r][c].tf & 15) { finish(false); return; }
    std::memmove(grid[0], grid[k], sizeof(Cell) * SW * (SH - k));
    int hole = (int)(rng() % SW);
    for (int r = SH - k; r < SH; ++r) {
        if (rng() % 3 == 0) hole = (int)(rng() % SW);
        uint8_t id = newId();
        for (int c = 0; c < SW; ++c) {
            grid[r][c] = Cell{};
            if (c == hole) continue;
            grid[r][c].tf = (uint8_t)(ROCK | ((c == (hole + 3) % SW) ? 16 : 0));
            grid[r][c].id = id;
        }
    }
    if (active) {
        while (collides(type, rot, x, y)) { --y; if (y < -2) { finish(false); return; } }
    }
    evGarbage++;
}

void Solo::newMission() {
    int L = missionLevel;
    Mission m;
    int r = (int)(rng() % 100);
    int secs = std::max(20, 45 - 3 * (L - 1));
    if (L >= 3 && r < 10) { m.kind = MK_TSPIN; m.n = 1; secs += 15; }
    else if (r < 40) { m.kind = MK_AT_ONCE; m.n = (uint8_t)(1 + rng() % (unsigned)std::min(4, L + 1)); if (m.n >= 3) secs += 10; }
    else if (r < 80) {
        m.kind = MK_WITH_PIECE;
        m.piece = (uint8_t)(1 + rng() % 7);
        int maxN = m.piece == 1 ? 4 : m.piece == 2 ? 2 : 3;
        m.n = (uint8_t)(1 + rng() % (unsigned)std::min(maxN, L));
        if (m.n >= 3) secs += 10;
    } else { m.kind = MK_TOTAL; m.n = (uint8_t)(3 + L + rng() % 3); secs += m.n * 2; }
    m.timeLeft = m.timeMax = secs * 60;
    mission = m;
}

void Solo::missionCheck(int n, int lockedType, bool tspin) {
    bool done = false;
    switch (mission.kind) {
        case MK_AT_ONCE: done = n >= mission.n; break;
        case MK_WITH_PIECE: done = lockedType == mission.piece && n >= mission.n; break;
        case MK_TSPIN: done = tspin && n >= 1; break;
        default: mission.progress += n; done = mission.progress >= mission.n; break;
    }
    if (!done) return;
    score += (1000 + mission.timeLeft / 60 * 50) * missionLevel;
    missionsDone++;
    evMissionOk++;
    missionFlash = 60;
    if (missionsDone % 5 == 0) { missionLevel++; level = std::min(20, missionLevel); evLevel++; }
    newMission();
}

void Solo::finish(bool win) {
    if (over) return;
    over = true; won = win; active = false; overTimer = 0;
}

float Solo::gravityPerFrame() const {
    if (mode == SM_PUZZLE) return 0;
    return 1.0f / framesPerRow(level);
}

void Solo::step(const Input& in) {
    frames++;
    if (over) { overTimer++; return; }
    if (readyTimer > 0) {
        if (--readyTimer == 0) {
            int t = queue.front();
            queue.erase(queue.begin());
            refill();
            spawn(t);
        }
        return;
    }
    if (missionFlash > 0) --missionFlash; else if (missionFlash < 0) ++missionFlash;
    if (mode == SM_MISSION) {
        if (--mission.timeLeft <= 0) {
            evMissionFail++;
            missionFlash = -60;
            addGarbage(2);
            if (over) return;
            newMission();
        }
    }
    if (!active) return;

    if ((in.pressed & A_HOLD) && !holdUsed) {
        int cur = type;
        evHold++;
        if (hold < 0) {
            hold = cur;
            active = false;
            if (queue.empty()) { int t = hold; hold = -1; spawn(t); }
            else { int t = queue.front(); queue.erase(queue.begin()); refill(); spawn(t); }
        } else {
            int t = hold; hold = cur; spawn(t);
        }
        holdUsed = true;
        if (!active) return;
    }
    if (in.pressed & A_CW) tryRotate(+1);
    if (in.pressed & A_CCW) tryRotate(-1);

    int dir = 0;
    if (in.held & A_LEFT) dir -= 1;
    if (in.held & A_RIGHT) dir += 1;
    if (in.pressed & A_LEFT) { tryMove(-1, 0); dasDir = -1; dasT = 0; }
    else if (in.pressed & A_RIGHT) { tryMove(+1, 0); dasDir = 1; dasT = 0; }
    else if (dir != 0 && dir == dasDir) {
        dasT++;
        if (dasT >= DAS && ((dasT - DAS) % ARR) == 0) tryMove(dir, 0);
    } else if (dir != 0) { dasDir = dir; dasT = 0; }
    else { dasDir = 0; dasT = 0; }

    if (in.pressed & A_HARD) {
        int d = 0;
        while (tryMove(0, 1)) ++d;
        if (mode != SM_PUZZLE) score += 2 * d;
        evLand++;
        lockPiece();
        return;
    }

    bool soft = (in.held & A_SOFT) != 0;
    float g = gravityPerFrame();
    if (soft) g = std::max(g, mode == SM_PUZZLE ? 0.34f : 0.5f);
    grav += g;
    while (grav >= 1.0f) {
        grav -= 1.0f;
        if (tryMove(0, 1)) { if (soft && mode != SM_PUZZLE) score += 1; }
        else { grav = 0; break; }
    }
    if (collides(type, rot, x, y + 1)) {
        if (mode == SM_PUZZLE) return;   // puzzles never lock on their own: press drop to place
        if (lockT == 0) evLand++;
        if (++lockT >= LOCK_FRAMES || lockResets >= LOCK_RESETS) lockPiece();
    } else {
        lockT = 0;
    }
}

// ------------------------------------------------------------------ puzzle solver
namespace {
const int PH = Solo::SH, PW = Solo::SW, SPAWN_Y = Solo::HIDDEN - 1;

struct Board { uint16_t r[PH]; };

bool fits(const Board& b, int t, int rot, int x, int y) {
    auto c = cells(t, rot);
    for (int i = 0; i < 4; ++i) {
        int cx = x + c[i][0], cy = y + c[i][1];
        if (cx < 0 || cx >= PW || cy >= PH) return false;
        if (cy >= 0 && (b.r[cy] & (1u << cx))) return false;
    }
    return true;
}

// hard drop from the spawn row; false if it can't be placed or ends up above the well
bool dropPlace(const Board& in, int t, int rot, int x, Board* out) {
    if (!fits(in, t, rot, x, SPAWN_Y)) return false;
    int y = SPAWN_Y;
    while (fits(in, t, rot, x, y + 1)) ++y;
    Board b = in;
    auto c = cells(t, rot);
    for (int i = 0; i < 4; ++i) {
        int cy = y + c[i][1];
        if (cy < 0) return false;
        b.r[cy] |= (uint16_t)(1u << (x + c[i][0]));
    }
    Board o{};
    int w = PH - 1;
    for (int r = PH - 1; r >= 0; --r) if (b.r[r] != 0x3FF) o.r[w--] = b.r[r];
    *out = o;
    return true;
}

uint64_t hashBoard(const Board& b, int idx, int hold) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(idx * 31 + hold + 7);
    for (int i = 0; i < PH; ++i) { h ^= b.r[i]; h *= 1099511628211ull; }
    return h;
}

struct Solver {
    const PuzzleDef& p;
    int cap, maxHeight;
    std::unordered_map<uint64_t, int> memo;
    std::vector<PuzzleMove> path, first;
    explicit Solver(const PuzzleDef& pd, int c) : p(pd), cap(c), maxHeight(pd.rows + 2) {}

    int run(const Board& b, int idx, int hold) {
        int filled = 0, rowsWith = 0, top = PH;
        for (int r = 0; r < PH; ++r) if (b.r[r]) { for (uint16_t v = b.r[r]; v; v &= (uint16_t)(v - 1)) ++filled; ++rowsWith; top = std::min(top, r); }
        if (!filled) { if (first.empty()) first = path; return 1; }
        int rem = (p.n - idx) + (hold >= 0 ? 1 : 0);
        if (rem == 0) return 0;
        if (PH - top > maxHeight) return 0;
        if (10 * rowsWith - filled > 4 * rem) return 0;
        if ((filled + 4 * rem) % 10 != 0) return 0;
        uint64_t key = hashBoard(b, idx, hold);
        auto it = memo.find(key);
        if (it != memo.end() && !(first.empty() && it->second > 0)) return it->second;
        int total = 0;
        struct Opt { int piece, nidx, nhold; bool hold; };
        Opt opts[2]; int no = 0;
        if (idx < p.n) {
            opts[no++] = {p.pieces[idx], idx + 1, hold, false};
            if (hold < 0) { if (idx + 1 < p.n) opts[no++] = {p.pieces[idx + 1], idx + 2, p.pieces[idx], true}; }
            else if (hold != p.pieces[idx]) opts[no++] = {hold, idx + 1, p.pieces[idx], true};
        } else if (hold >= 0) {
            opts[no++] = {hold, idx, -1, false};
        }
        for (int o = 0; o < no && total < cap; ++o) {
            std::unordered_set<uint64_t> seen;
            for (int rot = 0; rot < 4 && total < cap; ++rot)
                for (int x = -2; x < PW && total < cap; ++x) {
                    Board nb;
                    if (!dropPlace(b, opts[o].piece, rot, x, &nb)) continue;
                    uint64_t hb = hashBoard(nb, 0, 0);
                    if (!seen.insert(hb).second) continue;
                    path.push_back({opts[o].hold, (int8_t)rot, (int8_t)x});
                    total += run(nb, opts[o].nidx, opts[o].nhold);
                    path.pop_back();
                }
        }
        total = std::min(total, cap);
        memo[key] = total;
        return total;
    }
};

Board boardOf(const PuzzleDef& p) {
    Board b{};
    for (int r = 0; r < p.rows; ++r) b.r[PH - 1 - r] = p.cells[r];
    return b;
}
} // namespace

int solvePuzzle(const PuzzleDef& p, std::vector<PuzzleMove>* solution, int cap) {
    Solver s(p, cap);
    int n = s.run(boardOf(p), 0, -1);
    if (solution) *solution = s.first;
    return n;
}

void generatePuzzles(FILE* out, uint32_t seed) {
    std::mt19937 rng(seed);
    struct Cand { PuzzleDef p; int sol; };
    struct Tier { int rows, nMin, nMax, want; };
    const Tier tiers[] = {{2, 2, 4, 12}, {3, 3, 5, 14}, {4, 4, 6, 14}};
    fprintf(out, "// Generated by SlimePush --gen-puzzles %u. Every puzzle here was checked solvable by the solver.\n", seed);
    fprintf(out, "#include \"solo.hpp\"\nnamespace sp {\nconst PuzzleDef PUZZLES[] = {\n");
    int total = 0;
    for (const Tier& T : tiers) {
        std::vector<Cand> cands;
        std::unordered_set<uint64_t> uniq;
        int attempts = 0;
        while ((int)cands.size() < T.want * 5 && attempts < 400000) {
            ++attempts;
            int n = T.nMin + (int)(rng() % (unsigned)(T.nMax - T.nMin + 1));
            if (4 * n > 10 * T.rows - T.rows * 2) continue;
            // pack n random pieces into the bottom rectangle; their cells become the gaps
            uint16_t used[6] = {};
            PuzzleDef p{};
            p.rows = (uint8_t)T.rows; p.n = (uint8_t)n;
            bool ok = true;
            for (int k = 0; k < n && ok; ++k) {
                bool placed = false;
                for (int tries = 0; tries < 300 && !placed; ++tries) {
                    int t = 1 + (int)(rng() % 7), r = (int)(rng() % 4);
                    int px = -2 + (int)(rng() % 12), py = (int)(rng() % (unsigned)(T.rows + 3));
                    auto c = cells(t, r);
                    bool fitsHere = true;
                    for (int i = 0; i < 4 && fitsHere; ++i) {
                        int cx = px + c[i][0], cy = py - c[i][1];   // cy: 0 = bottom row going up
                        if (cx < 0 || cx >= PW || cy < 0 || cy >= T.rows || (used[cy] & (1u << cx))) fitsHere = false;
                    }
                    if (!fitsHere) continue;
                    for (int i = 0; i < 4; ++i) used[py - c[i][1]] |= (uint16_t)(1u << (px + c[i][0]));
                    p.pieces[k] = (uint8_t)t;
                    placed = true;
                }
                if (!placed) ok = false;
            }
            if (!ok) continue;
            for (int r = 0; r < T.rows; ++r) {
                p.cells[r] = (uint16_t)(0x3FF & ~used[r]);
                if (!used[r] || !p.cells[r]) ok = false;   // each row needs a gap and some slime
            }
            if (!ok) continue;
            for (int i = n - 1; i > 0; --i) { int j = (int)(rng() % (unsigned)(i + 1)); std::swap(p.pieces[i], p.pieces[j]); }
            uint64_t key = 0;
            for (int r = 0; r < T.rows; ++r) key = key * 1031 + p.cells[r];
            for (int i = 0; i < n; ++i) key = key * 8 + p.pieces[i];
            if (!uniq.insert(key).second) continue;
            int sol = solvePuzzle(p, nullptr, 500);
            if (sol < 1) continue;
            p.solutions = (uint16_t)sol;
            cands.push_back({p, sol});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.sol > b.sol; });
        for (int i = 0; i < T.want && !cands.empty(); ++i) {
            const Cand& c = cands[(size_t)((long)i * (long)(cands.size() - 1) / std::max(1, T.want - 1))];
            const PuzzleDef& p = c.p;
            fprintf(out, "    {%d, {", p.rows);
            for (int r = 0; r < 6; ++r) fprintf(out, "%s0x%03x", r ? ", " : "", p.cells[r]);
            fprintf(out, "}, %d, {", p.n);
            for (int k = 0; k < 7; ++k) fprintf(out, "%s%d", k ? ", " : "", k < p.n ? p.pieces[k] : 0);
            fprintf(out, "}, %d},\n", p.solutions);
            ++total;
        }
    }
    fprintf(out, "};\nconst int PUZZLE_COUNT = %d;\n} // namespace sp\n", total);
}

} // namespace sp

// ------------------------------------------------------------------ self-test
namespace sp {

// plays one placement through the real engine: optional hold, spin, slide, hard drop
static bool playMove(Solo& s, const PuzzleMove& m) {
    Input none;
    auto press = [&](uint16_t a) { Input in; in.held = a; in.pressed = a; s.step(in); s.step(none); };
    int guard = 0;
    while (!s.active && !s.over && guard++ < 200) s.step(none);
    if (s.over) return false;
    if (m.hold) press(A_HOLD);
    for (int i = 0; i < m.rot; ++i) press(A_CW);
    guard = 0;
    while (s.x != m.x && guard++ < 20) {
        int before = s.x;
        press(s.x < m.x ? A_RIGHT : A_LEFT);
        if (s.x == before) return false;
    }
    if (s.x != m.x || s.rot != m.rot) return false;
    press(A_HARD);
    return true;
}

// greedy placement for the marathon/mission checks: fewest holes, then lowest stack
PuzzleMove soloGreedyMove(const Solo& s) {
    PuzzleMove best{false, 0, 3};
    int bestScore = -1000000;
    for (int rot = 0; rot < 4; ++rot)
        for (int x = -2; x < Solo::SW; ++x) {
            if (s.collides(s.type, rot, x, s.y)) continue;
            int y = s.y;
            while (!s.collides(s.type, rot, x, y + 1)) ++y;
            bool occ[Solo::SH][Solo::SW];
            for (int r = 0; r < Solo::SH; ++r) for (int c = 0; c < Solo::SW; ++c) occ[r][c] = (s.grid[r][c].tf & 15) != 0;
            auto cl = cells(s.type, rot);
            for (int i = 0; i < 4; ++i) if (y + cl[i][1] >= 0) occ[y + cl[i][1]][x + cl[i][0]] = true;
            int linesDone = 0, holes = 0, height = 0, bump = 0, prevTop = -1;
            for (int r = 0; r < Solo::SH; ++r) { bool f = true; for (int c = 0; c < Solo::SW; ++c) f &= occ[r][c]; linesDone += f; }
            for (int c = 0; c < Solo::SW; ++c) {
                int top = Solo::SH;
                for (int r = 0; r < Solo::SH; ++r) if (occ[r][c]) { top = r; break; }
                for (int r = top + 1; r < Solo::SH; ++r) if (!occ[r][c]) ++holes;
                height = std::max(height, Solo::SH - top);
                if (prevTop >= 0) bump += std::abs(top - prevTop);
                prevTop = top;
            }
            int sc = linesDone * 80 - holes * 120 - height * 12 - bump * 6;
            if (sc > bestScore) { bestScore = sc; best = {false, (int8_t)rot, (int8_t)x}; }
        }
    return best;
}

int soloSelfTest() {
    int fails = 0;
    // every puzzle: the solver's answer has to clear the board in the real engine
    int solvedInPlay = 0;
    for (int i = 0; i < PUZZLE_COUNT; ++i) {
        std::vector<PuzzleMove> sol;
        int n = solvePuzzle(PUZZLES[i], &sol, 500);
        Solo s;
        s.start(SM_PUZZLE, 1, i);
        bool ok = n > 0 && !sol.empty();
        for (size_t k = 0; k < sol.size() && ok; ++k) ok = playMove(s, sol[k]);
        for (int f = 0; f < 5 && !s.over; ++f) s.step(Input{});
        ok = ok && s.over && s.won && s.boardEmpty();
        if (ok) ++solvedInPlay;
        else { printf("FAIL  puzzle %d could not be cleared in play (solutions %d, moves %zu)\n", i + 1, n, sol.size()); ++fails; }
    }
    printf("%s  %d / %d puzzles cleared in play using the solver's answer\n", solvedInPlay == PUZZLE_COUNT ? "ok  " : "FAIL", solvedInPlay, PUZZLE_COUNT);
    // a puzzle with the wrong answer must fail, not hang
    {
        Solo s; s.start(SM_PUZZLE, 1, 0);
        for (int k = 0; k < 10 && !s.over; ++k) playMove(s, {false, 0, 0});
        for (int f = 0; f < 300 && !s.over; ++f) s.step(Input{});
        bool ok = s.over && !s.won;
        printf("%s  a wrong answer ends the puzzle as failed\n", ok ? "ok  " : "FAIL"); fails += !ok;
    }
    // marathon: a simple placer should clear plenty of lines and the level should climb
    {
        Solo s; s.start(SM_MARATHON, 7);
        int pieces = 0;
        while (!s.over && pieces < 600) { if (!s.active) { s.step(Input{}); continue; } if (!playMove(s, soloGreedyMove(s))) break; ++pieces; }
        bool ok = s.lines >= 60 && s.level >= 7 && s.score > 0;
        printf("%s  marathon: %d pieces, %d lines, level %d, score %d%s\n", ok ? "ok  " : "FAIL", pieces, s.lines, s.level, s.score, s.over ? " (topped out)" : "");
        fails += !ok;
    }
    // mission: missions get completed, time-outs add garbage, the game keeps going
    {
        Solo s; s.start(SM_MISSION, 11);
        int pieces = 0, frames = 0;
        while (!s.over && frames < 60 * 60 * 6) {
            if (!s.active) { s.step(Input{}); ++frames; continue; }
            if (!playMove(s, soloGreedyMove(s))) break;
            ++pieces; frames += 30;
            for (int w = 0; w < 30 && !s.over; ++w) s.step(Input{});   // think time, so timers run
        }
        bool ok = s.missionsDone >= 3 && s.evMissionFail >= 1;
        printf("%s  mission: %d done, %d timed out, mission level %d, score %d%s\n", ok ? "ok  " : "FAIL", s.missionsDone, s.evMissionFail, s.missionLevel, s.score, s.over ? " (topped out)" : "");
        fails += !ok;
    }
    return fails;
}

} // namespace sp
