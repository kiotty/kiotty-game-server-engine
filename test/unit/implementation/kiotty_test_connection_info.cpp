// ConnectionInfo is the one piece of presentation data that crosses into the
// domain, and it crosses by value so the domain never holds presentation
// memory. makeConnectionInfo is a bounded strcpy, and bounded copies fail in
// exactly three ways: the bound is off by one, the terminator is lost, or the
// null input is dereferenced. The ip table covers each.

#include <domain/entity/kiotty_connection_info.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>

using kiotty::CONNECTION_IPV4_SIZE;
using kiotty::ConnectionInfo;
using kiotty::makeConnectionInfo;

// The buffer must hold the longest dotted-decimal IPv4 plus its terminator
// and nothing more - the truncation cases below assume exactly this.
static_assert(CONNECTION_IPV4_SIZE == 16, "255.255.255.255 is 15 chars plus NUL");
static_assert(sizeof(ConnectionInfo::ip) == CONNECTION_IPV4_SIZE,
              "ip must be exactly CONNECTION_IPV4_SIZE bytes");

namespace
{
    struct IpCase
    {
        const char* input;
        const char* expected;
        const char* name;
    };

    // null / empty / short / exactly at the bound / one past / far past.
    // The two overlong rows differ in what the 16th character is: a digit
    // looks like a plausible address and a letter does not, and a copy that
    // stopped at a non-digit would pass one row and fail the other.
    const IpCase kIpCases[] =
    {
        { nullptr,                       "",                "Null" },
        { "",                            "",                "Empty" },
        { "1.2.3.4",                     "1.2.3.4",         "Short" },
        { "255.255.255.255",             "255.255.255.255", "ExactlyFifteen" },
        { "255.255.255.2559",            "255.255.255.255", "SixteenTruncatedToFifteen" },
        { "255.255.255.255abcdefghijkl", "255.255.255.255", "FarOverlongTruncatedToFifteen" },
    };

    // port is copied, not parsed - still, 0 and 65535 are the two values a
    // narrowing or sign mistake would alter.
    const uint16_t kPorts[] = { 0, 8080, 65535 };

    class Make : public ::testing::TestWithParam<std::tuple<IpCase, uint16_t> >
    {
    };

    std::string nameOf(const ::testing::TestParamInfo<std::tuple<IpCase, uint16_t> >& info)
    {
        return std::string(std::get<0>(info.param).name) + "Port" +
               std::to_string(std::get<1>(info.param));
    }
}

TEST(ConnectionInfo, DefaultConstructedHasEmptyIpAndPortZero)
{
    ConnectionInfo info;

    EXPECT_STREQ("", info.ip);
    EXPECT_EQ(0u, info.port);

    // Every byte, not just the first, must be zero: the struct is compared
    // and copied as a whole.
    for (size_t i = 0; i < CONNECTION_IPV4_SIZE; ++i)
    {
        EXPECT_EQ(0, info.ip[i]) << "at index " << i;
    }
}

TEST_P(Make, CopiesTheIpUpToFifteenCharsAndTheStringStaysTerminated)
{
    const IpCase&  ip   = std::get<0>(GetParam());
    const uint16_t port = std::get<1>(GetParam());

    const ConnectionInfo info = makeConnectionInfo(ip.input, port);

    EXPECT_STREQ(ip.expected, info.ip);
    EXPECT_EQ(port, info.port);

    // The terminator is the contract, and the last byte is the one that a
    // copy of exactly CONNECTION_IPV4_SIZE characters would overwrite.
    EXPECT_EQ(0, info.ip[CONNECTION_IPV4_SIZE - 1]);
    EXPECT_LE(std::strlen(info.ip), CONNECTION_IPV4_SIZE - 1);
}

INSTANTIATE_TEST_SUITE_P(AllIpsAllPorts, Make,
                         ::testing::Combine(::testing::ValuesIn(kIpCases),
                                            ::testing::ValuesIn(kPorts)),
                         nameOf);

TEST(ConnectionInfo, MakeConnectionInfoDoesNotKeepThePointerItWasGiven)
{
    char source[] = "10.0.0.1";

    const ConnectionInfo info = makeConnectionInfo(source, 1);

    // Mutating the source after the call must not change the copy - the whole
    // point of the value type is that presentation memory can go away.
    source[0] = 'X';
    EXPECT_STREQ("10.0.0.1", info.ip);
}

TEST(ConnectionInfo, BytesAfterAShortIpStayZero)
{
    const ConnectionInfo info = makeConnectionInfo("1.2.3.4", 1);

    for (size_t i = std::strlen("1.2.3.4"); i < CONNECTION_IPV4_SIZE; ++i)
    {
        EXPECT_EQ(0, info.ip[i]) << "at index " << i;
    }
}
