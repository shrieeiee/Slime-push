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
    std::string screen;
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
        float pad = std::max(0.5f, cs * 0.04f), o = std::max(2.5f, cs * 0.15f), rad = cs * 0.34f;
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

    // ---- console UI kit -------------------------------------------------
    Col border() const { return th().line; }
    Col ink() const { return th().slimeInk; }

    // outlined display text (white or coloured fill, thick ink outline, drop shadow)
    float head(float x, float y, float size, Col fill, const char* s, Align al = AL_CENTER, int font = F_DISPLAY) {
        TextStyle st; st.font = font; st.fill = fill; st.outline = ink();
        st.outlineW = std::max(2.5f, size * 0.2f); st.shadow = std::max(2.0f, size * 0.14f);
        return g.print(x, y, size, st, s, al);
    }
    // bold body text, no outline
    float body(float x, float y, float size, Col c, const char* s, Align al = AL_LEFT) {
        return g.text(x, y, size, c, s, al, F_BODY);
    }

    void panelBox(float x, float y, float w, float h, Col fill = Col{0, 0, 0, 0}) {
        const Theme& T = th();
        g.rrect(x - 7, y - 7 + 9, w + 14, h + 14, 26, withA(ink(), 60));
        g.rrect(x - 7, y - 7, w + 14, h + 14, 26, border());
        g.rrect(x, y, w, h, 19, fill.a ? fill : T.panel);
    }

    // inset well (hold / next boxes)
    void well(float x, float y, float w, float h) {
        g.rrect(x - 4, y - 4, w + 8, h + 8, 14, border());
        g.rrect(x, y, w, h, 10, th().field);
        g.rrect(x, y, w, 6, 3, withA(ink(), 30));
    }

    // chunky console button with a 3D lip
    void button(float cx, float y, float w, const char* label, bool on, float h = 52) {
        const Theme& T = th();
        float lift = on ? -3 - std::fabs(std::sin((float)t * 5)) * 2 : 0;
        if (on) w += 16;
        float x = cx - w / 2, yy = y + lift, lipH = 7, b = 5;
        Col face = on ? T.pop : T.panel;
        Col lip = on ? hex(0xe09a2a) : mix(T.panel, ink(), 0.28f);
        g.rrect(x - b, y + 9 - b, w + 2 * b, h + lipH + 2 * b, (h + 2 * b) / 2, withA(ink(), 55));
        g.rrect(x - b, yy - b, w + 2 * b, h + lipH + 2 * b, (h + 2 * b) / 2, border());
        g.rrect(x, yy + lipH, w, h, h / 2, lip);
        g.rrect(x, yy, w, h, h / 2, face);
        g.rrect(x + h * 0.35f, yy + 6, w - h * 0.7f, h * 0.24f, h * 0.12f, Col{255, 255, 255, (uint8_t)(on ? 110 : 70)});
        float size = 17;
        float ty = yy + (h - size) / 2 + 1;
        if (on) head(cx, ty, size, Col{255, 255, 255, 255}, label, AL_CENTER, F_BODY);
        else body(cx, ty, size, T.text, label, AL_CENTER);
        if (on) {
            float hop = std::fabs(std::sin((float)t * 6));
            g.slime(x - 40, yy + h + 4 - hop * 10, 50, pieceColor(5), ink(), (1 - hop) * 0.35f - 0.1f, blinkFor(9, 1), 0, (float)t);
        }
    }

    // controller-style prompts along the bottom right, e.g. {"A","SELECT"},{"B","BACK"}
    void prompts(std::initializer_list<std::pair<const char*, const char*>> items, float y = SH - 46) {
        const Theme& T = th();
        float total = 0;
        for (auto& it : items) total += 40 + g.measure(it.second, 15, F_BODY) + 30;
        float x = SW - 28 - total;
        for (auto& it : items) {
            std::string k = it.first;
            Col c = k == "A" ? hex(0x46b98a) : k == "B" ? hex(0xff6f9f) : k == "X" ? hex(0x4fbfd8) : k == "Y" ? hex(0xffcf4d) : T.muted;
            float cx = x + 17, cy = y + 8;
            if (k.size() == 1) {
                g.circle(cx, cy + 3, 18, withA(ink(), 60));
                g.circle(cx, cy, 18, border());
                g.circle(cx, cy, 14, c);
                head(cx, cy - 7, 14, Col{255, 255, 255, 255}, k.c_str(), AL_CENTER, F_BODY);
                x += 40;
            } else {   // wide key such as START or a d-pad hint
                float kw = g.measure(k.c_str(), 11, F_BODY) + 24;
                g.rrect(x - 4, cy - 15, kw + 8, 30, 15, border());
                g.rrect(x, cy - 11, kw, 22, 11, c);
                head(x + kw / 2, cy - 5, 11, Col{255, 255, 255, 255}, k.c_str(), AL_CENTER, F_BODY);
                x += kw + 12;
            }
            body(x, y + 1, 15, T.text, it.second);
            x += g.measure(it.second, 15, F_BODY) + 30;
        }
    }

    // title ribbon for sub screens
    void banner(const char* title, Col fill) {
        float w = g.measure(title, 34, F_DISPLAY) + 110, h = 70, x = SW / 2 - w / 2, y = 26;
        g.rrect(x - 6, y - 6 + 9, w + 12, h + 12, 41, withA(ink(), 60));
        g.rrect(x - 6, y - 6, w + 12, h + 12, 41, border());
        g.rrect(x, y, w, h, 35, fill);
        g.rrect(x + 30, y + 7, w - 60, 14, 7, Col{255, 255, 255, 70});
        head(SW / 2, y + (h - 34) / 2, 34, Col{255, 255, 255, 255}, title);
    }

    void drawBackground() {
        const Theme& T = th();
        g.clear(T.bg);
        // slow diagonal scrolling dot grid, like a console home screen
        float sp = 64, off = std::fmod((float)t * 18, sp * 2);
        for (int row = -2; row < SH / sp + 3; ++row)
            for (int col = -2; col < SW / sp + 3; ++col) {
                float x = col * sp + (row & 1) * sp / 2 + off - sp, y = row * sp + off * 0.5f - sp;
                g.circle(x, y, 11, T.bgDot);
            }
    }

    void drawField(float sx, float sy) {
        const Theme& T = th();
        const State& s = view;
        float x0 = FX + sx, y0 = FY + sy;
        g.rrect(x0 - 18, y0 - 18 + 6, W * C + 36, H * C + 36, 24, withA(ink(), 60));
        g.rrect(x0 - 18, y0 - 18, W * C + 36, H * C + 36, 24, border());
        g.rrect(x0 - 12, y0 - 12, W * C + 24, H * C + 24, 18, T.shell);
        g.rrect(x0 - 5, y0 - 5, W * C + 10, H * C + 10, 8, border());
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
        for (int pass = 0; pass < 2; ++pass)
            for (float x = 0; x <= W * C; x += 2.5f) {
                float wy = std::sin(x * 0.12f + (float)t * 4) * 2.5f;
                g.circle(x0 + x, ly + wy, pass ? 2.6f : 4.6f, pass ? T.pop : border());
            }
        g.circle(x0 - 9, ly, 10, border()); g.circle(x0 - 9, ly, 6, T.pop);
        g.circle(x0 + W * C + 9, ly, 10, border()); g.circle(x0 + W * C + 9, ly, 6, T.pop);

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

    void drawPlayerPanel(int p, float x, float y) {
        const Theme& T = th();
        const PlayerView& v = view.pl[p];
        float w = 214, h = 404;
        panelBox(x, y, w, h);
        std::string name = (mode == M_CPU && p == 1) ? std::string("CPU ") + (cfg.cpu == 0 ? "EASY" : cfg.cpu == 1 ? "NORMAL" : "HARD")
                         : (mode == M_LOCAL) ? (p == 0 ? "PLAYER 1" : "PLAYER 2")
                         : (p == viewer ? myName : peerName);
        Col pc = p == 0 ? pieceColor(5) : pieceColor(6);
        // name ribbon
        g.rrect(x + 8, y + 10, w - 16, 42, 21, border());
        g.rrect(x + 13, y + 15, w - 26, 32, 16, pc);
        g.rrect(x + 26, y + 18, w - 52, 8, 4, Col{255, 255, 255, 80});
        head(x + w / 2, y + 23, 14, Col{255, 255, 255, 255}, name.c_str(), AL_CENTER, F_BODY);
        // hold + next
        float by = y + 70;
        body(x + 20, by, 13, T.text, "HOLD");
        well(x + 18, by + 20, 74, 58);
        if (v.hold > 0) drawMini(v.hold, x + 55, by + 49, 14);
        body(x + 116, by, 13, T.text, "NEXT");
        well(x + 114, by + 20, 82, 246);
        for (int i = 0; i < NEXT_SHOWN; ++i) drawMini(v.next[i], x + 155, by + 48 + i * 47, i == 0 ? 16 : 12);
        // stats
        char buf[64];
        float sy = by + 98;
        auto stat = [&](float yy, const char* label, const char* val, Col vc) {
            body(x + 20, yy, 12, T.muted, label);
            head(x + 20, yy + 20, 20, vc, val, AL_LEFT, F_DISPLAY);
        };
        snprintf(buf, sizeof(buf), "%d", v.score); stat(sy, "SCORE", buf, Col{255, 255, 255, 255});
        snprintf(buf, sizeof(buf), "%d", v.lines); stat(sy + 58, "LINES", buf, Col{255, 255, 255, 255});
        snprintf(buf, sizeof(buf), "%d", view.level); stat(sy + 116, "LEVEL", buf, Col{255, 255, 255, 255});
        // push lead
        int lead = (view.center - H / 2) * (p == 0 ? 1 : -1);
        snprintf(buf, sizeof(buf), "%+d", lead);
        float py = y + h - 60;
        g.rrect(x + 14, py - 4, w - 28, 50, 16, border());
        g.rrect(x + 18, py, w - 36, 42, 13, lead > 0 ? hex(0x46b98a) : lead < 0 ? T.danger : T.muted);
        head(x + 34, py + 13, 15, Col{255, 255, 255, 255}, "PUSH", AL_LEFT, F_BODY);
        head(x + w - 34, py + 10, 20, Col{255, 255, 255, 255}, buf, AL_RIGHT, F_DISPLAY);
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
        g.ellipse(cx, by + 4, w * 0.45f, w * 0.07f, withA(ink(), 60));
        g.slime(cx, by - hop, w, p == 0 ? pieceColor(5) : pieceColor(6), ink(), sq, blinkFor(p * 7 + 3, 0), moodFor(p), (float)t);
    }

    void centerCard(float y, float h) { panelBox(FX - 70, y, W * C + 140, h); }

    void drawGame() {
        const Theme& T = th();
        Col white{255, 255, 255, 255};
        drawBackground();
        if (!haveView) { head(SW / 2, SH / 2 - 20, 40, white, "CONNECTING..."); return; }
        std::uniform_real_distribution<float> U(-1, 1);
        float sx = shake > 0.3f ? U(rng) * shake : 0, sy = shake > 0.3f ? U(rng) * shake : 0;
        drawField(sx, sy);

        int me = viewer, op = 1 - viewer;
        drawPlayerPanel(me, FX - 300, FY + 2);
        drawPlayerPanel(op, FX + W * C + 86, SH - FY - 406);
        drawMascot(me, FX - 193, SH - 34, 170);
        drawMascot(op, FX + W * C + 193, 236, 170);

        if (mode == M_HOST || mode == M_CLIENT) prompts({{"START", "FORFEIT"}}, SH - 46);
        else prompts({{"START", "PAUSE"}}, SH - 46);

        for (auto& f : texts) {
            float a = f.t < 1.0f ? 1.0f : 1.0f - (f.t - 1.0f) / 0.3f;
            float pop = f.t < 0.12f ? 0.6f + f.t / 0.12f * 0.5f : 1.0f + 0.1f * std::max(0.0f, 0.25f - f.t);
            float size = f.scale * 8 * pop;
            TextStyle st; st.font = F_DISPLAY; st.fill = withA(f.c, (uint8_t)(255 * a));
            st.outline = withA(ink(), (uint8_t)(255 * a)); st.outlineW = size * 0.2f; st.shadow = size * 0.14f;
            g.print(f.x, f.y, size, st, f.s.c_str(), AL_CENTER);
        }

        const State& s = view;
        float mid = FX + W * C / 2;
        if (s.phase == PH_COUNTDOWN) {
            int n = s.phaseTimer / 60 + 1;
            char b[4]; snprintf(b, sizeof(b), "%d", std::min(3, n));
            float k = (s.phaseTimer % 60) / 60.0f;
            float size = 90 + 50 * k * k;
            head(mid, FY + H * C / 2 - size / 2, size, T.pop, b);
        } else if (s.phase == PH_PLAY && sawPlay && s.frame - playStartFrame < 50) {
            head(mid, FY + H * C / 2 - 30, 60, T.pop, "PUSH!");
        }

        if (s.phase == PH_OVER) {
            centerCard(250, 290);
            const char* hd;
            bool good = s.winner == viewer || mode == M_LOCAL;
            if (s.winner == 2) hd = "DRAW!";
            else if (mode == M_LOCAL) hd = s.winner == 0 ? "P1 WINS!" : "P2 WINS!";
            else hd = s.winner == viewer ? "YOU WIN!" : "YOU LOSE";
            head(mid, 282, 44, good ? T.pop : T.danger, hd);
            const char* why = s.reason == 1 ? "OPPONENT LEFT" : s.reason == 2 ? "BY FORFEIT" : "";
            body(mid, 348, 15, T.muted, why, AL_CENTER);
            bool canRematch = !(mode == M_HOST && peerGone);
            if (canRematch) {
                const char* r = s.pl[viewer].ready ? "WAITING FOR THEM..." : "PLAY AGAIN";
                if (mode == M_LOCAL) r = (s.pl[0].ready || s.pl[1].ready) ? "BOTH PRESS A..." : "PLAY AGAIN";
                button(mid, 384, 250, r, !s.pl[viewer].ready);
                if (mode != M_LOCAL && s.pl[1 - viewer].ready && !s.pl[viewer].ready)
                    body(mid, 458, 15, hex(0x46b98a), "THEY WANT A REMATCH!", AL_CENTER);
            }
            prompts({{"A", "REMATCH"}, {"B", "MENU"}}, 492);
        }

        if (paused) {
            g.rect(0, 0, SW, SH, withA(T.bg, 170));
            centerCard(220, 330);
            head(mid, 248, 40, white, "PAUSED");
            static const char* items[3] = {"RESUME", "RESTART", "QUIT TO MENU"};
            for (int i = 0; i < 3; ++i) button(mid, 320 + i * 72, 260, items[i], i == pauseSel);
        }
        if (forfeitAsk) {
            g.rect(0, 0, SW, SH, withA(T.bg, 140));
            centerCard(280, 210);
            head(mid, 310, 36, white, "GIVE UP?");
            body(mid, 372, 15, T.text, "THE OTHER PLAYER WINS THIS ROUND", AL_CENTER);
            prompts({{"A", "FORFEIT"}, {"B", "KEEP PLAYING"}}, 432);
        }
    }

    void drawTitleLogo(float cx, float y, float size) {
        const char* s = "SLIME PUSH";
        float w = g.measure(s, size, F_DISPLAY);
        float x = cx - w / 2;
        static const int cols[] = {5, 7, 2, 4, 3, 0, 6, 1, 5, 7};
        for (int i = 0; s[i]; ++i) {
            char ch[2] = {s[i], 0};
            float cw = g.measure(ch, size, F_DISPLAY);
            float wob = std::sin((float)t * 3 + i * 0.6f) * 6;
            if (s[i] != ' ') head(x, y + wob, size, pieceColor(cols[i] ? cols[i] : 2), ch, AL_LEFT, F_DISPLAY);
            x += cw;
        }
    }

    void drawTitle() {
        const Theme& T = th();
        drawBackground();
        drawTitleLogo(SW / 2, 42, 80);
        float hop = std::fabs(std::sin((float)t * 2.6f));
        g.ellipse(310, 660, 120 * (1 - hop * 0.3f), 16, withA(ink(), 50));
        g.slime(310, 650 - hop * 60, 260, pieceColor(5), ink(), (1 - hop) * 0.5f - 0.15f, blinkFor(1, 0), 0, (float)t);
        g.slime(455, 668, 110, pieceColor(6), ink(), std::sin((float)t * 5) * 0.15f, blinkFor(2, 0), 0, (float)t);
        static const char* items[7] = {"LAN BATTLE: HOST", "LAN BATTLE: JOIN", "VS CPU", "2 PLAYERS (2 PADS)", "HOW TO PLAY", "SETTINGS", "QUIT"};
        for (int i = 0; i < 7; ++i) button(880, 200 + i * 74, 400, items[i], i == sel);
        // player tag, bottom left
        g.rrect(20, SH - 62, g.measure(myName.c_str(), 14, F_BODY) + 40, 40, 20, border());
        g.rrect(24, SH - 58, g.measure(myName.c_str(), 14, F_BODY) + 32, 32, 16, T.panel);
        body(40, SH - 49, 14, T.text, myName.c_str());
        prompts({{"A", "SELECT"}});
    }

    void drawHost() {
        const Theme& T = th();
        drawBackground();
        banner("WAITING FOR A CHALLENGER", pieceColor(3));
        float hop = std::fabs(std::sin((float)t * 3));
        g.ellipse(SW / 2, 432, 100, 12, withA(ink(), 50));
        g.slime(SW / 2, 424 - hop * 40, 210, pieceColor(5), ink(), (1 - hop) * 0.4f, blinkFor(1, 0), 0, (float)t);
        panelBox(SW / 2 - 330, 470, 660, 230);
        std::string a = "YOU ARE " + myName;
        head(SW / 2, 494, 24, Col{255, 255, 255, 255}, a.c_str(), AL_CENTER, F_BODY);
        body(SW / 2, 548, 15, T.text, "ON THE OTHER DECK OR PC PICK  LAN BATTLE: JOIN", AL_CENTER);
        body(SW / 2, 576, 15, T.muted, "THIS ONE WILL SHOW UP THERE BY ITSELF", AL_CENTER);
        std::string ips;
        for (auto& ip : net::localAddresses()) { if (!ips.empty()) ips += "   "; ips += ip; }
        if (ips.empty()) ips = "NO NETWORK FOUND - CHECK WI-FI";
        body(SW / 2, 616, 15, T.muted, ("ADDRESS  " + ips).c_str(), AL_CENTER);
        int dots = (int)(t * 2) % 4;
        std::string w = "BROADCASTING"; for (int i = 0; i < dots; ++i) w += ".";
        body(SW / 2 - g.measure("BROADCASTING...", 15) / 2, 654, 15, hex(0x46b98a), w.c_str());
        prompts({{"B", "CANCEL"}});
    }

    void drawJoin() {
        const Theme& T = th();
        drawBackground();
        banner("FIND A GAME", pieceColor(6));
        if (joining) {
            float hop = std::fabs(std::sin((float)t * 4));
            g.slime(SW / 2, 420 - hop * 40, 200, pieceColor(6), ink(), (1 - hop) * 0.4f, 0, 2, (float)t);
            std::string s = "JOINING " + peerName + "...";
            head(SW / 2, 470, 26, Col{255, 255, 255, 255}, s.c_str(), AL_CENTER, F_BODY);
            prompts({{"B", "CANCEL"}});
            return;
        }
        auto& hs = finder.hosts;
        if (hs.empty()) {
            float k = std::sin((float)t * 2) * 0.2f;
            g.slime(SW / 2, 400, 200, pieceColor(6), ink(), k, blinkFor(4, 0), 1, (float)t);
            int dots = (int)(t * 2) % 4;
            const char* base = "LOOKING FOR SLIMES ON YOUR NETWORK";
            std::string s = base; for (int i = 0; i < dots; ++i) s += ".";
            head(SW / 2 - g.measure(base, 22, F_BODY) / 2, 450, 22, Col{255, 255, 255, 255}, s.c_str(), AL_LEFT, F_BODY);
            body(SW / 2, 508, 15, T.text, "ON THE OTHER DECK OR PC PICK  LAN BATTLE: HOST", AL_CENTER);
            body(SW / 2, 536, 15, T.muted, "BOTH NEED TO BE ON THE SAME NETWORK", AL_CENTER);
            prompts({{"B", "BACK"}});
        } else {
            for (size_t i = 0; i < hs.size() && i < 6; ++i) {
                std::string label = hs[i].name + (hs[i].busy ? "  (IN A GAME)" : "");
                button(SW / 2, 160 + (float)i * 78, 560, label.c_str(), (int)i == sel);
            }
            prompts({{"A", "JOIN"}, {"B", "BACK"}});
        }
    }

    void drawSettings() {
        drawBackground();
        banner("SETTINGS", pieceColor(4));
        std::string items[4] = {
            std::string("THEME:  ") + (cfg.theme ? "DARK" : "LIGHT"),
            std::string("SOUND:  ") + (cfg.sound ? "ON" : "OFF"),
            std::string("CPU:  ") + (cfg.cpu == 0 ? "EASY" : cfg.cpu == 1 ? "NORMAL" : "HARD"),
            "BACK"};
        for (int i = 0; i < 4; ++i) button(SW / 2, 190 + i * 86, 460, items[i].c_str(), i == sel);
        prompts({{"< >", "CHANGE"}, {"B", "BACK"}});
    }

    void drawHelp() {
        const Theme& T = th();
        drawBackground();
        banner("HOW TO PLAY", pieceColor(7));
        panelBox(110, 128, SW - 220, 530);
        const char* rules[] = {
            "YOU BOTH SHARE ONE TALL JAR OF SLIME.",
            "YOUR SLIMES DROP FROM THE TOP. THEIRS RISE FROM THE BOTTOM.",
            "FILL A ROW TO SPLAT IT. CLEAR 2+ ROWS AT ONCE TO SHOVE THE WHOLE",
            "PILE AT YOUR OPPONENT:  DOUBLE +1   TRIPLE +2   QUAD +4",
            "SLIME PUSHED OFF THEIR END, OR STACKED INTO THEIR OWN END, LOSES.",
            "A SLIME DROPPED DOWN AN EMPTY COLUMN PLOPS OUT AND IS GONE.",
        };
        float y = 156;
        head(146, y, 20, pieceColor(5), "RULES", AL_LEFT, F_DISPLAY);
        y += 40;
        for (const char* l : rules) { body(146, y, 15, T.text, l); y += 30; }
        y += 18;
        head(146, y, 20, pieceColor(6), "CONTROLS", AL_LEFT, F_DISPLAY);
        y += 42;
        const char* ctl[][2] = {
            {"D-PAD / STICK", "MOVE"}, {"DOWN", "SOFT DROP"}, {"UP", "HARD DROP"}, {"A", "SPIN RIGHT"},
            {"B / X", "SPIN LEFT"}, {"Y / L1 / R1", "HOLD"}, {"START", "PAUSE / FORFEIT"}};
        for (int i = 0; i < 7; ++i) {
            float cx = 146 + (i % 2) * 520, cy = y + (i / 2) * 40;
            float kw = g.measure(ctl[i][0], 12, F_BODY) + 26;
            g.rrect(cx - 4, cy - 8, kw + 8, 32, 16, border());
            g.rrect(cx, cy - 4, kw, 24, 12, T.field);
            body(cx + 13, cy + 2, 12, T.text, ctl[i][0]);
            body(cx + kw + 18, cy, 15, T.text, ctl[i][1]);
        }
        body(146, y + 4 * 40 + 6, 13, T.muted, "KEYBOARD: ARROWS, X / Z SPIN, C HOLD, SPACE HARD DROP, ENTER, ESC, F11 FULLSCREEN");
        prompts({{"B", "BACK"}});
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
            float w = g.measure(toast.c_str(), 16, F_BODY) + 60;
            float y = SH - 120;
            g.rrect(SW / 2 - w / 2 - 6, y - 6 + 8, w + 12, 60, 30, withA(ink(), 60));
            g.rrect(SW / 2 - w / 2 - 6, y - 6, w + 12, 60, 30, border());
            g.rrect(SW / 2 - w / 2, y, w, 48, 24, T.pop);
            body(SW / 2, y + 16, 16, T.slimeInk, toast.c_str(), AL_CENTER);
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
        else if (opt.screen == "settings") sc = SC_SETTINGS;
        else if (opt.screen == "help") sc = SC_HELP;

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
        else if (a == "--screen") app.opt.screen = next();
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
