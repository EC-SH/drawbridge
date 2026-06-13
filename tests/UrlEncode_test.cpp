// UrlEncode_test.cpp — coverage for the header-only RFC 3986 percent-encoder
// (src/Helpers/UrlEncode.hpp). The unreserved set passes through unchanged; every
// other byte becomes %XX with UPPERCASE hex, bytes treated as unsigned so high
// bytes (e.g. 0xFF -> "%FF") encode correctly. Pure/host-compilable, no deps.

#include <gtest/gtest.h>
#include <string>
#include "UrlEncode.hpp"

// ---- Unreserved characters (A-Z a-z 0-9 - _ . ~) pass through verbatim ----
TEST(UrlEncodeTest, UnreservedPassthrough)
{
    EXPECT_EQ(urlEncode("AZaz09-_.~"), "AZaz09-_.~");
    EXPECT_EQ(urlEncode(""), "");
    // Sanity over the full alphanumeric span plus each unreserved punctuation mark.
    EXPECT_EQ(urlEncode("ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    EXPECT_EQ(urlEncode("abcdefghijklmnopqrstuvwxyz"),
              "abcdefghijklmnopqrstuvwxyz");
    EXPECT_EQ(urlEncode("0123456789"), "0123456789");
}

// ---- Individual reserved/special characters encode to the correct %XX ----
TEST(UrlEncodeTest, ReservedCharsEncodeToUppercaseHex)
{
    EXPECT_EQ(urlEncode(" "), "%20");
    EXPECT_EQ(urlEncode("/"), "%2F");
    EXPECT_EQ(urlEncode(":"), "%3A");
    EXPECT_EQ(urlEncode("?"), "%3F");
    EXPECT_EQ(urlEncode("#"), "%23");
    EXPECT_EQ(urlEncode("&"), "%26");
    EXPECT_EQ(urlEncode("="), "%3D");
    EXPECT_EQ(urlEncode("@"), "%40");
    EXPECT_EQ(urlEncode("+"), "%2B");
    EXPECT_EQ(urlEncode("%"), "%25");

    // Hex digits A-F must be uppercase, never lowercase ("%2F" not "%2f").
    EXPECT_NE(urlEncode("/"), "%2f");
}

// ---- Realistic mixed string: unreserved kept, reserved encoded inline ----
TEST(UrlEncodeTest, RealisticMixedString)
{
    EXPECT_EQ(urlEncode("abc 123/x:y"), "abc%20123%2Fx%3Ay");
}

// ---- Bytes are unsigned: a high non-ASCII byte encodes as uppercase %XX ----
TEST(UrlEncodeTest, NonAsciiHighByteEncodesUnsigned)
{
    EXPECT_EQ(urlEncode(std::string("\xff")), "%FF");
    EXPECT_EQ(urlEncode(std::string("\x80")), "%80");
    // Embedded NUL byte must encode too (string carries its own length).
    EXPECT_EQ(urlEncode(std::string("\x00", 1)), "%00");
}
