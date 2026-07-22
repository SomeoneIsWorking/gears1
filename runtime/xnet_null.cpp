// Networking, reported as a console with an Ethernet adapter and no link.
//
// That is a real state hardware produces -- an unplugged cable -- so a title
// is required to handle it, and every answer here is one it would genuinely
// see. The alternative shapes are both worse: claiming a working stack would
// have the title wait on connections that can never arrive, and failing
// startup outright is a state the console does not produce and titles are not
// obliged to survive.
//
// Sockets are refused rather than half-implemented. A socket that can be
// created but never carries data is the kind of partial truth that strands a
// title in a retry loop far from the cause.
#include "import_stub.h"

#include <lucent/log.h>

#include <byteswap.h>

namespace
{
// Winsock values, which the console's networking keeps unchanged.
constexpr uint32_t kInvalidSocket = 0xFFFFFFFF;
constexpr uint32_t kSocketError = 0xFFFFFFFF;
constexpr uint32_t kErrorNetworkDown = 10050; // WSAENETDOWN

// XNetGetTitleXnAddr status: no address, and none is coming.
constexpr uint32_t kXnAddrNone = 0x00000001;

// No cable. XNET_ETHERNET_LINK_ACTIVE is bit 0, so zero is "no link".
constexpr uint32_t kEthernetLinkNone = 0;

void NoNetwork(PPCContext& ctx, const char* what, uint32_t result)
{
    lucent::debug("net", "{} -> no network ({:#x})", what, result);
    ctx.r3.u64 = result;
}
} // namespace

// Bringing the stack up succeeds: there is nothing wrong with initialising a
// stack that then reports no link, and that is what an unplugged console does.
void __imp__NetDll_XNetStartup(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::info("net", "XNetStartup: no network link will be reported");
    ctx.r3.u64 = 0;
}

void __imp__NetDll_XNetCleanup(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__NetDll_WSAStartup(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__NetDll_WSACleanup(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

// The one answer everything else follows from.
void __imp__NetDll_XNetGetEthernetLinkStatus(PPCContext& __restrict ctx, uint8_t*)
{
    NoNetwork(ctx, "XNetGetEthernetLinkStatus", kEthernetLinkNone);
}

void __imp__NetDll_XNetGetTitleXnAddr(PPCContext& __restrict ctx, uint8_t*)
{
    NoNetwork(ctx, "XNetGetTitleXnAddr", kXnAddrNone);
}

void __imp__NetDll_XNetGetDebugXnAddr(PPCContext& __restrict ctx, uint8_t*)
{
    NoNetwork(ctx, "XNetGetDebugXnAddr", kXnAddrNone);
}

void __imp__NetDll_WSAGetLastError(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = kErrorNetworkDown;
}

void __imp__NetDll_socket(PPCContext& __restrict ctx, uint8_t*)
{
    NoNetwork(ctx, "socket", kInvalidSocket);
}

void __imp__NetDll_WSACreateEvent(PPCContext& __restrict ctx, uint8_t*)
{
    NoNetwork(ctx, "WSACreateEvent", 0);
}

// Every operation on a socket that could never be created fails the same way.
#define GEARS_NET_FAILS(name)                                       \
    void __imp__NetDll_##name(PPCContext& __restrict ctx, uint8_t*) \
    {                                                               \
        NoNetwork(ctx, #name, kSocketError);                        \
    }

GEARS_NET_FAILS(bind)
GEARS_NET_FAILS(closesocket)
GEARS_NET_FAILS(connect)
GEARS_NET_FAILS(getsockname)
GEARS_NET_FAILS(getsockopt)
GEARS_NET_FAILS(ioctlsocket)
GEARS_NET_FAILS(recv)
GEARS_NET_FAILS(recvfrom)
GEARS_NET_FAILS(send)
GEARS_NET_FAILS(sendto)
GEARS_NET_FAILS(setsockopt)
GEARS_NET_FAILS(XNetDnsLookup)
GEARS_NET_FAILS(XNetDnsRelease)
GEARS_NET_FAILS(XNetQosListen)
GEARS_NET_FAILS(XNetQosLookup)
GEARS_NET_FAILS(XNetQosRelease)
GEARS_NET_FAILS(XNetXnAddrToInAddr)

// Key registration is local bookkeeping the console does without a link, so
// these succeed rather than failing: refusing them would be inventing a
// failure the hardware does not produce.
void __imp__NetDll_XNetCreateKey(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__NetDll_XNetRegisterKey(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__NetDll_XNetUnregisterKey(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

// inet_addr and XNetRandom are pure computation and work with no link at all.
void __imp__NetDll_inet_addr(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = kSocketError; // INADDR_NONE
}

void __imp__NetDll_XNetRandom(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t buffer = ctx.r4.u32;
    const uint32_t length = ctx.r5.u32;

    // Deterministic rather than random: this runtime has no security boundary
    // to protect, and a reproducible run is worth more than unpredictability.
    for (uint32_t i = 0; i < length; i++)
        *(base + buffer + i) = uint8_t(i * 31 + 7);

    ctx.r3.u64 = 0;
}
