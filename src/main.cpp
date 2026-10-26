// Slime Push - a slime-themed Push-mode versus block game for Steam Deck.
// Two Decks on the same LAN find each other automatically.
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include "game.hpp"
#include "net.hpp"
#include "gfx.hpp"
#include "audio.hpp"

using namespace sp;

static const int SW = 1280, SH = 800;
static const float C = 20.0f;                      // cell size
static const float FX = (SW - W * C) / 2, FY = 20; // field origin

// ------------------------------------------------------------------ config
struct Config {
    int theme = 0;       // 0 light, 1 dark
    bool sound = true;
    int cpu = 1;
    uint32_t hostId = 0;
    std::string path;
    void load() {
        std::string dir = net::configDir();
        path = dir + "/settings.txt";
        FILE* f = fopen(path.c_str(), "r");
        if (f) {
            char k[64]; unsigned v;
            while (fscanf(f, "%63s %u", k, &v) == 2) {
                if (!strcmp(k, "theme")) theme = (int)v & 1;
                else if (!strcmp(k, "sound")) sound = v != 0;
                else if (!strcmp(k, "cpu")) cpu = std::min(2, (int)v);
                else if (!strcmp(k, "id")) hostId = v;
            }
            fclose(f);
        }
        if (!hostId) { std::random_device rd; hostId = rd() | 1u; save(); }
    }
    void save() {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        fprintf(f, "theme %d\nsound %d\ncpu %d\nid %u\n", theme, sound ? 1 : 0, cpu, hostId);
        fclose(f);
    }
};

// ------------------------------------------------------------------ input
enum Raw : uint16_t {
    R_UP = 1, R_DOWN = 2, R_LEFT = 4, R_RIGHT = 8, R_A = 16, R_B = 32, R_X = 64, R_Y = 128,
    R_LB = 256, R_RB = 512, R_START = 1024, R_BACK = 2048, R_HARD = 4096
};
struct Device { SDL_GameController* gc = nullptr; SDL_JoystickID id = -1; uint16_t held = 0, prev = 0; };

static Input toActions(uint16_t held, uint16_t pressed, bool flipped) {
    auto map = [&](uint16_t r) {
        uint16_t a = 0;
        if (r & R_LEFT) a |= A_LEFT;
        if (r & R_RIGHT) a |= A_RIGHT;
        if (r & (flipped ? R_UP : R_DOWN)) a |= A_SOFT;
        if (r & ((flipped ? R_DOWN : R_UP) | R_HARD)) a |= A_HARD;
        uint16_t cw = A_CW, ccw = A_CCW;
        if (flipped) std::swap(cw, ccw);
        if (r & R_A) a |= cw;
        if (r & (R_B | R_X)) a |= ccw;
        if (r & (R_Y | R_LB | R_RB)) a |= A_HOLD;
        return a;
    };
    Input in;
    in.held = map(held);
    in.pressed = map(pressed);
    if (pressed & R_A) in.pressed |= A_READY;
    return in;
}

// ------------------------------------------------------------------ fx
struct Particle { float x, y, vx, vy, r, life, max; Col c; };
struct FloatText { std::string s; float x, y, t; Col c; float scale; };

enum Screen { SC_TITLE, SC_HOST, SC_JOIN, SC_SETTINGS, SC_HELP, SC_GAME };
enum Mode { M_CPU, M_LOCAL, M_HOST, M_CLIENT };

struct Options {
    bool host = false, cpu = false, autoplay = false, windowed = false, autojoin = false, find = false;
    std::string join;
    double exitAfter = 0;
    int selftest = 0;
    std::string shot; double shotAt = 0;
    int theme = -1;
};

struct App {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    Gfx g;
    Audio au;
    Config cfg;
    Options opt;
    std::mt19937 rng{std::random_device{}()};
    bool running = true;
    bool vsync = false;
    double now = 0, t = 0;

    std::vector<Device> pads;
    Device kb;

    Screen sc = SC_TITLE;
    int sel = 0;
    std::string toast; double toastUntil = 0;

    // match
    Mode mode = M_CPU;
    Game game;
    Bot bot[2];
    bool botFor[2] = {false, false};
    State view{}, prev{};
    bool haveView = false;
    int viewer = 0;
    bool paused = false, forfeitAsk = false, peerGone = false;
    int pauseSel = 0;
    uint32_t playStartFrame = 0;
    bool sawPlay = false;

    // net
    net::Udp gs;
    net::Finder finder;
    net::Addr peer{};
    bool peerOk = false;
    uint32_t nonce = 0, seq = 0, lastStateSeq = 0;
    double lastRecv = 0, lastBeacon = 0, joinStarted = 0, lastJoinSend = 0;
    bool joining = false;
    net::FoundHost joinTarget;
    net::InputSender insend;
    net::InputReceiver inrecv;
    std::vector<net::Addr> bcast;
    std::string myName, peerName;

    // fx
    std::vector<Particle> parts;
    std::vector<FloatText> texts;
    float shake = 0;
    float squash[2] = {0, 0}, squashV[2] = {0, 0};
    float excite[2] = {0, 0};

    const Theme& th() const { return cfg.theme ? themeDark() : themeLight(); }

    // ------------------------------------------------------------ setup
    void initName() {
        std::string n = net::computerName();
        if (n.empty()) n = "SLIME";
        if (n.size() > 12) n = n.substr(0, 12);
        static const char* A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint32_t h = cfg.hostId;
        std::string tag;
        for (int i = 0; i < 3; ++i) { tag += A[h % 32]; h /= 32; }
        myName = n + "-" + tag;
    }

    void say(const std::string& s, double secs = 3.0) { toast = s; toastUntil = now + secs; }

    // ------------------------------------------------------------ input
    void openPad(int index) {
        if (!SDL_IsGameController(index)) return;
        SDL_GameController* gc = SDL_GameControllerOpen(index);
        if (!gc) return;
        SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        for (auto& p : pads) if (p.id == id) { SDL_GameControllerClose(gc); return; }
        Device d; d.gc = gc; d.id = id;
        pads.push_back(d);
    }
    void closePad(SDL_JoystickID id) {
        for (size_t i = 0; i < pads.size(); ++i)
            if (pads[i].id == id) { SDL_GameControllerClose(pads[i].gc); pads.erase(pads.begin() + (long)i); return; }
    }
    void readDevices() {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        kb.prev = kb.held;
        uint16_t h = 0;
        if (k[SDL_SCANCODE_UP]) h |= R_UP;
        if (k[SDL_SCANCODE_DOWN]) h |= R_DOWN;
        if (k[SDL_SCANCODE_LEFT]) h |= R_LEFT;
        if (k[SDL_SCANCODE_RIGHT]) h |= R_RIGHT;
        if (k[SDL_SCANCODE_X]) h |= R_A;
        if (k[SDL_SCANCODE_Z]) h |= R_B;
        if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LSHIFT]) h |= R_LB;
        if (k[SDL_SCANCODE_SPACE]) h |= R_HARD;
        if (k[SDL_SCANCODE_RETURN]) h |= R_START;
        if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) h |= R_BACK;
        kb.held = h;
        for (auto& p : pads) {
            p.prev = p.held;
            uint16_t b = 0;
            auto B = [&](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(p.gc, x) != 0; };
            if (B(SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= R_UP;
            if (B(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= R_DOWN;
            if (B(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= R_LEFT;
            if (B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= R_RIGHT;
            if (B(SDL_CONTROLLER_BUTTON_A)) b |= R_A;
            if (B(SDL_CONTROLLER_BUTTON_B)) b |= R_B;
            if (B(SDL_CONTROLLER_BUTTON_X)) b |= R_X;
            if (B(SDL_CONTROLLER_BUTTON_Y)) b |= R_Y;
            if (B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= R_LB;
            if (B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= R_RB;
            if (B(SDL_CONTROLLER_BUTTON_START)) b |= R_START;
            if (B(SDL_CONTROLLER_BUTTON_BACK)) b |= R_BACK;
            int lx = SDL_GameControllerGetAxis(p.gc, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(p.gc, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx < -18000) b |= R_LEFT;
            if (lx > 18000) b |= R_RIGHT;
            if (ly < -22000) b |= R_UP;
            if (ly > 22000) b |= R_DOWN;
            if (SDL_GameControllerGetAxis(p.gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= R_LB;
            if (SDL_GameControllerGetAxis(p.gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= R_RB;
            p.held = b;
        }
    }
    uint16_t allHeld() const { uint16_t h = kb.held; for (auto& p : pads) h |= p.held; return h; }
    uint16_t allPressed() const {
        uint16_t e = (uint16_t)(kb.held & ~kb.prev);
        for (auto& p : pads) e |= (uint16_t)(p.held & ~p.prev);
        return e;
    }
    // menu repeat for held directions
    int repT = 0; uint16_t repDir = 0;
    uint16_t menuPressed() {
        uint16_t e = allPressed(), h = allHeld() & (R_UP | R_DOWN | R_LEFT | R_RIGHT);
        if (h && h == repDir) { if (++repT > 18 && (repT % 5) == 0) e |= h; }
        else { repDir = h; repT = 0; }
        return e;
    }

    // ------------------------------------------------------------ match control
    void startMatch(Mode m) {
        mode = m;
        game.reset(rng());
        botFor[0] = opt.autoplay;
        botFor[1] = (m == M_CPU);
        bot[0] = Bot(); bot[1] = Bot();
        bot[0].level = 2; bot[1].level = cfg.cpu;
        viewer = (m == M_CLIENT) ? 1 : 0;
        view = game.s; prev = view;
        haveView = (m != M_CLIENT);
        paused = forfeitAsk = peerGone = false;
        sawPlay = false;
        parts.clear(); texts.clear();
        inrecv.reset();
        sc = SC_GAME;
        lastRecv = now;
        lastStateSeq = 0;
    }

    void leaveMatch() {
        if ((mode == M_HOST || mode == M_CLIENT) && peerOk) {
            net::Bye b; net::fillHdr(b.h, net::P_BYE); b.nonce = nonce;
            for (int i = 0; i < 3; ++i) gs.send(&b, sizeof(b), peer);
        }
        gs.close(); peerOk = false;
        sc = SC_TITLE; sel = 0;
    }

    // ------------------------------------------------------------ hosting / joining
    bool openHostSocket() {
        for (uint16_t p = net::GAME_PORT; p < net::GAME_PORT + 10; ++p)
            if (gs.open(p, false, true)) return true;
        return false;
    }
    void sendBeacon(bool busy) {
        net::Beacon b{}; net::fillHdr(b.h, net::P_BEACON);
        strncpy(b.name, myName.c_str(), sizeof(b.name));
        b.port = gs.port; b.busy = busy ? 1 : 0; b.hostId = cfg.hostId;
        for (auto& a : bcast) gs.send(&b, sizeof(b), a);
    }
    void beginHost() {
        if (!openHostSocket()) { say("COULD NOT OPEN THE NETWORK PORT"); return; }
        bcast = net::broadcastTargets(net::DISC_PORT);
        lastBeacon = 0; peerOk = false;
        sc = SC_HOST;
    }
    void tickHostWait() {
        if (now - lastBeacon > 0.5) { bcast = net::broadcastTargets(net::DISC_PORT); sendBeacon(false); lastBeacon = now; }
        unsigned char buf[2048]; net::Addr from{}; int n;
        while ((n = gs.recv(buf, sizeof(buf), &from)) > 0) {
            net::PktType tp;
            if (!net::checkHdr(buf, n, &tp)) continue;
            if (tp == net::P_JOIN && n >= (int)sizeof(net::Join)) {
                const net::Join* j = (const net::Join*)buf;
                char nm[25]; memcpy(nm, j->name, 24); nm[24] = 0;
                peer = from; peerOk = true; nonce = j->nonce; peerName = nm;
                net::Welcome w{}; net::fillHdr(w.h, net::P_WELCOME);
                strncpy(w.name, myName.c_str(), sizeof(w.name)); w.nonce = nonce;
                gs.send(&w, sizeof(w), peer);
                au.play(S_JOIN);
                startMatch(M_HOST);
                return;
            }
        }
        if (allPressed() & (R_B | R_BACK)) { au.play(S_BACK); gs.close(); sc = SC_TITLE; }
    }
    void beginJoin() {
        if (!finder.start()) say("COULD NOT LISTEN FOR GAMES ON THIS NETWORK");
        joining = false; sel = 0;
        sc = SC_JOIN;
    }
    void connectTo(const net::Addr& a, const std::string& name) {
        if (!gs.open(0, false, false)) { say("COULD NOT OPEN THE NETWORK PORT"); return; }
        peer = a; peerOk = true; peerName = name;
        nonce = (uint32_t)rng() | 1u;
        joining = true; joinStarted = now; lastJoinSend = 0;
    }
    void tickJoin() {
        finder.poll(now, cfg.hostId);
        uint16_t e = menuPressed();
        if (joining) {
            if (now - lastJoinSend > 0.25) {
                net::Join j{}; net::fillHdr(j.h, net::P_JOIN);
                strncpy(j.name, myName.c_str(), sizeof(j.name)); j.nonce = nonce;
                gs.send(&j, sizeof(j), peer); lastJoinSend = now;
            }
            unsigned char buf[2048]; net::Addr from{}; int n;
            while ((n = gs.recv(buf, sizeof(buf), &from)) > 0) {
                net::PktType tp;
                if (!net::checkHdr(buf, n, &tp)) continue;
                if (tp == net::P_WELCOME && n >= (int)sizeof(net::Welcome) && ((const net::Welcome*)buf)->nonce == nonce) {
                    char nm[25]; memcpy(nm, ((const net::Welcome*)buf)->name, 24); nm[24] = 0;
                    peerName = nm; peer = from;
                    finder.stop();
                    au.play(S_JOIN);
                    startMatch(M_CLIENT);
                    return;
                }
                if (tp == net::P_FULL) { joining = false; gs.close(); say("THAT DECK IS ALREADY IN A GAME"); }
            }
            if (joining && now - joinStarted > 6) { joining = false; gs.close(); peerOk = false; say("NO ANSWER FROM THAT DECK. TRY AGAIN?"); }
            if (e & (R_B | R_BACK)) { joining = false; gs.close(); peerOk = false; au.play(S_BACK); }
            return;
        }
        int n = (int)finder.hosts.size();
        if (opt.autojoin && n && !finder.hosts[0].busy) { joinTarget = finder.hosts[0]; connectTo(joinTarget.addr, joinTarget.name); return; }
        if (n) {
            if (e & R_UP) { sel = (sel + n - 1) % n; au.play(S_MOVE); }
            if (e & R_DOWN) { sel = (sel + 1) % n; au.play(S_MOVE); }
            sel = std::min(sel, n - 1);
            if (e & (R_A | R_START)) {
                auto& h = finder.hosts[(size_t)sel];
                if (h.busy) say("THAT DECK IS ALREADY IN A GAME");
                else { au.play(S_SELECT); joinTarget = h; connectTo(h.addr, h.name); }
            }
        }
        if (e & (R_B | R_BACK)) { au.play(S_BACK); finder.stop(); sc = SC_TITLE; sel = 1; }
    }

    // ------------------------------------------------------------ game tick
    void hostNetPoll() {
        unsigned char buf[2048]; net::Addr from{}; int n;
        while ((n = gs.recv(buf, sizeof(buf), &from)) > 0) {
            net::PktType tp;
            if (!net::checkHdr(buf, n, &tp)) continue;
            bool fromPeer = from == peer;
            if (tp == net::P_JOIN && n >= (int)sizeof(net::Join)) {
                const net::Join* j = (const net::Join*)buf;
                if (fromPeer && j->nonce == nonce) {
                    net::Welcome w{}; net::fillHdr(w.h, net::P_WELCOME);
                    strncpy(w.name, myName.c_str(), sizeof(w.name)); w.nonce = nonce;
                    gs.send(&w, sizeof(w), peer);
                } else {
                    net::Hdr f; net::fillHdr(f, net::P_FULL); gs.send(&f, sizeof(f), from);
                }
            } else if (tp == net::P_INPUT && n >= (int)sizeof(net::InputPkt) && fromPeer) {
                const net::InputPkt* ip = (const net::InputPkt*)buf;
                if (ip->nonce == nonce) { inrecv.feed(*ip); lastRecv = now; }
            } else if (tp == net::P_BYE && fromPeer) {
                if (!peerGone) { peerGone = true; game.disconnect(1); say("YOUR OPPONENT LEFT"); }
            }
        }
        if (!peerGone && now - lastRecv > 4.0) { peerGone = true; game.disconnect(1); say("LOST CONTACT WITH YOUR OPPONENT"); }
        if (now - lastBeacon > 1.0) { sendBeacon(true); lastBeacon = now; }
    }

    void clientNetPoll() {
        unsigned char buf[2048]; net::Addr from{}; int n;
        while ((n = gs.recv(buf, sizeof(buf), &from)) > 0) {
            net::PktType tp;
            if (!net::checkHdr(buf, n, &tp)) continue;
            if (tp == net::P_STATE && n >= (int)sizeof(net::StatePkt)) {
                const net::StatePkt* sp_ = (const net::StatePkt*)buf;
                if (sp_->nonce != nonce) continue;
                if (sp_->seq > lastStateSeq || !haveView) {
                    lastStateSeq = sp_->seq;
                    view = sp_->st;
                    if (!haveView) { prev = view; haveView = true; }
                }
                lastRecv = now;
            } else if (tp == net::P_BYE) {
                say("YOUR OPPONENT LEFT", 4);
                gs.close(); peerOk = false; sc = SC_TITLE; sel = 0;
                return;
            }
        }
        if (now - lastRecv > 4.0) { say("LOST CONTACT WITH THE HOST", 4); gs.close(); peerOk = false; sc = SC_TITLE; sel = 0; }
    }

    void tickGame() {
        uint16_t e = allPressed();
        bool net_ = (mode == M_HOST || mode == M_CLIENT);
        const State& cur = (mode == M_CLIENT) ? view : game.s;
        bool over = cur.phase == PH_OVER;

        // overlays
        if (!net_ && paused) {
            uint16_t m = menuPressed();
            if (m & R_UP) { pauseSel = (pauseSel + 2) % 3; au.play(S_MOVE); }
            if (m & R_DOWN) { pauseSel = (pauseSel + 1) % 3; au.play(S_MOVE); }
            if (m & (R_START | R_B | R_BACK)) { paused = false; au.play(S_BACK); }
            else if (m & R_A) {
                au.play(S_SELECT);
                if (pauseSel == 0) paused = false;
                else if (pauseSel == 1) startMatch(mode);
                else { sc = SC_TITLE; sel = 0; }
            }
            return;
        }
        uint16_t forfeitPress = 0;
        if (net_ && forfeitAsk) {
            if (e & R_A) { forfeitAsk = false; forfeitPress = A_FORFEIT; au.play(S_SELECT); }
            else if (e & (R_B | R_BACK | R_START)) { forfeitAsk = false; au.play(S_BACK); }
            e = 0;
        } else if (!over && (e & (R_START | R_BACK))) {
            if (net_) forfeitAsk = true; else { paused = true; pauseSel = 0; }
            au.play(S_SELECT);
            return;
        }
        if (over && (e & (R_B | R_BACK))) { au.play(S_BACK); leaveMatch(); return; }

        // local inputs
        Input in[2];
        if (mode == M_LOCAL) {
            Device* d1 = pads.size() >= 1 ? &pads[0] : &kb;
            Device* d2 = pads.size() >= 2 ? &pads[1] : (pads.size() == 1 ? &kb : nullptr);
            uint16_t h1 = d1->held, p1 = (uint16_t)(d1->held & ~d1->prev);
            if (pads.size() >= 2) { h1 |= kb.held; p1 |= (uint16_t)(kb.held & ~kb.prev); }
            in[0] = toActions(h1, p1, false);
            if (d2) in[1] = toActions(d2->held, (uint16_t)(d2->held & ~d2->prev), true);
        } else {
            in[0] = toActions(allHeld(), e, false);
        }
        if (forfeitAsk) in[0] = Input{};
        in[0].pressed |= forfeitPress;

        if (mode == M_CLIENT) {
            Input mine = in[0];
            if (botFor[0] && haveView) { Game tmp; tmp.s = view; mine = bot[0].think(tmp, 1); mine.pressed |= forfeitPress; }
            insend.update(mine);
            net::InputPkt ip{}; net::fillHdr(ip.h, net::P_INPUT);
            ip.nonce = nonce; ip.seq = ++seq; ip.held = insend.held;
            memcpy(ip.counts, insend.counts, sizeof(ip.counts));
            gs.send(&ip, sizeof(ip), peer);
            clientNetPoll();
            return;
        }

        if (mode == M_HOST) { hostNetPoll(); in[1] = inrecv.take(); }
        for (int p = 0; p < 2; ++p) if (botFor[p]) { Input b = bot[p].think(game, p); if (p == 0) b.pressed |= forfeitPress; in[p] = b; }

        // rematch
        if (game.s.phase == PH_OVER) {
            for (int p = 0; p < 2; ++p) if (in[p].pressed & A_READY) game.s.pl[p].ready = 1;
            if (mode == M_HOST && peerGone) game.s.pl[1].ready = 0;
            if (game.s.pl[0].ready && game.s.pl[1].ready && game.s.phaseTimer > 45) {
                uint32_t keepFrame = game.s.frame;
                game.reset(rng());
                game.s.frame = keepFrame;
                sawPlay = false;
                for (auto& b : bot) { int l = b.level; b = Bot(); b.level = l; }
            }
        }
        game.step(in);
        view = game.s;
        haveView = true;
        if (mode == M_HOST && peerOk) {
            net::StatePkt sp_{}; net::fillHdr(sp_.h, net::P_STATE);
            sp_.nonce = nonce; sp_.seq = ++seq; sp_.st = game.s;
            gs.send(&sp_, sizeof(sp_), peer);
        }
    }

    // ------------------------------------------------------------ effects from state changes
    int srow(int globalRow) const { return viewer == 0 ? globalRow : H - 1 - globalRow; }

    void spawnSplash(float x, float y, Col c, int n, float speed) {
        std::uniform_real_distribution<float> U(-1.f, 1.f);
        for (int i = 0; i < n; ++i) {
            float a = U(rng) * 3.14159f, sp_ = speed * (0.4f + 0.6f * std::fabs(U(rng)));
            Particle p{x, y, std::cos(a) * sp_, std::sin(a) * sp_ - speed * 0.6f, 2.0f + 2.5f * std::fabs(U(rng)), 0, 0.6f + 0.5f * std::fabs(U(rng)), c};
            parts.push_back(p);
        }
    }

    void effects() {
        if (!haveView) return;
        const State& s = view;
        const State& o = prev;
        if (s.phase == PH_COUNTDOWN && o.phase == PH_OVER) { au.play(S_TICK); prev = view; return; }
        if (s.phase == PH_COUNTDOWN) {
            if (o.phase != PH_COUNTDOWN || (o.phaseTimer > 120 && s.phaseTimer <= 120) || (o.phaseTimer > 60 && s.phaseTimer <= 60))
                au.play(S_TICK);
        }
        if (s.phase == PH_PLAY && o.phase == PH_COUNTDOWN) { au.play(S_GO); playStartFrame = s.frame; sawPlay = true; }
        if (s.phase == PH_OVER && o.phase != PH_OVER) {
            bool won = s.winner == viewer || (mode == M_LOCAL && s.winner != 2);
            au.play(won ? S_WIN : S_LOSE);
            if (s.winner == 0 || s.winner == 1) excite[s.winner] = 99;
        }
        for (int p = 0; p < 2; ++p) {
            const PlayerView& a = s.pl[p];
            const PlayerView& b = o.pl[p];
            bool mine = (p == viewer) || mode == M_LOCAL;
            float pan = (p == viewer) ? -0.3f : 0.3f;
            float vol = mine ? 1.0f : 0.55f;
            if (a.evMove != b.evMove && mine) au.play(S_MOVE, 0.7f, pan);
            if (a.evRotate != b.evRotate && mine) au.play(S_ROTATE, 0.8f, pan);
            if (a.evLand != b.evLand && mine) au.play(S_LAND, 0.7f, pan);
            if (a.evHold != b.evHold && mine) au.play(S_HOLD, 0.8f, pan);
            if (a.evLock != b.evLock) { au.play(S_LOCK, vol, pan); squashV[p] += 2.5f; }
            if (a.evVanish != b.evVanish) {
                au.play(S_VANISH, vol, pan);
                float y = (p == viewer) ? FY + H * C - 30 : FY + 30;
                texts.push_back({"PLOP!", FX + W * C / 2, y, 0, pieceColor(5), 3});
            }
            if (a.evClear != b.evClear) {
                int n = a.lastClearN;
                au.play((Sfx)(S_CLEAR1 + std::max(0, std::min(3, n - 1))), vol, pan);
                for (int i = 0; i < std::min(n, 4); ++i) {
                    int r = srow(a.lastClearRows[i]);
                    for (int x = 0; x < W; ++x)
                        spawnSplash(FX + x * C + C / 2, FY + r * C + C / 2, pieceColor(a.lastClearTypes[i][x]), 3, 180);
                }
                static const char* names[5] = {"", "", "DOUBLE!", "TRIPLE!", "QUAD SPLAT!"};
                if (n >= 2) {
                    float y = FY + srow(a.lastClearRows[0]) * C;
                    texts.push_back({names[std::min(n, 4)], FX + W * C / 2, y, 0, hex(0xffffff), 3});
                }
                squashV[p] += 3;
            }
            if (a.evPush != b.evPush) {
                au.play(p == viewer || mode == M_LOCAL ? S_PUSH : S_PUSHED, 1.0f, pan);
                shake = 10.0f + a.lastPush * 3.0f;
                excite[p] = 1.6f;
                squashV[1 - p] -= 5;
                char buf[32]; snprintf(buf, sizeof(buf), "PUSH +%d", a.lastPush);
                float y = FY + srow(s.center) * C;
                texts.push_back({buf, FX + W * C / 2, y - 24, 0, th().pop, 4});
            }
        }
        prev = view;
    }

    void tickFx(float dt) {
        for (auto& p : parts) { p.life += dt; p.vy += 520 * dt; p.x += p.vx * dt; p.y += p.vy * dt; }
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.life >= p.max; }), parts.end());
        for (auto& f : texts) { f.t += dt; f.y -= 30 * dt; }
        texts.erase(std::remove_if(texts.begin(), texts.end(), [](const FloatText& f) { return f.t > 1.3f; }), texts.end());
        shake *= std::pow(0.02f, dt);
        for (int p = 0; p < 2; ++p) {
            // springy squash
            squashV[p] += -squash[p] * 260 * dt;
            squashV[p] *= std::pow(0.04f, dt);
            squash[p] += squashV[p] * dt * 6;
            squash[p] = std::max(-1.2f, std::min(1.2f, squash[p]));
            if (excite[p] > 0 && excite[p] < 50) excite[p] = std::max(0.0f, excite[p] - dt);
        }
    }

    // ------------------------------------------------------------ menus
    void tickTitle() {
        static const int N = 7;
        uint16_t e = menuPressed();
        if (e & R_UP) { sel = (sel + N - 1) % N; au.play(S_MOVE); }
        if (e & R_DOWN) { sel = (sel + 1) % N; au.play(S_MOVE); }
        if (e & (R_A | R_START)) {
            au.play(S_SELECT);
            switch (sel) {
                case 0: beginHost(); break;
                case 1: beginJoin(); break;
                case 2: startMatch(M_CPU); break;
                case 3:
                    if (pads.size() < 1) say("PLUG IN A SECOND CONTROLLER FOR 2 PLAYERS");
                    else startMatch(M_LOCAL);
                    break;
                case 4: sc = SC_HELP; break;
                case 5: sc = SC_SETTINGS; sel = 0; break;
                case 6: running = false; break;
            }
        }
    }
    void tickSettings() {
        uint16_t e = menuPressed();
        const int N = 4;
        if (e & R_UP) { sel = (sel + N - 1) % N; au.play(S_MOVE); }
        if (e & R_DOWN) { sel = (sel + 1) % N; au.play(S_MOVE); }
        int d = (e & R_RIGHT) ? 1 : (e & R_LEFT) ? -1 : (e & R_A) ? 1 : 0;
        if (d && sel < 3) {
            if (sel == 0) cfg.theme ^= 1;
            if (sel == 1) { cfg.sound = !cfg.sound; au.enabled = cfg.sound; }
            if (sel == 2) cfg.cpu = (cfg.cpu + 3 + d) % 3;
            cfg.save(); au.play(S_SELECT);
        }
        if ((e & (R_B | R_BACK)) || (sel == 3 && (e & (R_A | R_START)))) { au.play(S_BACK); sc = SC_TITLE; sel = 5; }
    }

    void tick() {
        readDevices();
        switch (sc) {
            case SC_TITLE: tickTitle(); break;
            case SC_HOST: tickHostWait(); break;
            case SC_JOIN: tickJoin(); break;
            case SC_SETTINGS: tickSettings(); break;
            case SC_HELP: if (allPressed() & (R_A | R_B | R_BACK | R_START)) { au.play(S_BACK); sc = SC_TITLE; sel = 4; } break;
            case SC_GAME: tickGame(); if (sc == SC_GAME) effects(); break;
        }
        tickFx(1.0f / 60);
    }

    // ------------------------------------------------------------ drawing
    struct BlobMap { uint8_t t[H][W]; uint8_t id[H][W]; uint8_t f[H][W]; int w, h; };

    float blinkFor(int id, int salt) const {
        float ph = std::fmod((float)t + (id * 0.618f + salt) * 2.7f, 4.4f);
        return ph < 0.13f ? 1.0f : 0.0f;
    }

    void drawBlobs(const BlobMap& m, float ox, float oy, float cs, uint8_t alpha, bool faces = true) {
        const Theme& T = th();
        float pad = std::max(0.5f, cs * 0.05f), o = std::max(1.5f, cs * 0.1f), rad = cs * 0.32f;
        auto same = [&](int y, int x, int y2, int x2) {
            if (y2 < 0 || y2 >= m.h || x2 < 0 || x2 >= m.w) return false;
            return m.t[y2][x2] && m.id[y2][x2] == m.id[y][x] && m.t[y][x];
        };
        Col ink = withA(T.slimeInk, alpha);
        for (int y = 0; y < m.h; ++y) for (int x = 0; x < m.w; ++x) {
            if (!m.t[y][x]) continue;
            float px = ox + x * cs, py = oy + y * cs;
            g.rrect(px + pad, py + pad, cs - 2 * pad, cs - 2 * pad, rad, ink);
            if (same(y, x, y, x + 1)) g.rect(px + cs / 2, py + pad, cs, cs - 2 * pad, ink);
            if (same(y, x, y + 1, x)) g.rect(px + pad, py + cs / 2, cs - 2 * pad, cs, ink);
        }
        for (int y = 0; y < m.h; ++y) for (int x = 0; x < m.w; ++x) {
            if (!m.t[y][x]) continue;
            Col c = withA(pieceColor(m.t[y][x]), alpha);
            float px = ox + x * cs, py = oy + y * cs, in = pad + o;
            g.rrect(px + in, py + in, cs - 2 * in, cs - 2 * in, std::max(1.0f, rad - o), c);
            bool r = same(y, x, y, x + 1), d = same(y, x, y + 1, x);
            if (r) g.rect(px + cs / 2, py + in, cs, cs - 2 * in, c);
            if (d) g.rect(px + in, py + cs / 2, cs - 2 * in, cs, c);
            if (r && d && same(y, x, y + 1, x + 1)) g.rect(px + cs / 2, py + cs / 2, cs, cs, c);
        }
        if (alpha < 200) return;
        for (int y = 0; y < m.h; ++y) for (int x = 0; x < m.w; ++x) {
            if (!m.t[y][x]) continue;
            float px = ox + x * cs, py = oy + y * cs;
            if (!same(y, x, y - 1, x) && !same(y, x, y, x - 1))
                g.ellipse(px + cs * 0.33f, py + cs * 0.3f, cs * 0.15f, cs * 0.09f, Col{255, 255, 255, 170});
            if (faces && m.f[y][x]) {
                float bl = blinkFor(m.id[y][x], x + y);
                float ey = py + cs * 0.52f, ex = cs * 0.2f;
                g.ellipse(px + cs / 2 - ex, ey, cs * 0.085f, cs * 0.12f * (1 - 0.85f * bl), T.slimeInk);
                g.ellipse(px + cs / 2 + ex, ey, cs * 0.085f, cs * 0.12f * (1 - 0.85f * bl), T.slimeInk);
                if (bl < 0.5f && cs >= 14) {
                    g.circle(px + cs / 2 - ex + cs * 0.03f, ey - cs * 0.05f, cs * 0.035f, Col{255, 255, 255, 255});
                    g.circle(px + cs / 2 + ex + cs * 0.03f, ey - cs * 0.05f, cs * 0.035f, Col{255, 255, 255, 255});
                }
                g.ellipse(px + cs * 0.18f, py + cs * 0.72f, cs * 0.1f, cs * 0.055f, hex(0xff6f9f, 150));
                g.ellipse(px + cs * 0.82f, py + cs * 0.72f, cs * 0.1f, cs * 0.055f, hex(0xff6f9f, 150));
                g.smile(px + cs / 2, py + cs * 0.72f, cs * 0.26f, cs * 0.13f, T.slimeInk);
            }
        }
    }

    void pieceIntoMap(BlobMap& m, int type, int rot, int ox, int oy, uint8_t id) {
        auto c = cells(type, rot);
        int fi = faceIndex(type);
        for (int i = 0; i < 4; ++i) {
            int x = ox + c[i][0], y = oy + c[i][1];
            if (x < 0 || x >= m.w || y < 0 || y >= m.h) continue;
            m.t[y][x] = (uint8_t)type; m.id[y][x] = id; m.f[y][x] = (i == fi);
        }
    }

    void drawMini(int type, float cx, float cy, float cs) {
        if (type < 1 || type > 7) return;
        static BlobMap m;
        memset(&m, 0, sizeof(m)); m.w = 4; m.h = 4;
        pieceIntoMap(m, type, 0, 0, 0, 1);
        int minx = 9, maxx = -1, miny = 9, maxy = -1;
        auto c = cells(type, 0);
        for (int i = 0; i < 4; ++i) { minx = std::min(minx, (int)c[i][0]); maxx = std::max(maxx, (int)c[i][0]); miny = std::min(miny, (int)c[i][1]); maxy = std::max(maxy, (int)c[i][1]); }
        float w = (maxx - minx + 1) * cs, h = (maxy - miny + 1) * cs;
        drawBlobs(m, cx - w / 2 - minx * cs, cy - h / 2 - miny * cs, cs, 255);
    }

    void panelBox(float x, float y, float w, float h) {
        const Theme& T = th();
        g.rrect(x - 3, y - 3, w + 6, h + 6, 18, T.line);
        g.rrect(x, y, w, h, 15, T.panel);
    }

    void drawBackground() {
        const Theme& T = th();
        g.clear(T.bg);
        // soft drifting bubbles
        for (int i = 0; i < 26; ++i) {
            float sx = std::fmod(i * 197.3f + (float)t * (6 + (i % 5) * 3), (float)SW + 120) - 60;
            float sy = std::fmod(i * 131.7f + 300, (float)SH) + std::sin((float)t * 0.7f + i) * 12;
            float r = 10 + (i * 37 % 40);
            g.circle(sx, sy, r, T.bgDot);
        }
    }

    void drawField(float sx, float sy) {
        const Theme& T = th();
        const State& s = view;
        float x0 = FX + sx, y0 = FY + sy;
        g.rrect(x0 - 14, y0 - 14, W * C + 28, H * C + 28, 20, T.line);
        g.rrect(x0 - 11, y0 - 11, W * C + 22, H * C + 22, 17, T.shell);
        g.rect(x0, y0, W * C, H * C, T.field);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) g.circle(x0 + x * C + C / 2, y0 + y * C + C / 2, 1.6f, T.fieldDot);

        // danger glow at an end when that player's stack gets close
        for (int p = 0; p < 2; ++p) {
            int nearest = H;
            for (int r = 0; r < H; ++r) {
                bool any = false;
                for (int x = 0; x < W; ++x) if (s.grid[r][x].tf & 15) { any = true; break; }
                if (any) { int lr = p == 0 ? r : H - 1 - r; nearest = std::min(nearest, lr); }
            }
            if (nearest < 7) {
                float a = (7 - nearest) / 7.0f * (0.5f + 0.5f * std::sin((float)t * 8));
                int er = srow(toGlobal(p, 0));
                float ey = er == 0 ? y0 : y0 + H * C - 4 * C;
                for (int i = 0; i < 4; ++i) {
                    float fade = er == 0 ? (4 - i) / 4.0f : (i + 1) / 4.0f;
                    g.rect(x0, ey + i * C, W * C, C, withA(T.danger, (uint8_t)(a * 90 * fade)));
                }
            }
        }

        // locked slimes
        static BlobMap m;
        memset(&m, 0, sizeof(m)); m.w = W; m.h = H;
        for (int r = 0; r < H; ++r) for (int x = 0; x < W; ++x) {
            const Cell& c = s.grid[r][x];
            if (!(c.tf & 15)) continue;
            int y = srow(r);
            m.t[y][x] = (uint8_t)c.type(); m.id[y][x] = c.id; m.f[y][x] = c.face();
        }
        drawBlobs(m, x0, y0, C, 255);

        // push line
        float ly = y0 + srow(s.center) * C;
        if (viewer == 1) ly = y0 + (H - s.center) * C;
        for (float x = 0; x <= W * C; x += 3) {
            float wy = std::sin(x * 0.12f + (float)t * 4) * 2.2f;
            g.circle(x0 + x, ly + wy, 2.2f, withA(T.pop, 230));
        }
        g.circle(x0 - 11, ly, 7, T.slimeInk); g.circle(x0 - 11, ly, 5, T.pop);
        g.circle(x0 + W * C + 11, ly, 7, T.slimeInk); g.circle(x0 + W * C + 11, ly, 5, T.pop);

        // ghosts + active pieces
        if (s.phase == PH_PLAY) {
            static Game calc;
            calc.s = s;
            for (int p = 0; p < 2; ++p) {
                const PlayerView& v = s.pl[p];
                if (!v.active) continue;
                bool local = (p == viewer) || mode == M_LOCAL;
                if (local) {
                    bool off = false;
                    int d = calc.dropDistance(p, &off);
                    auto c = cells(v.type, v.rot);
                    Col gc = pieceColor(v.type);
                    for (int i = 0; i < 4; ++i) {
                        int lx = v.x + c[i][0], lyy = v.y + c[i][1] + d;
                        if (off) {
                            // warn: this column is empty, the slime will plop out the far end
                            int r = srow(toGlobal(p, H - 1));
                            float wy = y0 + r * C;
                            g.rrect(x0 + lx * C + 3, wy + (r == 0 ? 2 : C - 8), C - 6, 6, 3, withA(T.danger, 200));
                            continue;
                        }
                        if (lyy < 0 || lyy >= H) continue;
                        int r = srow(toGlobal(p, lyy));
                        float px = x0 + lx * C, py = y0 + r * C;
                        g.rrect(px + 2, py + 2, C - 4, C - 4, 6, withA(gc, 70));
                        g.rrect(px + 5, py + 5, C - 10, C - 10, 3, withA(T.field, 255));
                        g.rrect(px + 5, py + 5, C - 10, C - 10, 3, withA(gc, 40));
                    }
                }
                memset(&m, 0, sizeof(m)); m.w = W; m.h = H;
                auto c = cells(v.type, v.rot);
                int fi = faceIndex(v.type);
                for (int i = 0; i < 4; ++i) {
                    int lx = v.x + c[i][0], lyy = v.y + c[i][1];
                    if (lyy < 0 || lyy >= H || lx < 0 || lx >= W) continue;
                    int y = srow(toGlobal(p, lyy));
                    m.t[y][lx] = (uint8_t)v.type; m.id[y][lx] = v.pieceId; m.f[y][lx] = (i == fi);
                }
                drawBlobs(m, x0, y0, C, 255);
            }
        }

        for (auto& p : parts) {
            float a = 1.0f - p.life / p.max;
            g.circle(p.x + sx, p.y + sy, p.r + 1.2f, withA(T.slimeInk, (uint8_t)(200 * a)));
            g.circle(p.x + sx, p.y + sy, p.r, withA(p.c, (uint8_t)(255 * a)));
        }
    }

    void drawPlayerPanel(int p, float x, float y, bool top) {
        const Theme& T = th();
        const PlayerView& v = view.pl[p];
        float w = 210, h = 400;
        panelBox(x, y, w, h);
        std::string name = (mode == M_CPU && p == 1) ? std::string("CPU ") + (cfg.cpu == 0 ? "EASY" : cfg.cpu == 1 ? "NORMAL" : "HARD")
                         : (mode == M_LOCAL) ? (p == 0 ? "PLAYER 1" : "PLAYER 2")
                         : (p == viewer ? myName : peerName);
        Col pc = p == 0 ? pieceColor(5) : pieceColor(6);
        float cy = top ? y + 18 : y + 18;
        g.rrect(x + 14, cy, w - 28, 26, 13, pc);
        g.text(x + w / 2, cy + 6, 2, T.slimeInk, name.c_str(), AL_CENTER);
        // hold + next
        float by = cy + 40;
        g.text(x + 20, by, 2, T.muted, "HOLD");
        g.rrect(x + 16, by + 20, 72, 56, 12, T.field);
        if (v.hold > 0) drawMini(v.hold, x + 52, by + 48, 14);
        g.text(x + 110, by, 2, T.muted, "NEXT");
        g.rrect(x + 106, by + 20, 88, 240, 12, T.field);
        for (int i = 0; i < NEXT_SHOWN; ++i) drawMini(v.next[i], x + 150, by + 46 + i * 46, i == 0 ? 15 : 12);
        // stats
        char buf[64];
        float sy = by + 90;
        g.text(x + 20, sy, 2, T.muted, "SCORE");
        snprintf(buf, sizeof(buf), "%d", v.score);
        g.text(x + 20, sy + 20, 3, T.text, buf);
        g.text(x + 20, sy + 60, 2, T.muted, "LINES");
        snprintf(buf, sizeof(buf), "%d", v.lines);
        g.text(x + 20, sy + 80, 3, T.text, buf);
        g.text(x + 20, sy + 120, 2, T.muted, "LEVEL");
        snprintf(buf, sizeof(buf), "%d", view.level);
        g.text(x + 20, sy + 140, 3, T.text, buf);
        // push lead
        int lead = (view.center - H / 2) * (p == 0 ? 1 : -1);
        g.text(x + 20, y + h - 50, 2, T.muted, "PUSH");
        snprintf(buf, sizeof(buf), "%+d", lead);
        g.text(x + 20, y + h - 30, 3, lead > 0 ? hex(0x46b98a) : lead < 0 ? T.danger : T.text, buf);
        (void)v;
    }

    int moodFor(int p) const {
        const State& s = view;
        if (s.phase == PH_OVER) return s.winner == p ? 2 : (s.winner == 2 ? 0 : 3);
        if (excite[p] > 0) return 2;
        int nearest = H;
        for (int r = 0; r < H; ++r) for (int x = 0; x < W; ++x)
            if (s.grid[r][x].tf & 15) { int lr = p == 0 ? r : H - 1 - r; nearest = std::min(nearest, lr); }
        return nearest < 8 ? 1 : 0;
    }

    void drawMascot(int p, float cx, float by, float w) {
        float bob = std::sin((float)t * 3 + p * 1.7f) * 0.06f;
        float sq = squash[p] + bob;
        float hop = 0;
        if (view.phase == PH_OVER && view.winner == p) hop = std::fabs(std::sin((float)t * 7)) * 30;
        g.ellipse(cx, by + 4, w * 0.45f, w * 0.06f, withA(th().slimeInk, 50));
        g.slime(cx, by - hop, w, p == 0 ? pieceColor(5) : pieceColor(6), th().slimeInk, sq, blinkFor(p * 7 + 3, 0), moodFor(p), (float)t);
    }

    void centerCard(float y, float h) {
        panelBox(FX - 60, y, W * C + 120, h);
    }

    void drawGame() {
        const Theme& T = th();
        if (!haveView) { drawBackground(); g.text(SW / 2, SH / 2, 4, T.text, "CONNECTING...", AL_CENTER, true); return; }
        drawBackground();
        std::uniform_real_distribution<float> U(-1, 1);
        float sx = shake > 0.3f ? U(rng) * shake : 0, sy = shake > 0.3f ? U(rng) * shake : 0;
        drawField(sx, sy);

        int me = viewer, op = 1 - viewer;
        drawPlayerPanel(me, FX - 290, FY, true);
        drawPlayerPanel(op, FX + W * C + 80, SH - FY - 400, false);
        drawMascot(me, FX - 185, SH - 40, 170);
        drawMascot(op, FX + W * C + 185, 230, 170);

        // controls hint
        const char* hint = (mode == M_LOCAL) ? "START: PAUSE" : (mode == M_CPU ? "START: PAUSE" : "START: FORFEIT");
        g.text(24, SH - 26, 2, T.muted, hint);

        for (auto& f : texts) {
            float a = f.t < 1.0f ? 1.0f : 1.0f - (f.t - 1.0f) / 0.3f;
            float pop = f.t < 0.12f ? 0.6f + f.t / 0.12f * 0.5f : 1.0f + 0.1f * std::max(0.0f, 0.25f - f.t);
            float s = f.scale * pop;
            g.text(f.x + 2, f.y + 2, s, withA(T.slimeInk, (uint8_t)(220 * a)), f.s.c_str(), AL_CENTER, true);
            g.text(f.x, f.y, s, withA(f.c, (uint8_t)(255 * a)), f.s.c_str(), AL_CENTER, true);
        }

        const State& s = view;
        if (s.phase == PH_COUNTDOWN) {
            int n = s.phaseTimer / 60 + 1;
            char b[4]; snprintf(b, sizeof(b), "%d", std::min(3, n));
            float k = (s.phaseTimer % 60) / 60.0f;
            float sc_ = 10 + 6 * k;
            float cy = FY + H * C / 2 - sc_ * 3.5f;
            g.text(FX + W * C / 2 + 4, cy + 4, sc_, T.slimeInk, b, AL_CENTER, true);
            g.text(FX + W * C / 2, cy, sc_, T.pop, b, AL_CENTER, true);
        } else if (s.phase == PH_PLAY && sawPlay && s.frame - playStartFrame < 50) {
            float cy = FY + H * C / 2 - 30;
            g.text(FX + W * C / 2 + 3, cy + 3, 8, T.slimeInk, "PUSH!", AL_CENTER, true);
            g.text(FX + W * C / 2, cy, 8, T.pop, "PUSH!", AL_CENTER, true);
        }

        if (s.phase == PH_OVER) {
            centerCard(270, 250);
            const char* head;
            if (s.winner == 2) head = "DRAW!";
            else if (mode == M_LOCAL) head = s.winner == 0 ? "P1 WINS!" : "P2 WINS!";
            else head = s.winner == viewer ? "YOU WIN!" : "YOU LOSE";
            g.text(FX + W * C / 2 + 3, 303, 6, T.slimeInk, head, AL_CENTER, true);
            g.text(FX + W * C / 2, 300, 6, s.winner == viewer || mode == M_LOCAL ? T.pop : T.danger, head, AL_CENTER, true);
            const char* why = s.reason == 1 ? "OPPONENT LEFT" : s.reason == 2 ? "BY FORFEIT" : "";
            g.text(FX + W * C / 2, 365, 2, T.muted, why, AL_CENTER);
            bool canRematch = !(mode == M_HOST && peerGone);
            if (canRematch) {
                const char* r = s.pl[viewer].ready ? "WAITING FOR THEM..." : "A: PLAY AGAIN";
                if (mode == M_LOCAL) r = (s.pl[0].ready || s.pl[1].ready) ? "BOTH PRESS A..." : "A: PLAY AGAIN";
                g.text(FX + W * C / 2, 410, 2, T.text, r, AL_CENTER);
                if (mode != M_LOCAL && s.pl[1 - viewer].ready && !s.pl[viewer].ready)
                    g.text(FX + W * C / 2, 440, 2, hex(0x46b98a), "THEY WANT A REMATCH!", AL_CENTER);
            }
            g.text(FX + W * C / 2, 475, 2, T.muted, "B: BACK TO MENU", AL_CENTER);
        }

        if (paused) {
            g.rect(0, 0, SW, SH, withA(T.bg, 170));
            centerCard(250, 300);
            g.text(FX + W * C / 2, 280, 5, T.text, "PAUSED", AL_CENTER, true);
            static const char* items[3] = {"RESUME", "RESTART", "QUIT TO MENU"};
            for (int i = 0; i < 3; ++i) pill(FX + W * C / 2, 350 + i * 56, 250, items[i], i == pauseSel);
        }
        if (forfeitAsk) {
            centerCard(300, 180);
            g.text(FX + W * C / 2, 330, 4, T.text, "GIVE UP?", AL_CENTER, true);
            g.text(FX + W * C / 2, 390, 2, T.text, "A: YES, FORFEIT", AL_CENTER);
            g.text(FX + W * C / 2, 420, 2, T.muted, "B: KEEP PLAYING", AL_CENTER);
        }
    }

    void pill(float cx, float y, float w, const char* label, bool on) {
        const Theme& T = th();
        float h = 44;
        float bounce = on ? std::sin((float)t * 6) * 2 : 0;
        g.rrect(cx - w / 2 - 3, y - 3 + bounce, w + 6, h + 6, (h + 6) / 2, T.line);
        g.rrect(cx - w / 2, y + bounce, w, h, h / 2, on ? T.pop : T.panel);
        g.text(cx, y + 15 + bounce, 2, on ? T.slimeInk : T.text, label, AL_CENTER);
    }

    void drawTitleLogo(float cx, float y, float scale) {
        const Theme& T = th();
        const char* s = "SLIME PUSH";
        float w = g.textWidth(s, scale);
        float x = cx - w / 2;
        static const int cols[] = {5, 7, 2, 4, 3, 0, 6, 1, 5, 7};
        for (int i = 0; s[i]; ++i) {
            char ch[2] = {s[i], 0};
            float wob = std::sin((float)t * 3 + i * 0.6f) * 5;
            g.text(x + i * 6 * scale + 5, y + wob + 6, scale, T.slimeInk, ch, AL_LEFT, true);
            g.text(x + i * 6 * scale, y + wob, scale, pieceColor(cols[i] ? cols[i] : 2), ch, AL_LEFT, true);
        }
    }

    void drawTitle() {
        const Theme& T = th();
        drawBackground();
        drawTitleLogo(SW / 2, 60, 11);
        float hop = std::fabs(std::sin((float)t * 2.6f));
        g.ellipse(330, 650, 120 * (1 - hop * 0.3f), 14, withA(T.slimeInk, 40));
        g.slime(330, 640 - hop * 60, 260, pieceColor(5), T.slimeInk, (1 - hop) * 0.5f - 0.15f, blinkFor(1, 0), 0, (float)t);
        g.slime(470, 660, 110, pieceColor(6), T.slimeInk, std::sin((float)t * 5) * 0.15f, blinkFor(2, 0), 0, (float)t);
        static const char* items[7] = {"LAN BATTLE: HOST", "LAN BATTLE: JOIN", "VS CPU", "2 PLAYERS (2 PADS)", "HOW TO PLAY", "SETTINGS", "QUIT"};
        for (int i = 0; i < 7; ++i) pill(870, 220 + i * 66, 400, items[i], i == sel);
        g.text(SW - 24, SH - 26, 2, T.muted, myName.c_str(), AL_RIGHT);
    }

    void drawHost() {
        const Theme& T = th();
        drawBackground();
        g.text(SW / 2, 70, 5, T.text, "WAITING FOR A CHALLENGER", AL_CENTER, true);
        float hop = std::fabs(std::sin((float)t * 3));
        g.slime(SW / 2, 420 - hop * 40, 220, pieceColor(5), T.slimeInk, (1 - hop) * 0.4f, blinkFor(1, 0), 0, (float)t);
        panelBox(SW / 2 - 320, 470, 640, 250);
        std::string a = "YOU ARE " + myName;
        g.text(SW / 2, 500, 3, T.text, a.c_str(), AL_CENTER);
        g.text(SW / 2, 545, 2, T.muted, "ON THE OTHER DECK PICK  LAN BATTLE: JOIN", AL_CENTER);
        g.text(SW / 2, 575, 2, T.muted, "THIS DECK WILL SHOW UP THERE BY ITSELF", AL_CENTER);
        // local addresses for the curious
        std::string ips;
        for (auto& ip : net::localAddresses()) { if (!ips.empty()) ips += "  "; ips += ip; }
        if (ips.empty()) ips = "NO NETWORK FOUND - CHECK WI-FI";
        g.text(SW / 2, 625, 2, T.muted, ("ADDRESS: " + ips).c_str(), AL_CENTER);
        int dots = (int)(t * 2) % 4;
        std::string w = "BROADCASTING"; for (int i = 0; i < dots; ++i) w += ".";
        g.text(SW / 2 - 70, 665, 2, T.text, w.c_str());
        g.text(SW / 2, SH - 40, 2, T.muted, "B: CANCEL", AL_CENTER);
    }

    void drawJoin() {
        const Theme& T = th();
        drawBackground();
        g.text(SW / 2, 60, 5, T.text, "FIND A GAME", AL_CENTER, true);
        if (joining) {
            float hop = std::fabs(std::sin((float)t * 4));
            g.slime(SW / 2, 420 - hop * 40, 200, pieceColor(6), T.slimeInk, (1 - hop) * 0.4f, 0, 2, (float)t);
            std::string s = "JOINING " + peerName + "...";
            g.text(SW / 2, 480, 3, T.text, s.c_str(), AL_CENTER);
            g.text(SW / 2, SH - 40, 2, T.muted, "B: CANCEL", AL_CENTER);
            return;
        }
        auto& hs = finder.hosts;
        if (hs.empty()) {
            float k = std::sin((float)t * 2) * 0.2f;
            g.slime(SW / 2, 400, 200, pieceColor(6), T.slimeInk, k, blinkFor(4, 0), 1, (float)t);
            int dots = (int)(t * 2) % 4;
            std::string s = "LOOKING FOR SLIMES ON YOUR NETWORK"; for (int i = 0; i < dots; ++i) s += ".";
            g.text(SW / 2 - g.textWidth("LOOKING FOR SLIMES ON YOUR NETWORK", 3) / 2, 450, 3, T.text, s.c_str());
            g.text(SW / 2, 510, 2, T.muted, "ON THE OTHER DECK PICK  LAN BATTLE: HOST", AL_CENTER);
            g.text(SW / 2, 540, 2, T.muted, "BOTH DECKS NEED TO BE ON THE SAME WI-FI", AL_CENTER);
        } else {
            for (size_t i = 0; i < hs.size() && i < 6; ++i) {
                std::string label = hs[i].name + (hs[i].busy ? "  (IN A GAME)" : "");
                pill(SW / 2, 170 + (float)i * 70, 560, label.c_str(), (int)i == sel);
            }
            g.text(SW / 2, SH - 70, 2, T.muted, "A: JOIN", AL_CENTER);
        }
        g.text(SW / 2, SH - 40, 2, T.muted, "B: BACK", AL_CENTER);
    }

    void drawSettings() {
        const Theme& T = th();
        drawBackground();
        g.text(SW / 2, 80, 6, T.text, "SETTINGS", AL_CENTER, true);
        std::string items[4] = {
            std::string("THEME: ") + (cfg.theme ? "DARK" : "LIGHT"),
            std::string("SOUND: ") + (cfg.sound ? "ON" : "OFF"),
            std::string("CPU: ") + (cfg.cpu == 0 ? "EASY" : cfg.cpu == 1 ? "NORMAL" : "HARD"),
            "BACK"};
        for (int i = 0; i < 4; ++i) pill(SW / 2, 220 + i * 80, 460, items[i].c_str(), i == sel);
        g.text(SW / 2, SH - 40, 2, T.muted, "LEFT / RIGHT: CHANGE    B: BACK", AL_CENTER);
    }

    void drawHelp() {
        const Theme& T = th();
        drawBackground();
        g.text(SW / 2, 40, 5, T.text, "HOW TO PLAY", AL_CENTER, true);
        panelBox(120, 110, SW - 240, 620);
        const char* lines[] = {
            "YOU BOTH SHARE ONE TALL JAR OF SLIME.",
            "YOUR SLIMES DROP DOWN FROM THE TOP. THEIRS RISE UP FROM THE BOTTOM.",
            "FILL A ROW TO SPLAT IT. CLEAR 2 OR MORE AT ONCE TO PUSH THE WHOLE",
            "PILE TOWARD YOUR OPPONENT: DOUBLE +1  TRIPLE +2  QUAD +4.",
            "IF SLIME GETS PUSHED OFF THEIR END, OR THEY STACK INTO THEIR OWN",
            "END, THEY LOSE. DROP A SLIME DOWN AN EMPTY COLUMN AND IT PLOPS AWAY.",
            "",
            "CONTROLS",
            "D-PAD OR LEFT STICK   MOVE",
            "DOWN                  SOFT DROP",
            "UP                    HARD DROP",
            "A                     SPIN RIGHT",
            "B OR X                SPIN LEFT",
            "Y, L1/R1, L2/R2       HOLD",
            "START                 PAUSE / FORFEIT",
            "",
            "KEYBOARD: ARROWS, X/Z SPIN, C HOLD, SPACE HARD DROP, ENTER, ESC",
        };
        float y = 140;
        for (const char* l : lines) {
            bool head = !strcmp(l, "CONTROLS");
            g.text(160, y, 2, head ? T.text : (l[0] ? T.text : T.muted), l);
            y += head ? 38 : 34;
        }
        g.text(SW / 2, SH - 40, 2, T.muted, "B: BACK", AL_CENTER);
    }

    void render() {
        switch (sc) {
            case SC_TITLE: drawTitle(); break;
            case SC_HOST: drawHost(); break;
            case SC_JOIN: drawJoin(); break;
            case SC_SETTINGS: drawSettings(); break;
            case SC_HELP: drawHelp(); break;
            case SC_GAME: drawGame(); break;
        }
        if (now < toastUntil && !toast.empty()) {
            const Theme& T = th();
            float w = g.textWidth(toast.c_str(), 2) + 48;
            g.rrect(SW / 2 - w / 2 - 3, SH - 100 - 3, w + 6, 46, 23, T.line);
            g.rrect(SW / 2 - w / 2, SH - 100, w, 40, 20, T.pop);
            g.text(SW / 2, SH - 87, 2, T.slimeInk, toast.c_str(), AL_CENTER);
        }
        SDL_RenderPresent(ren);
    }

    void screenshot(const std::string& path) {
        int w, h;
        SDL_GetRendererOutputSize(ren, &w, &h);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        SDL_SaveBMP(s, path.c_str());
        SDL_FreeSurface(s);
    }

    // ------------------------------------------------------------ main loop
    int run() {
        net::init();
        cfg.load();
        if (opt.theme >= 0) cfg.theme = opt.theme;
        initName();
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
        }
        Uint32 wf = SDL_WINDOW_ALLOW_HIGHDPI | (opt.windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP);
        win = SDL_CreateWindow("Slime Push", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SW, SH, wf);
        if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
        SDL_RenderSetLogicalSize(ren, SW, SH);
        { SDL_RendererInfo info; if (SDL_GetRendererInfo(ren, &info) == 0) vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0; }
        SDL_ShowCursor(SDL_DISABLE);
        g.init(ren);
        au.init();
        au.enabled = cfg.sound;
        for (int i = 0; i < SDL_NumJoysticks(); ++i) openPad(i);

        if (opt.host) beginHost();
        else if (!opt.join.empty()) { beginJoin(); connectTo(net::makeAddr(opt.join.c_str(), net::GAME_PORT), opt.join); }
        else if (opt.cpu) startMatch(M_CPU);
        else if (opt.find || opt.autojoin) beginJoin();

        Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
        Uint64 start = last;
        double acc = 0;
        bool shotDone = false;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                else if (ev.type == SDL_CONTROLLERDEVICEADDED) openPad(ev.cdevice.which);
                else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) closePad(ev.cdevice.which);
                else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11) {
                    Uint32 f = SDL_GetWindowFlags(win);
                    SDL_SetWindowFullscreen(win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
            }
            Uint64 nowc = SDL_GetPerformanceCounter();
            double dt = (double)(nowc - last) / freq;
            last = nowc;
            now = (double)(nowc - start) / freq;
            acc += std::min(dt, 0.25);
            const double step = 1.0 / 60.0;
            int steps = 0;
            while (acc >= step && steps < 6) { t += step; tick(); acc -= step; ++steps; if (!running) break; }
            if (steps == 6) acc = 0;
            render();
            if (!opt.shot.empty() && !shotDone && now >= opt.shotAt) { screenshot(opt.shot); shotDone = true; }
            if (opt.exitAfter > 0 && now >= opt.exitAfter) running = false;
            if (!vsync) SDL_Delay(steps ? 4 : 2);
        }
        if (sc == SC_GAME) {
            if (mode == M_HOST || mode == M_CLIENT) {
                fprintf(stdout, "RESULT mode=%s phase=%d winner=%d frame=%u score0=%d score1=%d lines0=%d lines1=%d center=%d\n",
                        mode == M_HOST ? "host" : "client", view.phase, view.winner, view.frame,
                        view.pl[0].score, view.pl[1].score, view.pl[0].lines, view.pl[1].lines, view.center);
            }
            leaveMatch();
        }
        au.shutdown();
        g.destroy();
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        net::shutdown();
        return 0;
    }
};

// headless CPU vs CPU games to sanity check the rules
static int selftest(int games) {
    std::mt19937 rng(42);
    int wins[3] = {0, 0, 0};
    long totalFrames = 0;
    int maxPush = 0, pushes = 0, vanishes = 0;
    for (int gi = 0; gi < games; ++gi) {
        Game gm; gm.reset(rng());
        Bot b[2]; b[0].level = 2; b[1].level = (gi % 3);
        int lastPush[2] = {0, 0}, lastVan[2] = {0, 0};
        while (gm.s.phase != PH_OVER && gm.s.frame < 60 * 60 * 15) {
            Input in[2] = {b[0].think(gm, 0), b[1].think(gm, 1)};
            gm.step(in);
            for (int p = 0; p < 2; ++p) {
                if (gm.s.pl[p].evPush != lastPush[p]) { lastPush[p] = gm.s.pl[p].evPush; pushes++; maxPush = std::max(maxPush, (int)gm.s.pl[p].lastPush); }
                if (gm.s.pl[p].evVanish != lastVan[p]) { lastVan[p] = gm.s.pl[p].evVanish; vanishes++; }
            }
        }
        if (gm.s.phase == PH_OVER) wins[gm.s.winner]++;
        totalFrames += gm.s.frame;
        printf("game %d: winner=%d frames=%u lines=%d/%d score=%d/%d center=%d\n", gi, gm.s.winner, gm.s.frame,
               gm.s.pl[0].lines, gm.s.pl[1].lines, gm.s.pl[0].score, gm.s.pl[1].score, gm.s.center);
    }
    printf("SUMMARY hard-wins=%d other-wins=%d draws=%d avg_seconds=%.1f pushes=%d maxpush=%d vanishes=%d\n",
           wins[0], wins[1], wins[2], totalFrames / 60.0 / games, pushes, maxPush, vanishes);
    return 0;
}

int main(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--host") app.opt.host = true;
        else if (a == "--join") app.opt.join = next();
        else if (a == "--cpu") app.opt.cpu = true;
        else if (a == "--autoplay") app.opt.autoplay = true;
        else if (a == "--autojoin") app.opt.autojoin = true;
        else if (a == "--find") app.opt.find = true;
        else if (a == "--windowed") app.opt.windowed = true;
        else if (a == "--exit-after") app.opt.exitAfter = atof(next().c_str());
        else if (a == "--selftest") return selftest(atoi(next().c_str()));
        else if (a == "--screenshot") { app.opt.shot = next(); app.opt.shotAt = atof(next().c_str()); }
        else if (a == "--dark") app.opt.theme = 1;
        else if (a == "--light") app.opt.theme = 0;
        else if (a == "--help" || a == "-h") {
            printf("Slime Push\n  --windowed        run in a window\n  --host            start hosting a LAN game\n"
                   "  --join <ip>       join a host by address\n  --cpu             start a game against the CPU\n"
                   "  --dark/--light    theme\n");
            return 0;
        }
    }
    return app.run();
}
