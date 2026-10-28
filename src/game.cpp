#include "game.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace sp {

// ---------------------------------------------------------------- geometry
// types: 1 I, 2 O, 3 T, 4 S, 5 Z, 6 J, 7 L
static int8_t SH[8][4][4][2];
static bool shInit = false;

static void initShapes() {
    static const int8_t base[8][4][2] = {
        {{0,0},{0,0},{0,0},{0,0}},
        {{0,1},{1,1},{2,1},{3,1}},   // I
        {{1,0},{2,0},{1,1},{2,1}},   // O
        {{1,0},{0,1},{1,1},{2,1}},   // T
        {{1,0},{2,0},{0,1},{1,1}},   // S
        {{0,0},{1,0},{1,1},{2,1}},   // Z
        {{0,0},{0,1},{1,1},{2,1}},   // J
        {{2,0},{0,1},{1,1},{2,1}},   // L
    };
    for (int t = 1; t <= 7; ++t) {
        int n = (t == 1) ? 4 : 3;
        for (int i = 0; i < 4; ++i) { SH[t][0][i][0] = base[t][i][0]; SH[t][0][i][1] = base[t][i][1]; }
        for (int r = 1; r < 4; ++r)
            for (int i = 0; i < 4; ++i) {
                int x = SH[t][r-1][i][0], y = SH[t][r-1][i][1];
                if (t == 2) { SH[t][r][i][0] = x; SH[t][r][i][1] = y; }
                else { SH[t][r][i][0] = (int8_t)(n - 1 - y); SH[t][r][i][1] = (int8_t)x; }
            }
    }
    shInit = true;
}

const int8_t (*cells(int type, int rot))[2] {
    if (!shInit) initShapes();
    if (type < 1 || type > 7) type = 1;
    return SH[type][rot & 3];
}

int faceIndex(int type) {
    static const int f[8] = {0, 1, 0, 2, 3, 2, 2, 2};
    return f[type & 7];
}

// SRS kicks, (x, y-up)
static const int8_t KJ[8][5][2] = {
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}, // 0->R
    {{0,0},{1,0},{1,-1},{0,2},{1,2}},     // R->0
    {{0,0},{1,0},{1,-1},{0,2},{1,2}},     // R->2
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}, // 2->R
    {{0,0},{1,0},{1,1},{0,-2},{1,-2}},    // 2->L
    {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},  // L->2
    {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},  // L->0
    {{0,0},{1,0},{1,1},{0,-2},{1,-2}},    // 0->L
};
static const int8_t KI[8][5][2] = {
    {{0,0},{-2,0},{1,0},{-2,-1},{1,2}},
    {{0,0},{2,0},{-1,0},{2,1},{-1,-2}},
    {{0,0},{-1,0},{2,0},{-1,2},{2,-1}},
    {{0,0},{1,0},{-2,0},{1,-2},{-2,1}},
    {{0,0},{2,0},{-1,0},{2,1},{-1,-2}},
    {{0,0},{-2,0},{1,0},{-2,-1},{1,2}},
    {{0,0},{1,0},{-2,0},{1,-2},{-2,1}},
    {{0,0},{-1,0},{2,0},{-1,2},{2,-1}},
};
static int kickRow(int from, int to) {
    static const int8_t tbl[4][4] = {{-1, 0, -1, 7}, {1, -1, 2, -1}, {-1, 3, -1, 4}, {6, -1, 5, -1}};
    return tbl[from & 3][to & 3];
}

// ---------------------------------------------------------------- tuning
static const int DAS = 10, ARR = 2, LOCK_FRAMES = 30, LOCK_RESETS = 15;
static float framesPerRow(int level) { return std::max(1.0f, 14.0f * std::pow(0.82f, (float)(level - 1))); }

// ---------------------------------------------------------------- game
uint8_t Game::newId() { if (++idCounter == 0) idCounter = 1; return idCounter; }

void Game::nextRound(uint32_t seed) {
    uint8_t wins0 = s.setWins[0], wins1 = s.setWins[1], target = s.setTarget, rnd = s.round;
    int st0 = s.pl[0].startLevel, st1 = s.pl[1].startLevel;
    if (s.setOver) { wins0 = wins1 = 0; rnd = 0; }
    uint32_t frame = s.frame;
    reset(seed, st0, st1, target);
    s.setWins[0] = wins0; s.setWins[1] = wins1; s.round = (uint8_t)(rnd + 1);
    s.frame = frame;
}

void Game::reset(uint32_t seed, int start0, int start1, int setTarget) {
    if (!shInit) initShapes();
    s = State{};
    s.setTarget = (uint8_t)std::max(1, std::min(3, setTarget));
    s.round = 1;
    s.phase = PH_COUNTDOWN;
    s.phaseTimer = 180;
    s.winner = -1;
    s.level = 1;
    s.center = H / 2;
    playFrames = 0;
    idCounter = 0;
    // the two starter blocks near the centre
    s.grid[H/2 - 1][4].tf = ROCK | 16; s.grid[H/2 - 1][4].id = newId();
    s.grid[H/2][5].tf = ROCK | 16;     s.grid[H/2][5].id = newId();
    for (int p = 0; p < 2; ++p) {
        pv[p] = Priv{};
        pv[p].rng.seed(seed);            // same seed: both players get the same piece order
        pv[p].bagPos = 7;
        for (int i = 0; i < NEXT_SHOWN; ++i) pv[p].queue[i] = nextFromBag(p);
        PlayerView& v = s.pl[p];
        v.type = 0; v.active = 0; v.hold = -1; v.alive = 1;
        v.startLevel = (uint8_t)std::max(1, std::min(9, p == 0 ? start0 : start1));
        v.level = v.startLevel;
        for (int i = 0; i < NEXT_SHOWN; ++i) v.next[i] = pv[p].queue[i];
    }
}

uint8_t Game::nextFromBag(int p) {
    Priv& q = pv[p];
    if (q.bagPos >= 7) {
        for (int i = 0; i < 7; ++i) q.bag[i] = (uint8_t)(i + 1);
        for (int i = 6; i > 0; --i) { int j = (int)(q.rng() % (unsigned)(i + 1)); std::swap(q.bag[i], q.bag[j]); }
        q.bagPos = 0;
    }
    return q.bag[q.bagPos++];
}

uint8_t Game::takeNext(int p) {
    Priv& q = pv[p];
    uint8_t t = q.queue[0];
    for (int i = 0; i < NEXT_SHOWN - 1; ++i) q.queue[i] = q.queue[i + 1];
    q.queue[NEXT_SHOWN - 1] = nextFromBag(p);
    for (int i = 0; i < NEXT_SHOWN; ++i) s.pl[p].next[i] = q.queue[i];
    return t;
}

bool Game::collides(int p, int type, int rot, int x, int y) const {
    auto c = cells(type, rot);
    for (int i = 0; i < 4; ++i) {
        int cx = x + c[i][0], ly = y + c[i][1];
        if (cx < 0 || cx >= W) return true;
        if (ly < 0 || ly >= H) continue;          // behind own end / past the far end: open air
        if (s.grid[toGlobal(p, ly)][cx].tf & 15) return true;
    }
    return false;
}

bool Game::offFar(int p, int type, int rot, int x, int y) const {
    (void)p;
    auto c = cells(type, rot);
    for (int i = 0; i < 4; ++i) if (y + c[i][1] >= H) return true;
    (void)x;
    return false;
}

int Game::dropDistance(int p, bool* fallsOff) const {
    const PlayerView& v = s.pl[p];
    int d = 0;
    *fallsOff = false;
    while (true) {
        if (collides(p, v.type, v.rot, v.x, v.y + d + 1)) break;
        ++d;
        if (offFar(p, v.type, v.rot, v.x, v.y + d)) { *fallsOff = true; break; }
    }
    return d;
}

bool Game::spawn(int p, int type) {
    PlayerView& v = s.pl[p];
    v.type = (int8_t)type; v.rot = 0; v.x = 3;
    int y0 = (type == 1) ? -1 : 0;
    v.active = 1;
    v.pieceId = newId();
    pv[p].grav = 0; pv[p].lockT = 0; pv[p].lockResets = 0; pv[p].lowestY = -99;
    for (int dy = 0; dy <= 2; ++dy) {
        if (!collides(p, type, 0, 3, y0 - dy)) { v.y = (int8_t)(y0 - dy); v.evSpawn++; return true; }
    }
    v.y = (int8_t)y0;
    v.active = 0;
    v.alive = 0;           // blocked at your own end
    return false;
}

void Game::groundedReset(int p) {
    PlayerView& v = s.pl[p];
    if (collides(p, v.type, v.rot, v.x, v.y + 1) && pv[p].lockResets < LOCK_RESETS) {
        pv[p].lockT = 0; pv[p].lockResets++;
    }
}

bool Game::tryMove(int p, int dx, int dy) {
    PlayerView& v = s.pl[p];
    if (collides(p, v.type, v.rot, v.x + dx, v.y + dy)) return false;
    v.x = (int8_t)(v.x + dx); v.y = (int8_t)(v.y + dy);
    if (dy > 0) {
        if (offFar(p, v.type, v.rot, v.x, v.y)) { vanish(p); return true; }
        if (v.y > pv[p].lowestY) { pv[p].lowestY = v.y; pv[p].lockResets = 0; }
    }
    if (dx) { v.evMove++; groundedReset(p); }
    return true;
}

bool Game::tryRotate(int p, int dir) {
    PlayerView& v = s.pl[p];
    if (v.type == 2) { v.evRotate++; return true; }
    int to = (v.rot + (dir > 0 ? 1 : 3)) & 3;
    int row = kickRow(v.rot, to);
    const int8_t (*k)[2] = (v.type == 1) ? KI[row] : KJ[row];
    for (int i = 0; i < 5; ++i) {
        int nx = v.x + k[i][0], ny = v.y - k[i][1];
        if (!collides(p, v.type, to, nx, ny)) {
            v.x = (int8_t)nx; v.y = (int8_t)ny; v.rot = (int8_t)to;
            v.evRotate++;
            if (offFar(p, v.type, v.rot, v.x, v.y)) { vanish(p); return true; }
            groundedReset(p);
            return true;
        }
    }
    return false;
}

void Game::vanish(int p) {
    PlayerView& v = s.pl[p];
    v.active = 0;
    v.evVanish++;
    v.holdUsed = 0;
    spawn(p, takeNext(p));
}

void Game::lock(int p) {
    PlayerView& v = s.pl[p];
    auto c = cells(v.type, v.rot);
    bool lost = false;
    int fi = faceIndex(v.type);
    for (int i = 0; i < 4; ++i) {
        int cx = v.x + c[i][0], ly = v.y + c[i][1];
        if (ly < 0) { lost = true; continue; }   // placed off your own end
        if (ly >= H) continue;
        Cell& cell = s.grid[toGlobal(p, ly)][cx];
        cell.tf = (uint8_t)(v.type | (i == fi ? 16 : 0));
        cell.id = v.pieceId;
    }
    v.active = 0;
    v.evLock++;
    v.holdUsed = 0;
    clearLines(p);
    resolveOverlap(1 - p);
    if (lost) v.alive = 0;
    if (v.alive) spawn(p, takeNext(p));
}

void Game::clearLines(int p) {
    PlayerView& v = s.pl[p];
    bool full[H];
    int n = 0;
    for (int r = 0; r < H; ++r) {
        full[r] = true;
        for (int x = 0; x < W; ++x) if (!(s.grid[r][x].tf & 15)) { full[r] = false; break; }
        if (full[r]) {
            if (n < 4) {
                v.lastClearRows[n] = (int8_t)r;
                for (int x = 0; x < W; ++x) v.lastClearTypes[n][x] = (uint8_t)s.grid[r][x].type();
            }
            ++n;
        }
    }
    if (!n) return;
    Cell tmp[H][W];
    std::memset(tmp, 0, sizeof(tmp));
    // rows on the clearer's side collapse toward the opponent
    if (p == 0) {
        int w = H - 1;
        for (int r = H - 1; r >= 0; --r) if (!full[r]) { std::memcpy(tmp[w], s.grid[r], sizeof(tmp[w])); --w; }
    } else {
        int w = 0;
        for (int r = 0; r < H; ++r) if (!full[r]) { std::memcpy(tmp[w], s.grid[r], sizeof(tmp[w])); ++w; }
    }
    std::memcpy(s.grid, tmp, sizeof(tmp));
    v.lastClearN = (uint8_t)std::min(n, 4);
    v.evClear++;
    static const int pts[5] = {0, 100, 300, 500, 800};
    v.score += pts[std::min(n, 4)] * v.level;
    v.lines = (int16_t)(v.lines + n);
    int k = pushRows(n);
    if (k > 0) push(p, k);
}

void Game::push(int p, int k) {
    Cell tmp[H][W];
    std::memset(tmp, 0, sizeof(tmp));
    bool off = false;
    for (int r = 0; r < H; ++r) {
        int nr = (p == 0) ? r + k : r - k;
        bool any = false;
        for (int x = 0; x < W; ++x) if (s.grid[r][x].tf & 15) any = true;
        if (nr < 0 || nr >= H) { if (any) off = true; continue; }
        std::memcpy(tmp[nr], s.grid[r], sizeof(tmp[nr]));
    }
    std::memcpy(s.grid, tmp, sizeof(tmp));
    s.center = (int8_t)(s.center + (p == 0 ? k : -k));
    s.center = (int8_t)std::max(0, std::min(H, (int)s.center));
    PlayerView& v = s.pl[p];
    v.evPush++; v.lastPush = (uint8_t)k;
    if (off) s.pl[1 - p].alive = 0;    // pushed off their end
    resolveOverlap(1 - p);
}

void Game::resolveOverlap(int q) {
    PlayerView& v = s.pl[q];
    if (!v.active) return;
    while (collides(q, v.type, v.rot, v.x, v.y)) {
        v.y--;
        if (v.y < -4) { v.alive = 0; v.active = 0; return; }
    }
}

void Game::forfeit(int p) {
    if (s.phase == PH_OVER) return;
    s.pl[p].alive = 0; s.pl[p].active = 0;
    s.reason = 2;
    checkEnd();
}

void Game::disconnect(int p) {
    if (s.phase == PH_OVER) return;
    s.pl[p].alive = 0; s.pl[p].active = 0;
    s.reason = 1;
    s.phase = PH_PLAY;
    checkEnd();
}

void Game::checkEnd() {
    bool a0 = s.pl[0].alive, a1 = s.pl[1].alive;
    if (a0 && a1) return;
    s.phase = PH_OVER;
    s.phaseTimer = 0;
    s.winner = (int8_t)((!a0 && !a1) ? 2 : (a0 ? 0 : 1));
    if (s.winner == 0 || s.winner == 1) s.setWins[s.winner]++;
    s.setOver = (s.setWins[0] >= s.setTarget || s.setWins[1] >= s.setTarget || s.reason != 0) ? 1 : 0;
    s.pl[0].active = s.pl[1].active = 0;
    s.pl[0].ready = s.pl[1].ready = 0;
}

void Game::step(const Input in[2]) {
    s.frame++;
    if (s.phase == PH_COUNTDOWN) {
        if (s.phaseTimer > 0) s.phaseTimer--;
        if (s.phaseTimer == 0) {
            s.phase = PH_PLAY;
            for (int p = 0; p < 2; ++p) spawn(p, takeNext(p));
            checkEnd();
        }
        return;
    }
    if (s.phase == PH_OVER) {
        if (s.phaseTimer < 60000) s.phaseTimer++;
        return;
    }
    playFrames++;
    for (int p = 0; p < 2; ++p)
        s.pl[p].level = (uint8_t)std::min(15u, s.pl[p].startLevel + playFrames / (60u * 30u));
    s.level = std::max(s.pl[0].level, s.pl[1].level);

    for (int p = 0; p < 2; ++p) {
        if (in[p].pressed & A_FORFEIT) { forfeit(p); return; }
    }

    for (int p = 0; p < 2; ++p) {
        PlayerView& v = s.pl[p];
        Priv& q = pv[p];
        if (!v.alive || !v.active) continue;
        const Input& I = in[p];

        if ((I.pressed & A_HOLD) && !v.holdUsed) {
            int cur = v.type;
            v.holdUsed = 1; v.evHold++;
            if (v.hold < 0) { v.hold = (int8_t)cur; v.active = 0; spawn(p, takeNext(p)); }
            else { int h = v.hold; v.hold = (int8_t)cur; spawn(p, h); }
            v.holdUsed = 1;
            if (!v.active) continue;
        }
        if (I.pressed & A_CW)  tryRotate(p, +1);
        if (!v.active) continue;
        if (I.pressed & A_CCW) tryRotate(p, -1);
        if (!v.active) continue;

        // horizontal with DAS
        int dir = 0;
        if (I.held & A_LEFT) dir -= 1;
        if (I.held & A_RIGHT) dir += 1;
        if (I.pressed & A_LEFT)  { tryMove(p, -1, 0); q.dasDir = -1; q.dasT = 0; }
        else if (I.pressed & A_RIGHT) { tryMove(p, +1, 0); q.dasDir = 1; q.dasT = 0; }
        else if (dir != 0 && dir == q.dasDir) {
            q.dasT++;
            if (q.dasT >= DAS && ((q.dasT - DAS) % ARR) == 0) tryMove(p, dir, 0);
        } else if (dir != 0) { q.dasDir = dir; q.dasT = 0; }
        else { q.dasDir = 0; q.dasT = 0; }
        if (!v.active) continue;

        if (I.pressed & A_HARD) {
            int d = 0;
            while (v.active && tryMove(p, 0, 1)) { ++d; if (!v.active) break; }
            if (v.active) { v.score += 2 * d; v.evLand++; lock(p); }
            continue;
        }

        // gravity
        float g = 1.0f / framesPerRow(v.level);
        bool soft = (I.held & A_SOFT) != 0;
        if (soft) g = std::max(g, 0.5f);
        q.grav += g;
        while (q.grav >= 1.0f && v.active) {
            q.grav -= 1.0f;
            if (tryMove(p, 0, 1)) { if (soft && v.active) v.score += 1; }
            else { q.grav = 0; break; }
        }
        if (!v.active) continue;

        if (collides(p, v.type, v.rot, v.x, v.y + 1)) {
            if (q.lockT == 0) v.evLand++;
            q.lockT++;
            if (q.lockT >= LOCK_FRAMES || q.lockResets >= LOCK_RESETS) lock(p);
        } else {
            q.lockT = 0;
        }
    }
    checkEnd();
}

// ---------------------------------------------------------------- bot
static int evalPlacement(const Game& g, int p, int type, int rot, int x, int y0, bool* ok, bool strategic, int* linesOut) {
    *linesOut = 0;
    *ok = false;
    if (g.collides(p, type, rot, x, y0)) return -1000000;
    int y = y0;
    auto c = cells(type, rot);
    while (true) {
        if (g.collides(p, type, rot, x, y + 1)) break;
        ++y;
        for (int i = 0; i < 4; ++i) if (y + c[i][1] >= H) return -1000000;   // falls through
    }
    // local copy
    bool b[H][W];
    for (int ly = 0; ly < H; ++ly)
        for (int cx = 0; cx < W; ++cx)
            b[ly][cx] = (g.s.grid[toGlobal(p, ly)][cx].tf & 15) != 0;
    int minLy = H;
    for (int i = 0; i < 4; ++i) {
        int ly = y + c[i][1];
        if (ly < 0) return -900000;
        b[ly][x + c[i][0]] = true;
        minLy = std::min(minLy, ly);
    }
    int lines = 0;
    bool full[H];
    for (int ly = 0; ly < H; ++ly) {
        full[ly] = true;
        for (int cx = 0; cx < W; ++cx) if (!b[ly][cx]) { full[ly] = false; break; }
        if (full[ly]) ++lines;
    }
    *linesOut = lines;
    if (lines) {
        bool nb[H][W];
        std::memset(nb, 0, sizeof(nb));
        int w = H - 1;
        for (int ly = H - 1; ly >= 0; --ly) if (!full[ly]) { std::memcpy(nb[w], b[ly], sizeof(nb[w])); --w; }
        std::memcpy(b, nb, sizeof(b));
        int k = Game::pushRows(lines);
        if (k) { // shift everything away from us
            std::memset(nb, 0, sizeof(nb));
            for (int ly = 0; ly + k < H; ++ly) std::memcpy(nb[ly + k], b[ly], sizeof(nb[0]));
            std::memcpy(b, nb, sizeof(b));
        }
    }
    int top[W], holes = 0, agg = 0, bump = 0, danger = H;
    for (int cx = 0; cx < W; ++cx) {
        top[cx] = H; int bot = -1;
        for (int ly = 0; ly < H; ++ly) if (b[ly][cx]) { if (top[cx] == H) top[cx] = ly; bot = ly; }
        if (top[cx] < H) {
            for (int ly = top[cx]; ly <= bot; ++ly) if (!b[ly][cx]) ++holes;
            agg += H - top[cx];
            danger = std::min(danger, top[cx]);
        }
    }
    int empties = 0;
    for (int cx = 0; cx < W; ++cx) {
        if (top[cx] == H) { ++empties; continue; }
        if (cx + 1 < W && top[cx + 1] < H) bump += std::abs(top[cx] - top[cx + 1]);
    }
    static const int lineBonus[5] = {0, 120, 600, 1100, 2000};
    // Push strategy: singles don't push, so save them up while it's safe and reward
    // rows that are one cell short with that cell reachable (a ready-made multi-clear)
    static const int pushBonus[5] = {0, -260, 1500, 2800, 4600};
    bool safe = danger >= 12;
    int lb = (strategic && (safe || lines >= 2)) ? pushBonus[std::min(lines, 4)] : lineBonus[std::min(lines, 4)];
    int ready = 0;
    if (strategic && safe) {
        int wellCol = -1;
        for (int ly = H - 1; ly >= 0; --ly) {
            int filled = 0, gap = -1;
            for (int cx = 0; cx < W; ++cx) { if (b[ly][cx]) ++filled; else gap = cx; }
            if (filled != W - 1) continue;
            bool open = true;
            for (int k = 0; k < ly && open; ++k) if (b[k][gap]) open = false;
            if (!open) continue;
            if (wellCol < 0) wellCol = gap;
            if (gap == wellCol) ++ready;
        }
        ready = std::min(ready, 4);
    }
    int score = lb + ready * 170
              - 40 * agg / W - 420 * holes - 35 * bump - 30 * (H - minLy) - 160 * empties;
    if (danger < 8) score -= (8 - danger) * 400;
    *ok = true;
    return score;
}

// how each difficulty plays (times in frames at 60 fps)
struct Persona {
    int thinkMin, thinkMax;   // pause after a new piece appears
    int tapMin, tapMax;       // gap between button taps
    int settleMin, settleMax; // pause once lined up, before dropping
    int hardPct;              // chance to slam it down rather than soft drop / let it fall
    int softPct;              // if not slamming: chance to hold soft drop (else let gravity do it)
    int noise;                // random jitter added to each placement's score (bigger = sloppier)
    int fumblePct;            // overshoot by a column and come back
    int keepFumblePct;        // ...and sometimes just leave it there
    bool strategic;           // saves up for pushing multi-clears instead of taking every single
};
static const Persona PERSONAS[4] = {
    // easy: slow and wobbly, never slams
    {34, 64, 12, 18, 10, 22,  0, 60, 1100, 18, 45, false},
    // normal: steady club player
    {16, 30,  6, 10,  5, 12, 75, 85, 160,  9,  0, true},
    // hard: quick and sharp, but still a human-ish hand speed
    { 6, 12,  4,  6,  2,  5, 100, 100,  60,  3,  0, true},
    // instant: the old machine-gun CPU (used by self-tests / autoplay)
    { 2,  2,  2,  2,  0,  0, 100, 0,    0,  0,  0, false},
};

void Bot::plan(const Game& g, int p) {
    const Persona& P = PERSONAS[std::max(0, std::min(3, level))];
    const PlayerView& v = g.s.pl[p];
    int bestScore = -2000000000, bestRot = v.rot, bestX = v.x, bestLines = 0;
    std::normal_distribution<float> jitter(0.0f, (float)std::max(1, P.noise));
    for (int r = 0; r < 4; ++r)
        for (int x = -3; x < W; ++x) {
            bool ok;
            int lines = 0;
            int sc = evalPlacement(g, p, v.type, r, x, std::max<int>(v.y, 0), &ok, P.strategic, &lines);
            if (!ok) continue;
            if (P.noise) sc += (int)jitter(rng);
            if (sc > bestScore) { bestScore = sc; bestRot = r; bestX = x; bestLines = lines; }
        }
    tRot = bestRot;
    planLines = bestLines;
    // route: optionally overshoot by one column first, like a slightly-too-long press
    nWay = 0; way = 0;
    int dir = bestX > v.x ? 1 : bestX < v.x ? -1 : 0;
    if (dir != 0 && chance(P.fumblePct)) {
        waypoints[nWay++] = bestX + dir;
        if (!chance(P.keepFumblePct)) waypoints[nWay++] = bestX;
    } else {
        waypoints[nWay++] = bestX;
    }
    stuck = 0;
}

Input Bot::think(const Game& g, int p) {
    const Persona& P = PERSONAS[std::max(0, std::min(3, level))];
    Input in;
    const PlayerView& v = g.s.pl[p];
    if (g.s.phase == PH_OVER) {
        // wander over to the rematch button after a moment, like a person would
        if (timer > 0) { --timer; return in; }
        in.pressed = in.held = A_READY;
        return in;
    }
    if (g.s.phase != PH_PLAY) { timer = range(40, 90); planLines = 0; return in; }
    if (!v.active) { softHeld = false; planLines = 0; return in; }

    if (v.evSpawn != lastSpawn) {
        lastSpawn = v.evSpawn;
        softHeld = false;
        plan(g, p);
        stage = THINK;
        timer = range(P.thinkMin, P.thinkMax);
        lastX = v.x; lastRot = v.rot;
    }
    if (softHeld) in.held = A_SOFT;
    if (timer > 0) { --timer; return in; }

    auto blocked = [&]() {
        bool same = (v.x == lastX && v.rot == lastRot);
        lastX = v.x; lastRot = v.rot;
        stuck = same ? stuck + 1 : 0;
        return stuck >= 3;
    };

    switch (stage) {
    case THINK:
        stage = MOVE;
        // fall through
    case MOVE: {
        if (blocked()) { stage = SETTLE; timer = 0; break; }   // wall or stack in the way: drop where it is
        if (v.rot != tRot) {
            int diff = (tRot - v.rot) & 3;
            in.pressed = (diff == 3) ? A_CCW : A_CW;
            timer = range(P.tapMin, P.tapMax) + (P.tapMin > 4 ? 3 : 0);   // spins take a touch longer
            break;
        }
        int target = waypoints[way];
        if (v.x < target) { in.pressed = A_RIGHT; in.held |= A_RIGHT; timer = range(P.tapMin, P.tapMax); break; }
        if (v.x > target) { in.pressed = A_LEFT; in.held |= A_LEFT; timer = range(P.tapMin, P.tapMax); break; }
        if (way + 1 < nWay) { ++way; timer = range(P.tapMin, P.tapMax) + 4; break; }   // "oops", go back
        stage = SETTLE;
        timer = range(P.settleMin, P.settleMax);
        break;
    }
    case SETTLE:
        if (chance(P.hardPct)) { in.pressed = A_HARD; stage = DROP_WAIT; }
        else if (chance(P.softPct)) { softHeld = true; in.held |= A_SOFT; stage = DROP_SOFT; }
        else stage = DROP_WAIT;     // just let gravity bring it down
        break;
    case DROP_SOFT:
    case DROP_WAIT:
        break;
    }
    return in;
}

} // namespace sp
