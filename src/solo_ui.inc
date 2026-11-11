// Single-player screens: the Solo menu, puzzle picker, the well (Marathon / Mission / Puzzle) and Catch.
// Included inside struct App in main.cpp, so it can use the app's drawing helpers directly.

    // ---------------------------------------------------------------- state
    sp::Solo solo;
    sp::Catch ctch;
    int soloMode = sp::SM_MARATHON;
    int soloSel = 0, puzzleSel = 0;
    bool soloPaused = false;
    int soloPauseSel = 0;
    bool soloRecorded = false, soloNewBest = false;
    int soloExcite = 0;
    struct SoloSnap { int lock, clear, move, rot, hold, land, ok, fail, lvl, garb; bool over; } sSnap{};
    struct CatchSnap { int caught, miss, hit, square, boom, move, rot, lvl, fuseSec; bool over; } cSnap{};

    static constexpr float SCELL = 32;                       // cell size in the solo well
    static constexpr float SWX = (SW - 10 * SCELL) / 2, SWY = 72;
    static constexpr float CCELL = 32;                       // cell size in Catch
    static constexpr float CWX = (SW - 14 * CCELL) / 2, CWY = 72;

    // ---------------------------------------------------------------- small helpers
    std::vector<std::string> wrapText(const std::string& text, float size, float maxW) {
        std::vector<std::string> out;
        std::string line, word;
        auto flush = [&]() {
            if (word.empty()) return;
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && g.measure(trial.c_str(), size, F_BODY) > maxW) { out.push_back(line); line = word; }
            else line = trial;
            word.clear();
        };
        for (char ch : text) { if (ch == ' ') flush(); else word += ch; }
        flush();
        if (!line.empty()) out.push_back(line);
        return out;
    }

    void frameBox(float x0, float y0, float w, float h) {
        const Theme& T = th();
        g.rrect(x0 - 18, y0 - 18 + 6, w + 36, h + 36, 24, withA(ink(), 60));
        g.rrect(x0 - 18, y0 - 18, w + 36, h + 36, 24, border());
        g.rrect(x0 - 12, y0 - 12, w + 24, h + 24, 18, T.shell);
        g.rrect(x0 - 5, y0 - 5, w + 10, h + 10, 8, border());
        g.rect(x0, y0, w, h, T.field);
    }

    void drawParts(float sx, float sy) {
        for (auto& p : parts) {
            float a = 1.0f - p.life / p.max;
            g.circle(p.x + sx, p.y + sy, p.r + 1.2f, withA(ink(), (uint8_t)(200 * a)));
            g.circle(p.x + sx, p.y + sy, p.r, withA(p.c, (uint8_t)(255 * a)));
        }
    }

    void drawPopups(float maxW) {
        for (auto& f : texts) {
            if (f.t < 0) continue;
            float a = f.t < 1.0f ? 1.0f : 1.0f - (f.t - 1.0f) / 0.3f;
            float pop = f.t < 0.12f ? 0.6f + f.t / 0.12f * 0.5f : 1.0f + 0.1f * std::max(0.0f, 0.25f - f.t);
            float size = f.scale * 8 * pop;
            float fw = g.measure(f.s.c_str(), size, F_DISPLAY);
            if (fw > maxW) size *= maxW / fw;
            TextStyle st; st.font = F_DISPLAY; st.fill = withA(f.c, (uint8_t)(255 * a));
            st.outline = withA(ink(), (uint8_t)(255 * a)); st.outlineW = size * 0.2f; st.shadow = size * 0.14f;
            g.print(f.x, f.y, size, st, f.s.c_str(), AL_CENTER);
        }
    }

    // a stat label with a big outlined number under it
    void statLine(float x, float y, const char* label, const char* val, float maxW) {
        body(x, y, 12, th().muted, label);
        float sz = 22, mw = g.measure(val, sz, F_DISPLAY);
        if (mw > maxW) sz *= maxW / mw;
        head(x, y + 20, sz, Col{255, 255, 255, 255}, val, AL_LEFT, F_DISPLAY);
    }

    void ribbon(float x, float y, float w, const char* text, Col fill) {
        g.rrect(x, y, w, 44, 22, border());
        g.rrect(x + 5, y + 5, w - 10, 34, 17, fill);
        g.rrect(x + 20, y + 9, w - 40, 8, 4, Col{255, 255, 255, 80});
        head(x + w / 2, y + 15, 15, Col{255, 255, 255, 255}, text, AL_CENTER, F_BODY);
    }

    // input for one player: keyboard plus every pad
    Input soloInput() {
        Input in = kbInput(false);
        for (auto& d : pads) in = mergeInput(in, padInput(d, false));
        return in;
    }

    // the menu, pause and results card shared by the solo screens
    void modeCard(const char* title, Col tc, const std::vector<std::string>& lines, const char* const* items, int n, int selIdx) {
        float w = std::max(460.0f, g.measure(title, 38, F_DISPLAY) + 120);
        for (auto& l : lines) w = std::max(w, g.measure(l.c_str(), 16, F_BODY) + 80);
        float h = 150 + lines.size() * 30 + n * 64, x = SW / 2 - w / 2, y = SH / 2 - h / 2;
        g.rect(0, 0, SW, SH, withA(th().bg, 160));
        panelBox(x, y, w, h);
        head(SW / 2, y + 26, 38, tc, title);
        float ly = y + 92;
        for (auto& l : lines) { body(SW / 2, ly, 16, th().text, l.c_str(), AL_CENTER); ly += 30; }
        for (int i = 0; i < n; ++i) button(SW / 2, ly + 12 + i * 64, 280, items[i], i == selIdx);
    }

    int menuChoice(int n) {   // up/down/A in a small card menu; returns the picked index or -1
        uint16_t e = menuPressed();
        if (e & R_UP) { soloPauseSel = (soloPauseSel + n - 1) % n; au.play(S_MOVE); }
        if (e & R_DOWN) { soloPauseSel = (soloPauseSel + 1) % n; au.play(S_MOVE); }
        if (e & R_A) { au.play(S_SELECT); return soloPauseSel; }
        return -1;
    }

    // ---------------------------------------------------------------- bests
    int puzzlesCleared() const { int n = 0; for (int i = 0; i < sp::PUZZLE_COUNT; ++i) if (cfg.puzzles & (1ull << i)) ++n; return n; }

    void recordSoloResult() {
        if (soloRecorded) return;
        soloRecorded = true;
        soloNewBest = false;
        if (soloMode == sp::SM_MARATHON && solo.score > cfg.bestMarathon) { cfg.bestMarathon = solo.score; soloNewBest = solo.score > 0; }
        if (soloMode == sp::SM_MISSION && solo.score > cfg.bestMission) { cfg.bestMission = solo.score; cfg.bestMissionCount = solo.missionsDone; soloNewBest = solo.score > 0; }
        if (soloMode == sp::SM_PUZZLE && solo.won) cfg.puzzles |= 1ull << solo.puzzle;
        cfg.save();
    }
    void recordCatchResult() {
        if (soloRecorded) return;
        soloRecorded = true;
        soloNewBest = ctch.score > cfg.bestCatch && ctch.score > 0;
        if (soloNewBest) cfg.bestCatch = ctch.score;
        cfg.save();
    }

    // ---------------------------------------------------------------- starting
    void startSolo(int m, int puzzleIndex = -1) {
        soloMode = m;
        solo.start(m, rng(), puzzleIndex);
        sSnap = SoloSnap{};
        mode = M_CPU; viewer = 0;
        soloPaused = false; soloRecorded = false; soloNewBest = false; soloPauseSel = 0;
        parts.clear(); texts.clear();
        pickBattleTrack();
        sc = SC_SOLO;
    }
    void startCatch() {
        ctch.start(rng());
        cSnap = CatchSnap{};
        cSnap.fuseSec = -1;
        mode = M_CPU; viewer = 0;
        soloPaused = false; soloRecorded = false; soloNewBest = false; soloPauseSel = 0;
        parts.clear(); texts.clear();
        pickBattleTrack();
        sc = SC_CATCH;
    }

    // ---------------------------------------------------------------- Solo menu
    static constexpr int SOLO_N = 5;
    void tickSoloMenu() {
        uint16_t e = menuPressed();
        if (e & R_UP) { soloSel = (soloSel + SOLO_N - 1) % SOLO_N; au.play(S_MOVE); }
        if (e & R_DOWN) { soloSel = (soloSel + 1) % SOLO_N; au.play(S_MOVE); }
        if (e & (R_A | R_START)) {
            au.play(S_SELECT);
            switch (soloSel) {
                case 0: startSolo(sp::SM_MARATHON); break;
                case 1: startSolo(sp::SM_MISSION); break;
                case 2: {
                    int first = 0;   // land on the first puzzle not yet cleared
                    while (first < sp::PUZZLE_COUNT - 1 && (cfg.puzzles & (1ull << first))) ++first;
                    puzzleSel = first; sc = SC_PUZZLES; break;
                }
                case 3: startCatch(); break;
                default: sc = SC_TITLE; sel = 2; break;
            }
        }
        if (e & (R_B | R_BACK)) { au.play(S_BACK); sc = SC_TITLE; sel = 2; }
    }

    void drawSoloMenu() {
        const Theme& T = th();
        drawBackground();
        banner("SOLO", pieceColor(7));
        static const char* items[SOLO_N] = {"MARATHON", "MISSION", "PUZZLE", "CATCH", "BACK"};
        for (int i = 0; i < SOLO_N; ++i) button(330, 170 + i * 82, 320, items[i], i == soloSel);
        if (soloSel >= 4) { prompts({{"A", "SELECT"}, {"B", "BACK"}}); return; }
        // info panel
        float px = 560, py = 150, pw = 640, ph = 470;
        panelBox(px, py, pw, ph);
        static const Col cols[4] = {pieceColor(1), pieceColor(3), pieceColor(4), pieceColor(5)};
        ribbon(px + 24, py + 22, 260, items[soloSel], cols[soloSel]);
        static const char* desc[4] = {
            "THE CLASSIC. CLEAR LINES AND KEEP GOING. EVERY 10 LINES THE SLIME FALLS FASTER. REACH 200 LINES TO BEAT IT.",
            "A NEW TASK EVERY FEW SECONDS, LIKE CLEAR 2 LINES WITH THE T SLIME. RUN OUT OF TIME AND SLIME BUBBLES UP FROM BELOW. EVERY 5 MISSIONS IT SPEEDS UP.",
            "40 SLIME PUZZLES. USE THE PIECES YOU ARE GIVEN TO CLEAR EVERY LAST BLOB. NO RUSH: NOTHING FALLS UNTIL YOU DROP IT.",
            "STEER YOUR SLIME AROUND AND CATCH THE FALLING PIECES. BUILD SOLID 4 X 4 SQUARES: THEY GO OFF AFTER 10 SECONDS, OR PRESS DROP TO SET THEM OFF EARLY. DODGE THE SPIKY URCHINS!"};
        float y = py + 90;
        for (auto& l : wrapText(desc[soloSel], 16, pw - 60)) { body(px + 30, y, 16, T.text, l.c_str()); y += 30; }
        y += 16;
        char best[96];
        if (soloSel == 0) snprintf(best, sizeof(best), "BEST SCORE  %d", cfg.bestMarathon);
        else if (soloSel == 1) snprintf(best, sizeof(best), "BEST SCORE  %d   (%d MISSIONS)", cfg.bestMission, cfg.bestMissionCount);
        else if (soloSel == 2) snprintf(best, sizeof(best), "CLEARED  %d / %d", puzzlesCleared(), sp::PUZZLE_COUNT);
        else snprintf(best, sizeof(best), "BEST SCORE  %d", cfg.bestCatch);
        head(px + 30, y, 20, cols[soloSel], best, AL_LEFT, F_BODY);
        // a happy slime in the corner
        float hop = std::fabs(std::sin((float)t * 3));
        g.ellipse(px + pw - 110, py + ph - 24, 70, 10, withA(ink(), 50));
        g.slime(px + pw - 110, py + ph - 30 - hop * 26, 150, cols[soloSel], ink(), (1 - hop) * 0.35f, blinkFor(soloSel + 20, 0), 0, (float)t);
        prompts({{"A", "PLAY"}, {"B", "BACK"}});
    }

    // ---------------------------------------------------------------- puzzle picker
    static constexpr int PZ_COLS = 8;
    void tickPuzzles() {
        uint16_t e = menuPressed();
        int n = sp::PUZZLE_COUNT;
        if (e & R_LEFT) { puzzleSel = (puzzleSel + n - 1) % n; au.play(S_MOVE); }
        if (e & R_RIGHT) { puzzleSel = (puzzleSel + 1) % n; au.play(S_MOVE); }
        if (e & R_UP) { puzzleSel = (puzzleSel - PZ_COLS + n) % n; au.play(S_MOVE); }
        if (e & R_DOWN) { puzzleSel = (puzzleSel + PZ_COLS) % n; au.play(S_MOVE); }
        if (e & (R_A | R_START)) { au.play(S_SELECT); startSolo(sp::SM_PUZZLE, puzzleSel); }
        if (e & (R_B | R_BACK)) { au.play(S_BACK); sc = SC_SOLO_MENU; soloSel = 2; }
    }

    int puzzleStars(int i) const {
        int s = sp::PUZZLES[i].solutions;
        return s >= 20 ? 1 : s >= 5 ? 2 : s >= 2 ? 3 : 4;
    }

    void drawPuzzles() {
        const Theme& T = th();
        drawBackground();
        banner("PUZZLES", pieceColor(4));
        float tw = 100, thh = 70, gap = 16, gx = SW / 2 - (PZ_COLS * tw + (PZ_COLS - 1) * gap) / 2, gy = 132;
        for (int i = 0; i < sp::PUZZLE_COUNT; ++i) {
            float x = gx + (i % PZ_COLS) * (tw + gap), y = gy + (i / PZ_COLS) * (thh + 14);
            bool on = i == puzzleSel, done = (cfg.puzzles >> i) & 1;
            float lift = on ? -3 - std::fabs(std::sin((float)t * 5)) * 2 : 0;
            g.rrect(x - 5, y - 5 + 7, tw + 10, thh + 10, 20, withA(ink(), 55));
            g.rrect(x - 5, y - 5 + lift, tw + 10, thh + 10, 20, border());
            g.rrect(x, y + lift, tw, thh, 16, on ? T.pop : done ? hex(0x46b98a) : T.panel);
            char num[16]; snprintf(num, sizeof(num), "%d", i + 1);
            if (on || done) head(x + tw / 2, y + 14 + lift, 24, Col{255, 255, 255, 255}, num, AL_CENTER, F_DISPLAY);
            else body(x + tw / 2, y + 14 + lift, 24, T.text, num, AL_CENTER);
            int st = puzzleStars(i);
            for (int k = 0; k < 4; ++k)
                g.circle(x + tw / 2 - 21 + k * 14, y + thh - 13 + lift, 4.5f, k < st ? (on || done ? Col{255, 255, 255, 230} : T.muted) : withA(T.muted, on || done ? 90 : 60));
        }
        // preview of the selected puzzle
        const sp::PuzzleDef& p = sp::PUZZLES[puzzleSel];
        float bx = 200, by = 572, bw = 880, bh = 140;
        panelBox(bx, by, bw, bh);
        float cs = 22, ox = bx + 28, oy = by + bh / 2 + 2 * cs;
        g.rrect(ox - 8, oy - 4 * cs - 8, 10 * cs + 16, 4 * cs + 16, 12, border());
        g.rrect(ox - 4, oy - 4 * cs - 4, 10 * cs + 8, 4 * cs + 8, 9, T.field);
        static BlobMap m;
        memset(&m, 0, sizeof(m)); m.w = 10; m.h = 4;
        for (int r = 0; r < p.rows; ++r)
            for (int c = 0; c < 10; ++c)
                if (p.cells[r] & (1u << c)) { m.t[3 - r][c] = ROCK; m.id[3 - r][c] = (uint8_t)(r + 1); }
        drawBlobs(m, ox, oy - 4 * cs, cs, 255, false);
        float px = ox + 10 * cs + 44;
        char info[96];
        snprintf(info, sizeof(info), "PUZZLE %d", puzzleSel + 1);
        head(px, by + 24, 22, Col{255, 255, 255, 255}, info, AL_LEFT, F_DISPLAY);
        static const char* diff[5] = {"", "EASY", "TRICKY", "HARD", "ONLY ONE WAY"};
        snprintf(info, sizeof(info), "%s%s", diff[puzzleStars(puzzleSel)], (cfg.puzzles >> puzzleSel) & 1 ? "   -   CLEARED" : "");
        body(px, by + 62, 15, T.muted, info);
        body(px, by + 96, 15, T.text, "YOUR SLIMES");
        for (int k = 0; k < p.n; ++k) drawMini(p.pieces[k], px + 172 + k * 66, by + 103, 13);
        prompts({{"A", "PLAY"}, {"B", "BACK"}});
    }

    // ---------------------------------------------------------------- the well
    void soloEffects() {
        const sp::Solo& s = solo;
        float cx = SWX + 5 * SCELL;
        auto rowY = [&](int r) { return SWY + (r - sp::Solo::HIDDEN) * SCELL; };
        if (s.evMove != sSnap.move) au.play(S_MOVE, 0.7f);
        if (s.evRotate != sSnap.rot) au.play(S_ROTATE, 0.8f);
        if (s.evLand != sSnap.land) au.play(S_LAND, 0.7f);
        if (s.evHold != sSnap.hold) au.play(S_HOLD, 0.8f);
        if (s.evLock != sSnap.lock) { au.play(S_LOCK); squashV[0] += 2.5f; rumble(0, 0x2000, 0x1000, 35); }
        if (s.evClear != sSnap.clear) {
            int n = s.lastClearN;
            au.play((Sfx)(S_CLEAR1 + std::max(0, std::min(3, n - 1))));
            for (int i = 0; i < std::min(n, 4); ++i)
                for (int x = 0; x < 10; ++x)
                    spawnSplash(SWX + x * SCELL + SCELL / 2, rowY(s.lastClearRows[i]) + SCELL / 2, pieceColor(s.lastClearTypes[i][x]), 3, 220);
            if (n >= 2 || s.lastTSpin) texts.push_back({s.lastClearName, cx, rowY(s.lastClearRows[0]) - 10, 0, Col{255, 255, 255, 255}, 3.4f});
            squashV[0] += 3; soloExcite = 70;
            rumble(0, 0x3000 + n * 0x1800, 0x2000 + n * 0x1000, 70 + n * 30);
        }
        if (s.evMissionOk != sSnap.ok) {
            au.play(S_JOIN);
            texts.push_back({"MISSION CLEAR!", cx, SWY + 220, -0.25f, th().pop, 4});
            soloExcite = 90;
        }
        if (s.evMissionFail != sSnap.fail) {
            au.play(S_PUSHED);
            shake = std::max(shake, 12.0f);
            texts.push_back({"TIME UP!", cx, SWY + 260, 0, th().danger, 4});
            rumble(0, 0xA000, 0x6000, 260);
        } else if (s.evGarbage != sSnap.garb) {
            shake = std::max(shake, 8.0f);
        }
        if (s.evLevel != sSnap.lvl) {
            au.play(S_GO);
            char b[24]; snprintf(b, sizeof(b), "LEVEL %d", s.level);
            texts.push_back({b, cx, SWY + 300, -0.5f, pieceColor(1), 4});
        }
        if (s.over && !sSnap.over) {
            au.play(s.won ? S_WIN : S_LOSE);
            if (!s.won) rumble(0, 0xFFFF, 0xA000, 500);
            recordSoloResult();
        }
        sSnap = {s.evLock, s.evClear, s.evMove, s.evRotate, s.evHold, s.evLand, s.evMissionOk, s.evMissionFail, s.evLevel, s.evGarbage, s.over};
    }

    // demo player for screenshots and testing (--autoplay)
    Input soloDemo() {
        Input in;
        if (!solo.active) return in;
        static int lastSpawn = -1, wait = 0;
        static sp::PuzzleMove target;
        if (solo.evSpawn != lastSpawn) { lastSpawn = solo.evSpawn; target = sp::soloGreedyMove(solo); wait = 8; }
        if (wait > 0) { --wait; return in; }
        wait = 3;
        if (solo.rot != target.rot) in.pressed = A_CW;
        else if (solo.x < target.x) in.pressed = A_RIGHT;
        else if (solo.x > target.x) in.pressed = A_LEFT;
        else in.pressed = A_HARD;
        in.held = in.pressed;
        return in;
    }

    void debugForce(bool* over, int* overTimer) {
        if (opt.force.empty() || opt.shotAt <= 0 || now < opt.shotAt - 0.8) return;
        if (opt.force == "pause") { soloPaused = true; soloPauseSel = 1; }
        else if (opt.force == "over") { *over = true; *overTimer = 120; }
        opt.force.clear();
    }

    void tickSolo() {
        debugForce(&solo.over, &solo.overTimer);
        if (solo.over && !soloRecorded) recordSoloResult();
        uint16_t e = allPressed();
        if (solo.over) {
            if (solo.overTimer < 50) { solo.step(Input{}); soloEffects(); return; }
            bool wonPuzzle = soloMode == sp::SM_PUZZLE && solo.won && solo.puzzle + 1 < sp::PUZZLE_COUNT;
            int n = 2;
            int pick = menuChoice(n);
            if (e & (R_B | R_BACK)) pick = 1;
            if (pick == 0) {
                if (wonPuzzle) startSolo(sp::SM_PUZZLE, solo.puzzle + 1);
                else startSolo(soloMode, solo.puzzle);
            } else if (pick == 1) {
                if (soloMode == sp::SM_PUZZLE) { puzzleSel = std::min(sp::PUZZLE_COUNT - 1, solo.puzzle + (solo.won ? 1 : 0)); sc = SC_PUZZLES; }
                else { sc = SC_SOLO_MENU; }
            }
            return;
        }
        if (soloPaused) {
            if (e & (R_START | R_B)) { soloPaused = false; au.play(S_BACK); return; }
            int pick = menuChoice(3);
            if (pick == 0) soloPaused = false;
            else if (pick == 1) startSolo(soloMode, solo.puzzle);
            else if (pick == 2) { if (soloMode == sp::SM_PUZZLE) { puzzleSel = solo.puzzle; sc = SC_PUZZLES; } else sc = SC_SOLO_MENU; }
            return;
        }
        if (e & R_START) { soloPaused = true; soloPauseSel = 0; au.play(S_SELECT); return; }
        if ((e & R_BACK) && soloMode == sp::SM_PUZZLE) { au.play(S_BACK); startSolo(sp::SM_PUZZLE, solo.puzzle); return; }
        Input in = opt.autoplay ? soloDemo() : soloInput();
        solo.step(in);
        if (soloExcite > 0) --soloExcite;
        soloEffects();
    }

    void drawSoloBlobs(float ox, float oy) {
        const sp::Solo& s = solo;
        const Theme& T = th();
        const int HID = sp::Solo::HIDDEN;
        static BlobMap m;
        memset(&m, 0, sizeof(m)); m.w = 10; m.h = 20;
        for (int r = HID; r < sp::Solo::SH; ++r)
            for (int c = 0; c < 10; ++c) {
                const Cell& cell = s.grid[r][c];
                if (!(cell.tf & 15)) continue;
                m.t[r - HID][c] = (uint8_t)cell.type(); m.id[r - HID][c] = cell.id; m.f[r - HID][c] = cell.face();
            }
        drawBlobs(m, ox, oy, SCELL, 255);
        if (!s.active) return;
        // ghost
        int gy = s.ghostY();
        auto cl = cells(s.type, s.rot);
        Col gc = pieceColor(s.type);
        if (gy != s.y)
            for (int i = 0; i < 4; ++i) {
                int r = gy + cl[i][1] - HID;
                if (r < 0) continue;
                float px = ox + (s.x + cl[i][0]) * SCELL, py = oy + r * SCELL;
                g.rrect(px + 3, py + 3, SCELL - 6, SCELL - 6, 9, withA(gc, 80));
                g.rrect(px + 7, py + 7, SCELL - 14, SCELL - 14, 5, T.field);
                g.rrect(px + 7, py + 7, SCELL - 14, SCELL - 14, 5, withA(gc, 45));
            }
        // the falling piece (its top row may peek out above the well)
        memset(&m, 0, sizeof(m)); m.w = 10; m.h = 21;
        int fi = faceIndex(s.type);
        for (int i = 0; i < 4; ++i) {
            int r = s.y + cl[i][1] - HID + 1, c = s.x + cl[i][0];
            if (r < 0 || r >= 21 || c < 0 || c >= 10) continue;
            m.t[r][c] = (uint8_t)s.type; m.id[r][c] = s.pieceId; m.f[r][c] = i == fi;
        }
        drawBlobs(m, ox, oy - SCELL, SCELL, 255);
    }

    void drawSolo() {
        const Theme& T = th();
        const sp::Solo& s = solo;
        Col white{255, 255, 255, 255};
        drawBackground();
        std::uniform_real_distribution<float> U(-1, 1);
        float sx = shake > 0.3f ? U(rng) * shake : 0, sy = shake > 0.3f ? U(rng) * shake : 0;
        float ox = SWX + sx, oy = SWY + sy, ww = 10 * SCELL, wh = 20 * SCELL;
        frameBox(ox, oy, ww, wh);
        // danger glow when the stack gets high
        int hgt = s.stackHeight();
        if (soloMode != sp::SM_PUZZLE && hgt > 15) {
            float a = (hgt - 15) / 5.0f * (0.5f + 0.5f * std::sin((float)t * 8));
            for (int i = 0; i < 4; ++i) g.rect(ox, oy + i * SCELL, ww, SCELL, withA(T.danger, (uint8_t)(a * 80 * (4 - i) / 4)));
        }
        drawSoloBlobs(ox, oy);
        drawParts(sx, sy);

        static const char* modeNames[3] = {"MARATHON", "MISSION", "PUZZLE"};
        static const Col modeCols[3] = {pieceColor(1), pieceColor(3), pieceColor(4)};
        char buf[96];
        // left panel: name, hold, stats
        const float PW = 250, lx = SWX - 44 - PW, top = SWY;
        float ph = 470;
        panelBox(lx, top, PW, ph);
        if (soloMode == sp::SM_PUZZLE) snprintf(buf, sizeof(buf), "PUZZLE %d", s.puzzle + 1);
        else snprintf(buf, sizeof(buf), "%s", modeNames[soloMode]);
        ribbon(lx + 8, top + 10, PW - 16, buf, modeCols[soloMode]);
        body(lx + 24, top + 70, 13, T.text, "HOLD");
        well(lx + 22, top + 92, 100, 78);
        if (s.hold > 0) drawMini(s.hold, lx + 72, top + 131, 19);
        float sxl = lx + 140, sy0 = top + 70;
        if (soloMode == sp::SM_PUZZLE) {
            int left = (int)s.queue.size() + (s.active ? 1 : 0) + (s.hold > 0 ? 1 : 0);
            snprintf(buf, sizeof(buf), "%d", left); statLine(sxl, sy0, "SLIMES", buf, 90);
            static const char* diff[5] = {"", "EASY", "TRICKY", "HARD", "ONE WAY"};
            body(lx + 24, top + 200, 12, T.muted, "DIFFICULTY");
            head(lx + 24, top + 220, 18, white, diff[puzzleStars(std::max(0, s.puzzle))], AL_LEFT, F_BODY);
            auto lines = wrapText("CLEAR EVERY BLOB. NOTHING FALLS UNTIL YOU DROP IT, SO TAKE YOUR TIME.", 14, PW - 48);
            float yy = top + 270;
            for (auto& l : lines) { body(lx + 24, yy, 14, T.text, l.c_str()); yy += 26; }
            body(lx + 24, top + ph - 40, 13, T.muted, "SELECT: START OVER");
        } else {
            snprintf(buf, sizeof(buf), "%d", s.level); statLine(sxl, sy0, "LEVEL", buf, 90);
            snprintf(buf, sizeof(buf), "%d", s.score); statLine(lx + 24, top + 200, "SCORE", buf, PW - 48);
            if (soloMode == sp::SM_MARATHON) {
                snprintf(buf, sizeof(buf), "%d / 200", s.lines); statLine(lx + 24, top + 264, "LINES", buf, PW - 48);
                snprintf(buf, sizeof(buf), "%d", std::max(cfg.bestMarathon, s.score)); statLine(lx + 24, top + 328, "BEST", buf, PW - 48);
            } else {
                snprintf(buf, sizeof(buf), "%d", s.missionsDone); statLine(lx + 24, top + 264, "MISSIONS", buf, PW - 48);
                snprintf(buf, sizeof(buf), "%d", std::max(cfg.bestMission, s.score)); statLine(lx + 24, top + 328, "BEST", buf, PW - 48);
            }
            // level progress bar
            float prog = soloMode == sp::SM_MARATHON ? (s.lines % 10) / 10.0f : (s.missionsDone % 5) / 5.0f;
            float by = top + ph - 56;
            body(lx + 24, by - 22, 12, T.muted, soloMode == sp::SM_MARATHON ? "NEXT LEVEL" : "NEXT LEVEL (5 MISSIONS)");
            g.rrect(lx + 20, by, PW - 40, 26, 13, border());
            g.rrect(lx + 24, by + 4, PW - 48, 18, 9, T.field);
            if (prog > 0) g.rrect(lx + 24, by + 4, std::max(18.0f, (PW - 48) * prog), 18, 9, modeCols[soloMode]);
        }
        // right side: next queue
        const float rx = SWX + ww + 44;
        float nh = soloMode == sp::SM_MISSION ? 290 : 380;
        panelBox(rx, top, PW, nh);
        body(rx + 24, top + 18, 13, T.text, "NEXT");
        int shown = std::min((int)s.queue.size(), soloMode == sp::SM_PUZZLE ? 6 : 5);
        if (soloMode == sp::SM_MISSION) shown = std::min(shown, 4);
        for (int i = 0; i < shown; ++i) {
            float cy = top + 76 + (i == 0 ? 0 : 20 + i * (soloMode == sp::SM_MISSION ? 52 : 56));
            drawMini(s.queue[(size_t)i], rx + PW / 2, cy, i == 0 ? 20 : 15);
        }
        if (shown > 1) g.rect(rx + 30, top + 118, PW - 60, 3, T.fieldDot);
        if (soloMode == sp::SM_PUZZLE && s.queue.empty()) body(rx + PW / 2, top + 80, 14, T.muted, "LAST ONE!", AL_CENTER);
        // mission card
        if (soloMode == sp::SM_MISSION && !s.over) {
            float my = top + nh + 26, mh = 230;
            Col mc = s.missionFlash > 0 ? hex(0x46b98a) : s.missionFlash < 0 ? T.danger : T.panel;
            g.rrect(rx - 7, my - 7, PW + 14, mh + 14, 26, border());
            g.rrect(rx, my, PW, mh, 19, mc);
            ribbon(rx + 8, my + 10, PW - 16, "MISSION", pieceColor(3));
            auto lines = wrapText(s.mission.text(), 17, PW - 40);
            float yy = my + 68;
            for (auto& l : lines) { head(rx + PW / 2, yy, 17, white, l.c_str(), AL_CENTER, F_BODY); yy += 28; }
            if (s.mission.kind == sp::MK_WITH_PIECE) { drawMini(s.mission.piece, rx + PW / 2, yy + 22, 15); yy += 46; }
            if (s.mission.kind == sp::MK_TOTAL) {
                snprintf(buf, sizeof(buf), "%d / %d", s.mission.progress, s.mission.n);
                body(rx + PW / 2, yy, 15, T.text, buf, AL_CENTER);
            }
            float frac = s.mission.timeMax ? (float)s.mission.timeLeft / s.mission.timeMax : 0;
            bool low = s.mission.timeLeft < 8 * 60;
            float by = my + mh - 36;
            g.rrect(rx + 16, by, PW - 32, 24, 12, border());
            g.rrect(rx + 20, by + 4, PW - 40, 16, 8, T.field);
            if (frac > 0) g.rrect(rx + 20, by + 4, std::max(16.0f, (PW - 40) * frac), 16, 8, low && (int)(t * 6) % 2 ? T.danger : T.pop);
            snprintf(buf, sizeof(buf), "%d", (s.mission.timeLeft + 59) / 60);
            head(rx + PW - 22, by - 24, 16, low ? T.danger : white, buf, AL_RIGHT, F_DISPLAY);
        }
        // mascot
        int mood = s.over ? (s.won ? 2 : 3) : soloExcite > 0 ? 2 : (hgt > 14 ? 1 : 0);
        float hop = s.over && s.won ? std::fabs(std::sin((float)t * 7)) * 26 : 0;
        float mxp = soloMode == sp::SM_MISSION ? lx + PW / 2 : rx + PW / 2;
        g.ellipse(mxp, SH - 30, 64, 10, withA(ink(), 60));
        g.slime(mxp, SH - 34 - hop, 140, modeCols[soloMode], ink(), squash[0] + std::sin((float)t * 3) * 0.05f, blinkFor(31, 0), mood, (float)t);
        prompts({{"START", "PAUSE"}}, 30);

        // ready / popups / overlays
        if (s.readyTimer > 0) {
            const char* r = s.readyTimer > 30 ? "READY?" : "GO!";
            head(SWX + ww / 2, SWY + wh / 2 - 40, 50, T.pop, r);
        }
        drawPopups(ww + 60);
        if (soloPaused) {
            const char* items[3] = {"RESUME", "RESTART", soloMode == sp::SM_PUZZLE ? "PICK A PUZZLE" : "QUIT"};
            modeCard("PAUSED", white, {}, items, 3, soloPauseSel);
        } else if (s.over && s.overTimer >= 50) {
            std::vector<std::string> lines;
            const char* title;
            Col tc = s.won ? T.pop : T.danger;
            const char* items[2] = {"TRY AGAIN", soloMode == sp::SM_PUZZLE ? "PICK A PUZZLE" : "MENU"};
            if (soloMode == sp::SM_PUZZLE) {
                title = s.won ? "CLEARED!" : "OUT OF SLIMES";
                if (s.won && s.puzzle + 1 < sp::PUZZLE_COUNT) items[0] = "NEXT PUZZLE";
                snprintf(buf, sizeof(buf), "%d OF %d PUZZLES CLEARED", puzzlesCleared(), sp::PUZZLE_COUNT); lines.push_back(buf);
                if (!s.won) lines.push_back("TIP: PRESS SELECT TO START A PUZZLE OVER");
            } else {
                title = s.won ? "YOU BEAT IT!" : soloNewBest ? "NEW BEST!" : "GAME OVER";
                if (soloNewBest) tc = T.pop;
                snprintf(buf, sizeof(buf), "SCORE  %d", s.score); lines.push_back(buf);
                if (soloMode == sp::SM_MARATHON) snprintf(buf, sizeof(buf), "%d LINES   LEVEL %d", s.lines, s.level);
                else snprintf(buf, sizeof(buf), "%d MISSIONS   LEVEL %d", s.missionsDone, s.level);
                lines.push_back(buf);
            }
            modeCard(title, tc, lines, items, 2, soloPauseSel);
        }
    }

    // ---------------------------------------------------------------- Catch
    void catchEffects() {
        const sp::Catch& c = ctch;
        float cx = CWX + (c.cx + 0.5f) * CCELL, cy = CWY + (c.cy + 0.5f) * CCELL;
        if (c.evMove != cSnap.move) au.play(S_MOVE, 0.5f);
        if (c.evRotate != cSnap.rot) au.play(S_ROTATE, 0.8f);
        if (c.evCatch != cSnap.caught) { au.play(S_LOCK); squashV[0] += 2; rumble(0, 0x1800, 0x1000, 30); }
        if (c.evMiss != cSnap.miss) au.play(S_VANISH, 0.6f);
        if (c.evHit != cSnap.hit) {
            au.play(S_PUSHED);
            shake = std::max(shake, 12.0f);
            float hx = CWX + (c.lastHitX + 0.5f) * CCELL, hy = CWY + (c.lastHitY + 0.5f) * CCELL;
            spawnSplash(hx, hy, hex(0x6a579e), 18, 300);
            texts.push_back({"OUCH!", hx, hy - 40, 0, th().danger, 3});
            rumble(0, 0xC000, 0x8000, 300);
        }
        if (c.evSquare != cSnap.square) {
            au.play(S_CLEAR2);
            texts.push_back({"SQUARE!", cx, cy - 90, 0, th().pop, 3});
            soloExcite = 60;
        }
        if (c.evBoom != cSnap.boom) {
            au.play(S_PUSH);
            shake = std::max(shake, 10.0f + c.lastBoomSquares * 3.0f);
            float bx = CWX + (c.lastBoomX + 0.5f) * CCELL, by = CWY + (c.lastBoomY + 0.5f) * CCELL;
            for (int i = 0; i < 26 + c.lastBoomCells * 2; ++i) spawnSplash(bx, by, pieceColor(1 + (int)(rng() % 7)), 1, 420);
            char b[48];
            snprintf(b, sizeof(b), c.lastBoomSquares > 1 ? "%dX BOOM! +%d" : "BOOM! +%d", c.lastBoomSquares > 1 ? c.lastBoomSquares : c.lastBoomScore, c.lastBoomScore);
            texts.push_back({b, bx, by - 30, 0, th().pop, c.lastBoomSquares > 1 ? 4.5f : 4.0f});
            soloExcite = 90;
            rumble(0, 0x8000 + c.lastBoomSquares * 0x1800, 0xA000, 250 + c.lastBoomSquares * 80);
        }
        if (c.evLevel != cSnap.lvl) {
            au.play(S_GO);
            char b[24]; snprintf(b, sizeof(b), "LEVEL %d", c.level);
            texts.push_back({b, CWX + 7 * CCELL, CWY + 200, -0.6f, pieceColor(1), 4});
        }
        int fuseSec = c.fuse >= 0 ? (c.fuse + 59) / 60 : -1;
        if (fuseSec != cSnap.fuseSec && fuseSec > 0 && fuseSec <= 3) au.play(S_TICK);
        if (c.over && !cSnap.over) { au.play(S_LOSE); rumble(0, 0xFFFF, 0xA000, 500); recordCatchResult(); }
        cSnap = {c.evCatch, c.evMiss, c.evHit, c.evSquare, c.evBoom, c.evMove, c.evRotate, c.evLevel, fuseSec, c.over};
    }

    void tickCatch() {
        debugForce(&ctch.over, &ctch.overTimer);
        if (ctch.over && !soloRecorded) recordCatchResult();
        uint16_t e = allPressed();
        if (ctch.over) {
            if (ctch.overTimer < 60) { ctch.step(Input{}, 0, 0); catchEffects(); return; }
            int pick = menuChoice(2);
            if (e & (R_B | R_BACK)) pick = 1;
            if (pick == 0) startCatch(); else if (pick == 1) { sc = SC_SOLO_MENU; soloSel = 3; }
            return;
        }
        if (soloPaused) {
            if (e & (R_START | R_B)) { soloPaused = false; au.play(S_BACK); return; }
            int pick = menuChoice(3);
            if (pick == 0) soloPaused = false; else if (pick == 1) startCatch(); else if (pick == 2) { sc = SC_SOLO_MENU; soloSel = 3; }
            return;
        }
        if (e & R_START) { soloPaused = true; soloPauseSel = 0; au.play(S_SELECT); return; }
        // directions come from the d-pad and stick in all four ways; spin and set-off use your button layout
        const uint16_t DIRS = R_UP | R_DOWN | R_LEFT | R_RIGHT;
        auto dirBits = [](uint16_t r) { return ((r & R_UP) ? 1 : 0) | ((r & R_DOWN) ? 2 : 0) | ((r & R_LEFT) ? 4 : 0) | ((r & R_RIGHT) ? 8 : 0); };
        int held = dirBits(allHeld()), pressed = dirBits(allPressed());
        Binds kbCatch = BINDS_KEYBOARD;
        kbCatch.m[B_HARD] = R_HARD;   // keyboard: Space sets squares off (Up just moves up)
        Input in = toActions((uint16_t)(kb.held & ~DIRS), (uint16_t)((kb.held & ~kb.prev) & ~DIRS), false, kbCatch);
        for (auto& d : pads) in = mergeInput(in, toActions((uint16_t)(d.held & ~DIRS), (uint16_t)((d.held & ~d.prev) & ~DIRS), false, cfg.binds));
        if (opt.autoplay) {   // demo: drift under the lowest piece, dodge urchins
            in = Input{}; held = 0; pressed = 0;
            const sp::Catch::Faller* tg = nullptr;
            for (auto& f : ctch.fallers) if (!f.urchin && (!tg || f.y > tg->y)) tg = &f;
            for (auto& f : ctch.fallers) if (f.urchin && std::abs(f.x - ctch.cx) <= 2 && f.y > ctch.cy - 8) { held |= f.x <= ctch.cx ? 8 : 4; tg = nullptr; }
            if (tg && (ctch.frames % 8) == 0) pressed |= tg->x + 1 < ctch.cx ? 4 : tg->x + 1 > ctch.cx ? 8 : 0;
        }
        ctch.step(in, held, pressed);
        if (soloExcite > 0) --soloExcite;
        catchEffects();
    }

    void drawUrchin(float x, float y, float s) {
        const Theme& T = th();
        Col body1 = hex(0x6a579e), spike = ink();
        for (int i = 0; i < 10; ++i) {
            float a = i * 0.6283f + (float)t * 1.5f;
            g.line(x, y, x + std::cos(a) * s * 0.62f, y + std::sin(a) * s * 0.62f, s * 0.16f, spike);
        }
        g.circle(x, y, s * 0.42f, spike);
        g.circle(x, y, s * 0.34f, body1);
        // angry eyes
        g.line(x - s * 0.2f, y - s * 0.12f, x - s * 0.05f, y - s * 0.04f, 3, T.slimeInk);
        g.line(x + s * 0.2f, y - s * 0.12f, x + s * 0.05f, y - s * 0.04f, 3, T.slimeInk);
        g.circle(x - s * 0.12f, y + s * 0.04f, s * 0.06f, Col{255, 255, 255, 255});
        g.circle(x + s * 0.12f, y + s * 0.04f, s * 0.06f, Col{255, 255, 255, 255});
    }

    void drawCatch() {
        const Theme& T = th();
        const sp::Catch& c = ctch;
        Col white{255, 255, 255, 255};
        drawBackground();
        std::uniform_real_distribution<float> U(-1, 1);
        float sx = shake > 0.3f ? U(rng) * shake : 0, sy = shake > 0.3f ? U(rng) * shake : 0;
        const int AW = sp::Catch::CW, AH = sp::Catch::CH;
        float ox = CWX + sx, oy = CWY + sy, ww = AW * CCELL, wh = AH * CCELL;
        frameBox(ox, oy, ww, wh);
        SDL_Rect clip{(int)ox, (int)oy, (int)ww, (int)wh};
        SDL_RenderSetClipRect(ren, &clip);
        // the blob
        static BlobMap m;
        memset(&m, 0, sizeof(m)); m.w = AW; m.h = AH;
        for (auto& cell : c.mass) {
            int x = c.cx + cell.dx, y = c.cy + cell.dy;
            if (x < 0 || x >= AW || y < 0 || y >= AH) continue;
            m.t[y][x] = cell.type; m.id[y][x] = cell.id; m.f[y][x] = cell.face;
        }
        drawBlobs(m, ox, oy, CCELL, 255);
        // the core gets a little crown so you know which bit you are steering
        float kx = ox + (c.cx + 0.5f) * CCELL, ky = oy + c.cy * CCELL;
        g.circle(kx, ky + 2, 7, border()); g.circle(kx, ky + 2, 4.5f, T.pop);
        // lit squares flash, faster as the fuse runs down
        if (c.fuse >= 0) {
            float rate = 3 + (1.0f - c.fuse / (float)sp::Catch::FUSE_FRAMES) * 14;
            uint8_t a = (uint8_t)(70 + 90 * (0.5f + 0.5f * std::sin((float)t * rate)));
            float fx = 0, fy = 0; int fn = 0;
            for (auto& cell : c.mass) if (cell.fused) {
                float px = ox + (c.cx + cell.dx) * CCELL, py = oy + (c.cy + cell.dy) * CCELL;
                g.rrect(px + 4, py + 4, CCELL - 8, CCELL - 8, 8, Col{255, 255, 255, a});
                fx += px; fy += py; ++fn;
            }
            if (fn) {
                char b[16]; snprintf(b, sizeof(b), "%d", (c.fuse + 59) / 60);
                head(fx / fn + CCELL / 2, fy / fn - 2, 34, T.pop, b);
            }
        }
        // falling pieces
        for (auto& f : c.fallers) {
            if (f.urchin) { drawUrchin(ox + (f.x + 0.5f) * CCELL, oy + (f.y + 0.5f) * CCELL, CCELL * 1.1f); continue; }
            static BlobMap fm;
            memset(&fm, 0, sizeof(fm)); fm.w = 4; fm.h = 4;
            pieceIntoMap(fm, f.type, f.rot, 0, 0, f.id);
            drawBlobs(fm, ox + f.x * CCELL, oy + f.y * CCELL, CCELL, 255);
        }
        for (auto& d : c.debris) {
            float px = ox + d.x * CCELL, py = oy + d.y * CCELL;
            g.rrect(px + 3, py + 3, CCELL - 6, CCELL - 6, 9, withA(ink(), 160));
            g.rrect(px + 6, py + 6, CCELL - 12, CCELL - 12, 7, withA(pieceColor(d.type), 200));
        }
        SDL_RenderSetClipRect(ren, nullptr);
        drawParts(sx, sy);

        char buf[64];
        const float PW = 250, lx = CWX - 44 - PW, top = CWY;
        float ph = 470;
        panelBox(lx, top, PW, ph);
        ribbon(lx + 8, top + 10, PW - 16, "CATCH", pieceColor(5));
        snprintf(buf, sizeof(buf), "%d", c.score); statLine(lx + 24, top + 70, "SCORE", buf, PW - 48);
        snprintf(buf, sizeof(buf), "%d", c.level); statLine(lx + 24, top + 134, "LEVEL", buf, 80);
        snprintf(buf, sizeof(buf), "%d", c.booms); statLine(lx + 130, top + 134, "BOOMS", buf, 90);
        snprintf(buf, sizeof(buf), "%d", std::max(cfg.bestCatch, c.score)); statLine(lx + 24, top + 198, "BEST", buf, PW - 48);
        // energy
        body(lx + 24, top + 274, 12, T.muted, "ENERGY");
        float ey = top + 296, ew = PW - 48, frac = c.energy / 100.0f;
        bool low = c.energy <= 30;
        g.rrect(lx + 20, ey, ew + 8, 34, 17, border());
        g.rrect(lx + 24, ey + 4, ew, 26, 13, T.field);
        if (frac > 0) g.rrect(lx + 24, ey + 4, std::max(26.0f, ew * frac), 26, 13, low && (int)(t * 5) % 2 ? T.danger : hex(0x46b98a));
        auto tips = wrapText("MISSED PIECES AND URCHINS COST ENERGY. EVERY BOOM GIVES SOME BACK.", 13, PW - 48);
        float yy = top + 352;
        for (auto& l : tips) { body(lx + 24, yy, 13, T.muted, l.c_str()); yy += 22; }
        if (c.fuse >= 0) {
            snprintf(buf, sizeof(buf), "BOOM IN %d", (c.fuse + 59) / 60);
            head(lx + PW / 2, top + ph - 34, 18, T.pop, buf, AL_CENTER, F_BODY);
        }
        // right: controls and the mascot
        const float rx = CWX + ww + 44;
        panelBox(rx, top, PW, 250);
        body(rx + 24, top + 20, 13, T.text, "CONTROLS");
        body(rx + 24, top + 56, 15, T.text, "MOVE");
        chip(rx + 112, top + 64, "D-PAD");
        body(rx + 24, top + 104, 15, T.text, "SPIN");
        chips(rx + 112, top + 112, (uint16_t)(cfg.binds.m[B_CW] | cfg.binds.m[B_CCW]), 6);
        body(rx + 24, top + 152, 15, T.text, "BOOM");
        chips(rx + 112, top + 160, (uint16_t)(cfg.binds.m[B_HARD] & ~(R_UP | R_DOWN | R_LEFT | R_RIGHT)), 6);
        body(rx + 24, top + 196, 13, T.muted, "KEYBOARD: ARROWS MOVE,");
        body(rx + 24, top + 216, 13, T.muted, "X / Z SPIN, SPACE BOOM");
        int mood = c.over ? 3 : soloExcite > 0 ? 2 : c.energy <= 30 ? 1 : 0;
        g.ellipse(rx + PW / 2, SH - 30, 64, 10, withA(ink(), 60));
        g.slime(rx + PW / 2, SH - 34, 150, pieceColor(5), ink(), squash[0] + std::sin((float)t * 3) * 0.05f, blinkFor(41, 0), mood, (float)t);
        prompts({{"START", "PAUSE"}}, 30);

        if (c.readyTimer > 0) head(CWX + ww / 2, CWY + 120, 50, T.pop, c.readyTimer > 30 ? "READY?" : "GO!");
        drawPopups(ww);
        if (soloPaused) {
            const char* items[3] = {"RESUME", "RESTART", "QUIT"};
            modeCard("PAUSED", white, {}, items, 3, soloPauseSel);
        } else if (c.over && c.overTimer >= 60) {
            std::vector<std::string> lines;
            snprintf(buf, sizeof(buf), "SCORE  %d", c.score); lines.push_back(buf);
            snprintf(buf, sizeof(buf), "%d BOOMS   %d SQUARES   LEVEL %d", c.booms, c.squaresTotal, c.level); lines.push_back(buf);
            const char* items[2] = {"TRY AGAIN", "MENU"};
            modeCard(soloNewBest ? "NEW BEST!" : "OUT OF ENERGY", soloNewBest ? T.pop : T.danger, lines, items, 2, soloPauseSel);
        }
    }
