// LAN networking: UDP broadcast discovery + host-authoritative game sync.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "game.hpp"

namespace net {

constexpr uint16_t DISC_PORT = 47790;   // hosts broadcast beacons here
constexpr uint16_t GAME_PORT = 47791;   // host listens here (or the next free port)
constexpr uint8_t  VERSION   = 4;

enum PktType : uint8_t { P_BEACON = 1, P_JOIN, P_WELCOME, P_INPUT, P_STATE, P_BYE, P_FULL };

#pragma pack(push, 1)
struct Hdr { char magic[4]; uint8_t ver; uint8_t type; };
struct Beacon  { Hdr h; char name[24]; uint16_t port; uint8_t busy; uint32_t hostId; };
struct Join    { Hdr h; char name[24]; uint32_t nonce; uint8_t speed; };
struct Welcome { Hdr h; char name[24]; uint32_t nonce; };
struct InputPkt {
    Hdr h; uint32_t nonce; uint32_t seq;
    uint16_t held; uint8_t counts[16];   // press counters per action bit
};
struct StatePkt { Hdr h; uint32_t nonce; uint32_t seq; sp::State st; };
struct Bye     { Hdr h; uint32_t nonce; };
#pragma pack(pop)

// IPv4 address + port, host byte order (keeps OS socket headers out of the game code)
struct Addr {
    uint32_t ip = 0; uint16_t port = 0;
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    bool loopback() const { return (ip >> 24) == 127; }
};

// platform helpers (Linux/SteamOS and Windows)
bool init();
void shutdown();
std::string computerName();
std::string configDir();                 // created if missing
std::vector<std::string> localAddresses();

void fillHdr(Hdr& h, PktType t);
bool checkHdr(const void* buf, int len, PktType* t);

class Udp {
public:
    intptr_t fd = -1;
    uint16_t port = 0;
    bool open(uint16_t port, bool reuse, bool broadcast);
    void close();
    int  recv(void* buf, int cap, Addr* from);
    void send(const void* buf, int len, const Addr& to);
    ~Udp() { close(); }
};

std::vector<Addr> broadcastTargets(uint16_t port);
std::string addrStr(const Addr& a);
Addr makeAddr(const char* ip, uint16_t port);

struct FoundHost {
    std::string name;
    Addr addr;             // game address
    bool busy = false;
    uint32_t hostId = 0;
    double lastSeen = 0;
};

// Listens for beacons on the LAN
class Finder {
public:
    std::vector<FoundHost> hosts;
    bool start();
    void stop();
    void poll(double now, uint32_t ownHostId);
private:
    Udp sock;
};

// Packs local controller edges into counters so lost packets never drop a press
class InputSender {
public:
    uint8_t counts[16] = {};
    uint16_t held = 0;
    void update(const sp::Input& in) {
        held = in.held;
        for (int b = 0; b < 16; ++b) if (in.pressed & (1u << b)) counts[b]++;
    }
};
class InputReceiver {
public:
    uint8_t last[16] = {};
    bool primed = false;
    sp::Input pending;    // accumulated until the sim consumes it
    void feed(const InputPkt& p) {
        if (!primed) { for (int b = 0; b < 16; ++b) last[b] = p.counts[b]; primed = true; }
        pending.held = p.held;
        for (int b = 0; b < 16; ++b) if (p.counts[b] != last[b]) { pending.pressed |= (uint16_t)(1u << b); last[b] = p.counts[b]; }
    }
    sp::Input take() { sp::Input r = pending; pending.pressed = 0; return r; }
    void reset() { primed = false; pending = {}; }
};

} // namespace net
