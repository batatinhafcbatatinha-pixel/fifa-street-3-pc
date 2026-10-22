// Xbox LIVE / network layer for the Xbox 360 ports (master copy: work/xbox-online; copied into
// each port's src/). The SDK's network exports are stubs that tell the game there is no network
// cable (XNetGetEthernetLinkStatus = 0), no address and no DNS. With online play switched on
// (ini [opcoes] online=1 or PCS_ONLINE=1) these definitions take their place:
//
//   - the link is up and the console's address is this PC's LAN address (XNADDR from the host);
//   - XNet addresses map 1:1 to IPv4 (no Xbox LIVE security association: XnAddr <-> in_addr);
//   - DNS names resolve through the host, except EA's service hosts, which go to the local
//     server (PCS_ONLINE_SERVER, default 127.0.0.1) - the original servers are gone;
//   - QoS probes answer at once with a good link, UDP receive works (WSARecvFrom);
//   - the profile is "signed in to Xbox LIVE" with online privileges.
//
// Every call is logged (first occurrences) so the protocol the game speaks can be studied.
// With online play off, each export behaves like the SDK's stub.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

using rex::system::XEvent;
using rex::system::XSocket;
using rex::system::XThread;
using rex::X_HRESULT;
using rex::X_RESULT;

// ---------------------------------------------------------------------------- settings

bool OnlineEnabled() {
  static const bool on = [] {
    if (const char* e = std::getenv("PCS_ONLINE"); e && *e) return *e != '0';
    wchar_t ini[MAX_PATH];
    GetModuleFileNameW(nullptr, ini, MAX_PATH);
    if (wchar_t* slash = wcsrchr(ini, L'\\')) *slash = 0;
    // Any *-pc.ini next to the executable (the port's settings file).
    WIN32_FIND_DATAW fd;
    std::wstring pattern = std::wstring(ini) + L"\\*-pc.ini";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    bool enabled = false;
    if (find != INVALID_HANDLE_VALUE) {
      std::wstring path = std::wstring(ini) + L"\\" + fd.cFileName;
      enabled = GetPrivateProfileIntW(L"opcoes", L"online", 0, path.c_str()) != 0;
      FindClose(find);
    }
    return enabled;
  }();
  return on;
}

// IPv4 (network order) of the local server that answers for EA's hosts.
uint32_t ServerAddress() {
  static const uint32_t address = [] {
    const char* e = std::getenv("PCS_ONLINE_SERVER");
    in_addr a{};
    if (!e || !*e || inet_pton(AF_INET, e, &a) != 1) a.s_addr = htonl(INADDR_LOOPBACK);
    return uint32_t(a.s_addr);
  }();
  return address;
}

struct HostIdentity {
  uint32_t ip = htonl(INADDR_LOOPBACK);  // network order
  uint8_t mac[6] = {0x00, 0x22, 0x48, 0x00, 0x00, 0x01};
};

// The first IPv4 adapter that is up and not loopback.
const HostIdentity& Host() {
  static const HostIdentity id = [] {
    HostIdentity h;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, list, &size) == NO_ERROR) {
      for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstUnicastAddress)
          continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(a->FirstUnicastAddress->Address.lpSockaddr);
        h.ip = sin->sin_addr.s_addr;
        if (a->PhysicalAddressLength == 6) std::memcpy(h.mac, a->PhysicalAddress, 6);
        break;
      }
    }
    char text[32];
    inet_ntop(AF_INET, &h.ip, text, sizeof(text));
    REXLOG_INFO("[online] console address {} (MAC {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X})", text, h.mac[0],
                h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
    return h;
  }();
  return id;
}

// ------------------------------------------------------------------------------ helpers

uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
void Store32(uint8_t* base, uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, a), v); }
void Store16(uint8_t* base, uint32_t a, uint16_t v) { rex::memory::store_and_swap<uint16_t>(rex::memory::GuestPtr(base, a), v); }

// Argument n (0 = r3) of a guest call; from the 9th on they are on the stack.
uint32_t Arg(PPCContext& ctx, uint8_t* base, int n) {
  switch (n) {
    case 0: return ctx.r3.u32;
    case 1: return ctx.r4.u32;
    case 2: return ctx.r5.u32;
    case 3: return ctx.r6.u32;
    case 4: return ctx.r7.u32;
    case 5: return ctx.r8.u32;
    case 6: return ctx.r9.u32;
    case 7: return ctx.r10.u32;
  }
  return Load32(base, ctx.r1.u32 + 0x54 + (n - 8) * 8);
}

void SignalEvent(uint32_t handle) {
  if (!handle) return;
  if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(handle)) ev->Set(0, false);
}

// Logs the first `limit` occurrences of each message key.
bool LogOnce(const char* key, int limit = 8) {
  static std::mutex mu;
  static std::vector<std::pair<std::string, int>> seen;
  std::lock_guard<std::mutex> lock(mu);
  for (auto& [k, n] : seen)
    if (k == key) return ++n <= limit;
  seen.emplace_back(key, 1);
  return true;
}

std::string GuestString(uint8_t* base, uint32_t address, size_t max = 256) {
  if (!address) return {};
  const char* p = reinterpret_cast<const char*>(rex::memory::GuestPtr(base, address));
  return std::string(p, strnlen(p, max));
}

bool IsEaHost(const std::string& host) {
  auto ends = [&](const char* s) {
    size_t n = std::strlen(s);
    return host.size() >= n && _stricmp(host.c_str() + host.size() - n, s) == 0;
  };
  return ends(".ea.com") || ends(".easports.com") || ends(".easo.ea.com") || ends("gamespy.com");
}

// XNADDR: +0 ina, +4 inaOnline, +8 wPortOnline, +10 abEnet[6], +16 abOnline[20].
void FillXnAddr(uint8_t* base, uint32_t xnaddr, uint32_t ip) {
  uint8_t* p = rex::memory::GuestPtr(base, xnaddr);
  std::memset(p, 0, 36);
  std::memcpy(p + 0, &ip, 4);  // in_addr is stored in network order, as on the console
  std::memcpy(p + 4, &ip, 4);
  Store16(base, xnaddr + 8, 3074);
  std::memcpy(p + 10, Host().mac, 6);
  std::memcpy(p + 16, &ip, 4);  // abOnline: enough to tell consoles apart
}

constexpr uint32_t kXnAddrEthernet = 0x02, kXnAddrStatic = 0x04, kXnAddrGateway = 0x20, kXnAddrDns = 0x40,
                   kXnAddrOnline = 0x80;

}  // namespace

// ----------------------------------------------------------------------------- XNet

// XNetGetEthernetLinkStatus(caller): active, 100 Mbps, full duplex.
REX_HOOK_RAW(__imp__NetDll_XNetGetEthernetLinkStatus) {
  ctx.r3.u64 = OnlineEnabled() ? 0x01 | 0x02 | 0x08 : 0;
}

// XNetGetTitleXnAddr(caller, XNADDR*)
REX_HOOK_RAW(__imp__NetDll_XNetGetTitleXnAddr) {
  const uint32_t xnaddr = ctx.r4.u32;
  if (!OnlineEnabled()) {  // the SDK's answer: loopback, MAC CC:CC:CC:CC:CC:CC
    uint8_t* p = rex::memory::GuestPtr(base, xnaddr);
    std::memset(p, 0, 36);
    uint32_t loopback = htonl(INADDR_LOOPBACK);
    std::memcpy(p, &loopback, 4);
    std::memset(p + 10, 0xCC, 6);
    ctx.r3.u64 = kXnAddrStatic;
    return;
  }
  FillXnAddr(base, xnaddr, Host().ip);
  if (LogOnce("xnaddr", 1)) REXLOG_INFO("[online] XNetGetTitleXnAddr");
  ctx.r3.u64 = kXnAddrEthernet | kXnAddrStatic | kXnAddrGateway | kXnAddrDns | kXnAddrOnline;
}

// XNetXnAddrToInAddr(caller, const XNADDR*, const XNKID*, IN_ADDR* out)
REX_HOOK_RAW(__imp__NetDll_XNetXnAddrToInAddr) {
  const uint32_t xnaddr = ctx.r4.u32, out = ctx.r6.u32;
  if (!OnlineEnabled() || !xnaddr || !out) {
    ctx.r3.u64 = 1;
    return;
  }
  std::memcpy(rex::memory::GuestPtr(base, out), rex::memory::GuestPtr(base, xnaddr), 4);
  ctx.r3.u64 = 0;
}

// XNetInAddrToXnAddr(caller, IN_ADDR, XNADDR* out, XNKID* out)
REX_HOOK_RAW(__imp__NetDll_XNetInAddrToXnAddr) {
  const uint32_t ina = _byteswap_ulong(ctx.r4.u32), xnaddr = ctx.r5.u32, xnkid = ctx.r6.u32;
  if (!OnlineEnabled() || !xnaddr) {
    ctx.r3.u64 = 1;
    return;
  }
  FillXnAddr(base, xnaddr, ina);
  if (xnkid) std::memset(rex::memory::GuestPtr(base, xnkid), 0, 8);
  ctx.r3.u64 = 0;
}

// XNetServerToInAddr(caller, IN_ADDR server, DWORD service_id, IN_ADDR* out): Xbox LIVE
// service (XLSP) addresses; EA's services are reached through the local server.
REX_HOOK_RAW(__imp__NetDll_XNetServerToInAddr) {
  const uint32_t service = ctx.r5.u32, out = ctx.r6.u32;
  if (LogOnce("server2inaddr")) REXLOG_INFO("[online] XNetServerToInAddr(service {:08X})", service);
  if (!OnlineEnabled() || !out) {
    ctx.r3.u64 = 1;
    return;
  }
  uint32_t server = ServerAddress();
  std::memcpy(rex::memory::GuestPtr(base, out), &server, 4);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__NetDll_XNetConnect) { ctx.r3.u64 = 0; }

// XNetGetConnectStatus(caller, IN_ADDR): 2 = connected.
REX_HOOK_RAW(__imp__NetDll_XNetGetConnectStatus) { ctx.r3.u64 = OnlineEnabled() ? 2 : 0; }

REX_HOOK_RAW(__imp__NetDll_XNetRegisterKey) { ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__NetDll_XNetUnregisterInAddr) { ctx.r3.u64 = 0; }

// XNetDnsLookup(caller, const char* name, WSAEVENT, XNDNS** out). XNDNS: +0 status, +4 count,
// +8 addresses[8].
REX_HOOK_RAW(__imp__NetDll_XNetDnsLookup) {
  const std::string host = GuestString(base, ctx.r4.u32);
  const uint32_t event = ctx.r5.u32, out = ctx.r6.u32;
  auto* memory = REX_KERNEL_MEMORY();
  uint32_t dns = memory->SystemHeapAlloc(40);
  std::memset(rex::memory::GuestPtr(base, dns), 0, 40);
  std::vector<uint32_t> found;
  if (OnlineEnabled()) {
    if (IsEaHost(host)) {
      found.push_back(ServerAddress());
    } else {
      addrinfo hints{}, *result = nullptr;
      hints.ai_family = AF_INET;
      if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0) {
        for (auto* r = result; r && found.size() < 8; r = r->ai_next)
          found.push_back(reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(result);
      }
    }
  }
  REXLOG_INFO("[online] DNS {} -> {} address(es){}", host, found.size(),
              IsEaHost(host) && OnlineEnabled() ? " (local server)" : "");
  Store32(base, dns, found.empty() ? 1 : 0);
  Store32(base, dns + 4, uint32_t(found.size()));
  for (size_t i = 0; i < found.size(); ++i) std::memcpy(rex::memory::GuestPtr(base, dns + 8 + uint32_t(i) * 4), &found[i], 4);
  if (out) Store32(base, out, dns);
  SignalEvent(event);
  ctx.r3.u64 = 0;
}

// XNetQosLookup(caller, cxna, apxna, apxnkid, apxnkey, cina, aina, aservice, probes, bps, flags,
// event, XNQOS** out). Every target answers at once: contacted, 20 ms, 10 Mbit/s.
// XNQOS: +0 count, +4 pending, then 24-byte XNQOSINFO entries (+0 flags, +2 xmit, +4 recv,
// +6 data length, +8 data, +12 rtt min, +14 rtt med, +16 up bps, +20 down bps).
REX_HOOK_RAW(__imp__NetDll_XNetQosLookup) {
  const uint32_t cxna = Arg(ctx, base, 1), cina = Arg(ctx, base, 5);
  const uint32_t event = Arg(ctx, base, 11), out = Arg(ctx, base, 12);
  const uint32_t count = OnlineEnabled() ? cxna + cina : 0;
  const uint32_t size = 8 + std::max<uint32_t>(count, 1) * 24;
  uint32_t qos = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  std::memset(rex::memory::GuestPtr(base, qos), 0, size);
  Store32(base, qos, count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t e = qos + 8 + i * 24;
    *rex::memory::GuestPtr(base, e) = 0x01 | 0x02;  // XNET_XNQOSINFO_COMPLETE | TARGET_CONTACTED
    Store16(base, e + 2, 4);
    Store16(base, e + 4, 4);
    Store16(base, e + 12, 20);
    Store16(base, e + 14, 20);
    Store32(base, e + 16, 10'000'000);
    Store32(base, e + 20, 10'000'000);
  }
  if (out) Store32(base, out, qos);
  if (LogOnce("qos")) REXLOG_INFO("[online] XNetQosLookup({} consoles, {} servers)", cxna, cina);
  SignalEvent(event);
  ctx.r3.u64 = 0;
}

// WSARecvFrom(caller, socket, WSABUF*, count, DWORD* received, DWORD* flags, sockaddr* from,
// int* fromlen, overlapped, completion). Non-overlapped receive into the first buffer.
REX_HOOK_RAW(__imp__NetDll_WSARecvFrom) {
  const uint32_t handle = Arg(ctx, base, 1), buffers = Arg(ctx, base, 2), received = Arg(ctx, base, 4);
  const uint32_t flags_ptr = Arg(ctx, base, 5), from = Arg(ctx, base, 6), fromlen = Arg(ctx, base, 7);
  const uint32_t overlapped = Arg(ctx, base, 8);
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  if (overlapped && LogOnce("recvfrom-overlapped", 3))
    REXLOG_WARN("[online] WSARecvFrom with OVERLAPPED: answered synchronously");
  const uint32_t len = Load32(base, buffers), buf = Load32(base, buffers + 4);
  rex::system::N_XSOCKADDR_IN native_from{};
  uint32_t native_len = fromlen ? Load32(base, fromlen) : 0;
  uint32_t flags = flags_ptr ? Load32(base, flags_ptr) : 0;
  int n = socket->RecvFrom(rex::memory::GuestPtr(base, buf), len, flags, &native_from, fromlen ? &native_len : nullptr);
  if (n < 0) {
    XThread::SetLastError(WSAGetLastError());
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  if (received) Store32(base, received, uint32_t(n));
  if (from) {
    auto* x = reinterpret_cast<rex::system::XSOCKADDR_IN*>(rex::memory::GuestPtr(base, from));
    x->sin_family = native_from.sin_family;
    x->sin_port = native_from.sin_port;
    x->sin_addr = native_from.sin_addr;
    std::memset(x->x_sin_zero, 0, sizeof(x->x_sin_zero));
  }
  if (fromlen) Store32(base, fromlen, native_len);
  ctx.r3.u64 = 0;
}

// -------------------------------------------------------------------- Xbox LIVE profile

// XamUserGetSigninState(user): 2 = signed in to Xbox LIVE.
REX_HOOK_RAW(__imp__XamUserGetSigninState) {
  const uint32_t user = ctx.r3.u32;
  if (OnlineEnabled() && user == 0) {
    ctx.r3.u64 = 2;
    return;
  }
  ctx.r3.u64 = user == 0 ? 1 : 0;  // the SDK's answer: user 0 signed in locally
}

// XamUserCheckPrivilege(user, mask, BOOL* result): every privilege granted.
REX_HOOK_RAW(__imp__XamUserCheckPrivilege) {
  const uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
  if (user != 0xFF && user >= 4) {
    ctx.r3.u64 = 0x57;  // X_ERROR_INVALID_PARAMETER
    return;
  }
  if (user != 0xFF && user) {
    ctx.r3.u64 = 0x525;  // X_ERROR_NO_SUCH_USER
    return;
  }
  if (out) Store32(base, out, OnlineEnabled() ? 1 : 0);
  if (OnlineEnabled() && LogOnce("privilege", 4)) REXLOG_INFO("[online] privilege {:X} granted", ctx.r4.u32);
  ctx.r3.u64 = 0;
}

// connect(caller, socket, sockaddr*, namelen): as the SDK, plus a log of where the game goes.
REX_HOOK_RAW(__imp__NetDll_connect) {
  const uint32_t handle = ctx.r4.u32, name = ctx.r5.u32, namelen = ctx.r6.u32;
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  rex::system::N_XSOCKADDR native(reinterpret_cast<const rex::system::XSOCKADDR*>(rex::memory::GuestPtr(base, name)));
  if (OnlineEnabled()) {
    const uint8_t* a = rex::memory::GuestPtr(base, name);
    REXLOG_INFO("[online] connect to {}.{}.{}.{}:{}", a[4], a[5], a[6], a[7], (a[2] << 8) | a[3]);
  }
  if (XFAILED(socket->Connect(&native, int(namelen)))) {
    XThread::SetLastError(WSAGetLastError());
    ctx.r3.u64 = uint64_t(-1);
    return;
  }
  ctx.r3.u64 = 0;
}

namespace {
// The profile's XUID for Xbox LIVE: online XUIDs start with 0x0009; the low bits come from this
// PC's MAC address, so two PCs on a LAN are different players.
uint64_t OnlineXuid() {
  uint64_t x = 0x0009000000000000ull;
  for (int i = 0; i < 6; ++i) x |= uint64_t(Host().mac[i]) << (8 * (5 - i));
  return x;
}

void Store64(uint8_t* base, uint32_t a, uint64_t v) { rex::memory::store_and_swap<uint64_t>(rex::memory::GuestPtr(base, a), v); }
}  // namespace

// XamUserGetSigninInfo(user, flags, X_USER_SIGNIN_INFO*): +0 xuid, +12 state, +24 name[16].
REX_HOOK_RAW(__imp__XamUserGetSigninInfo) {
  const uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  if (!info) {
    ctx.r3.u64 = 0x80070057;  // X_E_INVALIDARG
    return;
  }
  std::memset(rex::memory::GuestPtr(base, info), 0, 40);
  if (user) {
    ctx.r3.u64 = 0x80070525;  // X_E_NO_SUCH_USER
    return;
  }
  const auto* profile = REX_KERNEL_STATE()->user_profile();
  Store64(base, info, OnlineEnabled() ? OnlineXuid() : profile->xuid());
  Store32(base, info + 12, OnlineEnabled() ? 2 : profile->signin_state());
  std::strncpy(reinterpret_cast<char*>(rex::memory::GuestPtr(base, info + 24)), profile->name().c_str(), 15);
  if (OnlineEnabled() && LogOnce("signininfo", 2)) REXLOG_INFO("[online] signed in to Xbox LIVE as {:016X}", OnlineXuid());
  ctx.r3.u64 = 0;
}

// XamUserGetXUID(user, type_mask, u64* out): mask 2/4 = online XUID.
REX_HOOK_RAW(__imp__XamUserGetXUID) {
  const uint32_t user = ctx.r3.u32, mask = ctx.r4.u32, out = ctx.r5.u32;
  if (!out) {
    ctx.r3.u64 = 0x80070057;
    return;
  }
  uint64_t xuid = 0;
  uint32_t result = 0x80070525;
  if (user == 0) {
    const auto* profile = REX_KERNEL_STATE()->user_profile();
    uint32_t type = profile->type() & mask;
    if (OnlineEnabled() && (mask & (2 | 4))) {
      xuid = OnlineXuid();
      result = 0;
    } else if (type & (1 | 2 | 4)) {
      xuid = profile->xuid();
      result = 0;
    }
  } else if (user >= 4) {
    result = 0x80070057;
  }
  Store64(base, out, xuid);
  ctx.r3.u64 = result;
}

// ------------------------------------------------------------- Xbox LIVE service messages

namespace {

constexpr uint32_t kXLiveBaseApp = 0xFC;
constexpr uint32_t kGetServiceInfo = 0x00058007;

// The xlivebase messages online play needs that the SDK refuses. Returns true when handled.
// GetServiceInfo(service id, XONLINE_SERVICE_INFO*): the address of an Xbox LIVE Server
// Platform (XLSP) service - EA's - is the local server's. XONLINE_SERVICE_INFO: +0 id,
// +4 IN_ADDR, +8 port.
bool HandleLiveMessage(uint8_t* base, uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2,
                       X_HRESULT* result) {
  if (!OnlineEnabled() || app != kXLiveBaseApp) return false;
  if (message == kGetServiceInfo && arg2) {
    uint32_t server = ServerAddress();
    Store32(base, arg2, arg1);
    std::memcpy(rex::memory::GuestPtr(base, arg2 + 4), &server, 4);
    Store16(base, arg2 + 8, 0);
    Store16(base, arg2 + 10, 0);
    char text[32];
    inet_ntop(AF_INET, &server, text, sizeof(text));
    REXLOG_INFO("[online] Xbox LIVE service {:08X} -> {}", arg1, text);
    *result = 0;
    return true;
  }
  return false;
}

X_HRESULT StartIoRequest(uint8_t* base, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
                         uint32_t length) {
  X_HRESULT result;
  if (!HandleLiveMessage(base, app, message, buffer, length, &result)) {
    result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer, length);
    if (result == X_E_NOTFOUND) {
      result = X_E_INVALIDARG;
      XThread::SetLastError(X_ERROR_NOT_FOUND);
    }
  }
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, result);
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) XThread::SetLastError(0);
  return result;
}

}  // namespace

// XMsgStartIORequest(app, message, overlapped, buffer, length)
REX_HOOK_RAW(__imp__XMsgStartIORequest) {
  ctx.r3.u64 = StartIoRequest(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

// XMsgStartIORequestEx(app, message, overlapped, buffer, length, unknown)
REX_HOOK_RAW(__imp__XMsgStartIORequestEx) {
  ctx.r3.u64 = StartIoRequest(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

// XMsgInProcessCall(app, message, arg1, arg2)
REX_HOOK_RAW(__imp__XMsgInProcessCall) {
  X_HRESULT result;
  if (!HandleLiveMessage(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, &result))
    result = REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  ctx.r3.u64 = result;
}
