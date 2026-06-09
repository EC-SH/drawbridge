#include <gtest/gtest.h>
#include <string>
#include "ArpLookup.hpp"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// ArpLookup host-build behaviour. Off-device there is no ARP table, so
// pdLookupMac() is a stub that always returns std::nullopt — which the
// Learn-mode caller treats as a (permanent) first-packet miss: accept + defer
// the MAC-lock. We assert that contract here plus the pure toHex12() formatter.

TEST(ArpLookup, HostStubReturnsNullopt)
{
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = inet_addr("192.168.12.42");
    EXPECT_FALSE(ArpLookup::pdLookupMac(s).has_value());
}

TEST(ArpLookup, HostStubNulloptForNonInet)
{
    sockaddr_in s{};
    s.sin_family = 0;  // not AF_INET
    EXPECT_FALSE(ArpLookup::pdLookupMac(s).has_value());
}

TEST(ArpLookup, ToHex12FormatsLowercaseNoSeparators)
{
    ArpLookup::Mac mac = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
    EXPECT_EQ(ArpLookup::toHex12(mac), "a1b2c3d4e5f6");
}

TEST(ArpLookup, ToHex12ZeroPadsEachByte)
{
    ArpLookup::Mac mac = {0x00, 0x01, 0x0a, 0x00, 0xff, 0x0f};
    // Each byte must render as exactly two hex chars (leading zeros kept).
    std::string out = ArpLookup::toHex12(mac);
    EXPECT_EQ(out.size(), 12u);
    EXPECT_EQ(out, "00010a00ff0f");
}
