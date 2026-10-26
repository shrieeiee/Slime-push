#include "net.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <windows.h>
  #include <direct.h>
  #include <vector>
  typedef int socklen_t;
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  static const intptr_t BAD = (intptr_t)INVALID_SOCKET;
  static void closeSock(intptr_t fd) { closesocket((SOCKET)fd); }
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  static const intptr_t BAD = -1;
  static void closeSock(intptr_t fd) { ::close((int)fd); }
#endif

namespace net {

static const char MAGIC[4] = {'S', 'L', 'M', 'P'};

static sockaddr_in toSock(const Addr& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(a.ip);
    s.sin_port = htons(a.port);
    return s;
}
static Addr fromSock(const sockaddr_in& s) { return Addr{ntohl(s.sin_addr.s_addr), ntohs(s.sin_port)}; }

// ---------------------------------------------------------------- platform
bool init() {
#ifdef _WIN32
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
#else
    return true;
#endif
}

void shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

std::string computerName() {
    char buf[256] = {0};
#ifdef _WIN32
    DWORD n = sizeof(buf);
    if (!GetComputerNameA(buf, &n)) buf[0] = 0;
#else
    gethostname(buf, sizeof(buf) - 1);
#endif
    std::string s = buf;
    for (auto& ch : s) ch = (char)toupper((unsigned char)ch);
    return s;
}

static void makeDir(const std::string& d) {
#ifdef _WIN32
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0755);
#endif
}

std::string configDir() {
    std::string dir;
#ifdef _WIN32
    const char* app = getenv("APPDATA");
    dir = app ? app : ".";
    dir += "\\SlimePush";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    dir = xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.config";
    makeDir(dir);
    dir += "/slimepush";
#endif
    makeDir(dir);
    return dir;
}

#ifdef _WIN32
// IPv4 unicast addresses of adapters that are up, with their prefix length
static std::vector<std::pair<uint32_t, int>> winAddrs() {
    std::vector<std::pair<uint32_t, int>> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return out;
    for (IP_ADAPTER_ADDRESSES* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
            out.push_back({ip, (int)u->OnLinkPrefixLength});
        }
    }
    return out;
}
#endif

std::vector<std::string> localAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    for (auto& a : winAddrs()) out.push_back(addrStr(Addr{a.first, 0}));
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
            out.push_back(addrStr(fromSock(*(sockaddr_in*)i->ifa_addr)));
        }
        freeifaddrs(ifs);
    }
#endif
    return out;
}

// ---------------------------------------------------------------- packets
void fillHdr(Hdr& h, PktType t) { std::memcpy(h.magic, MAGIC, 4); h.ver = VERSION; h.type = t; }

bool checkHdr(const void* buf, int len, PktType* t) {
    if (len < (int)sizeof(Hdr)) return false;
    const Hdr* h = (const Hdr*)buf;
    if (std::memcmp(h->magic, MAGIC, 4) != 0 || h->ver != VERSION) return false;
    *t = (PktType)h->type;
    return true;
}

// ---------------------------------------------------------------- sockets
bool Udp::open(uint16_t p, bool reuse, bool broadcast) {
    close();
    intptr_t s = (intptr_t)::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD) return false;
    fd = s;
    int one = 1;
    if (reuse) {
        setsockopt((decltype(::socket(0, 0, 0)))fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt((int)fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }
    if (broadcast) setsockopt((decltype(::socket(0, 0, 0)))fd, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    sockaddr_in a = toSock(Addr{INADDR_ANY, p});
    if (::bind((decltype(::socket(0, 0, 0)))fd, (sockaddr*)&a, sizeof(a)) != 0) { close(); return false; }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
    BOOL off = FALSE; DWORD ret = 0;   // stop ICMP "port unreachable" from erroring later recvs
    WSAIoctl((SOCKET)fd, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
#else
    fcntl((int)fd, F_SETFL, fcntl((int)fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    socklen_t l = sizeof(a);
    getsockname((decltype(::socket(0, 0, 0)))fd, (sockaddr*)&a, &l);
    port = ntohs(a.sin_port);
    return true;
}

void Udp::close() { if (fd != BAD) closeSock(fd); fd = BAD; }

int Udp::recv(void* buf, int cap, Addr* from) {
    if (fd == BAD) return -1;
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    int n = (int)::recvfrom((decltype(::socket(0, 0, 0)))fd, (char*)buf, cap, 0, (sockaddr*)&a, &l);
    if (n > 0 && from) *from = fromSock(a);
    return n;
}

void Udp::send(const void* buf, int len, const Addr& to) {
    if (fd == BAD) return;
    sockaddr_in a = toSock(to);
    ::sendto((decltype(::socket(0, 0, 0)))fd, (const char*)buf, len, 0, (const sockaddr*)&a, sizeof(a));
}

Addr makeAddr(const char* ip, uint16_t port) {
    sockaddr_in a{};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return Addr{ntohl(a.sin_addr.s_addr), port};
}

std::string addrStr(const Addr& a) {
    char b[32];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", (a.ip >> 24) & 255, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255);
    return b;
}

std::vector<Addr> broadcastTargets(uint16_t port) {
    std::vector<Addr> out;
    auto add = [&](uint32_t ip) {
        for (auto& o : out) if (o.ip == ip) return;
        out.push_back(Addr{ip, port});
    };
    add(0xFFFFFFFFu);
#ifdef _WIN32
    // Windows only sends 255.255.255.255 out of one adapter, so also aim at each subnet
    for (auto& a : winAddrs()) {
        if (a.second <= 0 || a.second >= 32) continue;
        uint32_t mask = 0xFFFFFFFFu << (32 - a.second);
        add(a.first | ~mask);
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP)) continue;
            if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
                add(fromSock(*(sockaddr_in*)i->ifa_broadaddr).ip);
        }
        freeifaddrs(ifs);
    }
#endif
    add(0x7F000001u);   // lets two copies on one machine find each other
    return out;
}

// ---------------------------------------------------------------- discovery
bool Finder::start() {
    hosts.clear();
    return sock.open(DISC_PORT, true, true);
}

void Finder::stop() { sock.close(); hosts.clear(); }

void Finder::poll(double now, uint32_t ownHostId) {
    unsigned char buf[2048];
    Addr from;
    int n;
    while ((n = sock.recv(buf, sizeof(buf), &from)) > 0) {
        PktType t;
        if (!checkHdr(buf, n, &t) || t != P_BEACON || n < (int)sizeof(Beacon)) continue;
        const Beacon* b = (const Beacon*)buf;
        if (b->hostId == ownHostId) continue;
        Addr ga{from.ip, b->port};
        char nm[25]; std::memcpy(nm, b->name, 24); nm[24] = 0;
        FoundHost* f = nullptr;
        for (auto& h : hosts) if (h.hostId == b->hostId) { f = &h; break; }
        if (!f) { hosts.push_back(FoundHost{}); f = &hosts.back(); }
        // prefer a real LAN address over loopback when both arrive
        if (f->lastSeen == 0 || !from.loopback() || f->addr.loopback()) f->addr = ga;
        f->name = nm; f->busy = b->busy != 0; f->hostId = b->hostId; f->lastSeen = now;
    }
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [&](const FoundHost& h) { return now - h.lastSeen > 3.0; }), hosts.end());
}

} // namespace net
