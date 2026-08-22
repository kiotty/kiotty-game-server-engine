// The reason LittleEndian and PacketHeader exist at all is that a header is
// read where it lands in the receive buffer, which is any offset at all. So the
// tests put headers at odd offsets on purpose: at offset 0 a broken
// implementation that casts instead of copying still passes.
//
// The layout claims themselves are static_asserts rather than tests. A wrong
// size should stop the compile on the machine that has the wrong size, not
// produce a red test somewhere else.

#include <core/kiotty_packet_concept.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>

using kiotty::hasPacketMagic;
using kiotty::hasSupportedVersion;
using kiotty::LittleEndian;
using kiotty::PacketHeader;

// A wire field must add nothing of its own, or a header could not be read where
// it lands. These are the assumptions the receive path is built on.
static_assert(sizeof(LittleEndian<uint16_t>) == 2, "a wire field is its bytes and nothing else");
static_assert(sizeof(LittleEndian<uint32_t>) == 4, "a wire field is its bytes and nothing else");
static_assert(sizeof(LittleEndian<uint64_t>) == 8, "a wire field is its bytes and nothing else");
static_assert(alignof(LittleEndian<uint16_t>) == 1, "a wire field must need no alignment");
static_assert(alignof(LittleEndian<uint32_t>) == 1, "a wire field must need no alignment");
static_assert(alignof(LittleEndian<uint64_t>) == 1, "a wire field must need no alignment");

static_assert(sizeof(PacketHeader) == kiotty::PACKET_HEADER_SIZE,
              "PACKET_HEADER_SIZE and the struct must not drift apart");

// payload_length is a uint16_t, so a value above PACKET_MAX_PAYLOAD cannot be
// spelled. That is a layout guarantee rather than a runtime one, and saying it
// here is worth more than a test that could only assert what the type already
// forbids.
static_assert(kiotty::PACKET_MAX_PAYLOAD == std::numeric_limits<uint16_t>::max(),
              "PACKET_MAX_PAYLOAD must be exactly what payload_length can hold");
static_assert(sizeof(decltype(PacketHeader::payload_length)) == sizeof(uint16_t),
              "payload_length must stay 16 bits or the bound above is a lie");

namespace
{
    // The four alignment offsets. 0 is the case that passes even when the
    // implementation is wrong, so the other three are the ones doing the work;
    // 7 is the worst case for an 8 byte field.
    const size_t kOffsets[] = { 0, 1, 3, 7 };

    // A value whose every byte differs, so a swapped or dropped byte cannot
    // hide behind a symmetric pattern: 0x02'01 for 16 bits, 0x08..01 for 64.
    template <typename T>
    T distinctBytePattern()
    {
        T value = 0;

        for (size_t i = 0; i < sizeof(T); ++i)
        {
            value = static_cast<T>(value |
                    (static_cast<T>(static_cast<uint8_t>(i + 1)) << (8u * i)));
        }
        return value;
    }

    struct MagicCase
    {
        uint32_t    magic;
        bool        expect_accepted;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    // Every single byte of the magic changed on its own. A comparison that only
    // looked at part of the field would let one of these through.
    const MagicCase kMagicCases[] =
    {
        { kiotty::PACKET_MAGIC,               true,  "Exact",       "exactly the magic" },
        { kiotty::PACKET_MAGIC ^ 0x000000FFu, false, "Byte0Wrong",  "low byte differs" },
        { kiotty::PACKET_MAGIC ^ 0x0000FF00u, false, "Byte1Wrong",  "second byte differs" },
        { kiotty::PACKET_MAGIC ^ 0x00FF0000u, false, "Byte2Wrong",  "third byte differs" },
        { kiotty::PACKET_MAGIC ^ 0xFF000000u, false, "Byte3Wrong",  "high byte differs" },
        { 0u,                                 false, "AllZero",     "nothing at all" },
        { 0xFFFFFFFFu,                        false, "AllOnes",     "every bit set" },
    };

    class Magic : public ::testing::TestWithParam<MagicCase>
    {
    };

    struct VersionCase
    {
        uint8_t     major;
        uint8_t     minor;
        bool        expect_supported;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    // Only the major byte decides. The minor groups are there to prove the
    // check does not quietly look at the whole field.
    const VersionCase kVersionCases[] =
    {
        { kiotty::PACKET_VERSION_MAJOR, 0,   true,  "OurMajorMinor0",   "our major, minor 0" },
        { kiotty::PACKET_VERSION_MAJOR, 1,   true,  "OurMajorMinor1",   "our major, minor 1" },
        { kiotty::PACKET_VERSION_MAJOR, 255, true,  "OurMajorMinor255", "our major, minor at its top" },
        { 0,                            0,   false, "Major0",           "major 0" },
        { 2,                            0,   false, "Major2",           "major one above ours" },
        { 255,                          255, false, "Major255Minor255", "both bytes at their top" },
    };

    class Version : public ::testing::TestWithParam<VersionCase>
    {
    };

    // Without these gtest names each row by dumping the struct as bytes, and
    // ctest -R becomes unusable on exactly the tests that need it most.
    std::string nameOfMagic(const ::testing::TestParamInfo<MagicCase>& info)
    {
        return info.param.name;
    }

    std::string nameOfVersion(const ::testing::TestParamInfo<VersionCase>& info)
    {
        return info.param.name;
    }

    std::string nameOfOffset(const ::testing::TestParamInfo<size_t>& info)
    {
        return "Offset" + std::to_string(info.param);
    }

    // A header placed at a chosen offset inside an over-aligned buffer. Nothing
    // here casts a misaligned pointer: the header is constructed where it is
    // meant to live, which is what the receive path effectively does.
    class HeaderAt
    {
    public:
        explicit HeaderAt(size_t offset) :
            _header(::new (_storage + offset) PacketHeader())
        {
        }

        PacketHeader& header() { return *_header; }
        const uint8_t* bytes() const { return reinterpret_cast<const uint8_t*>(_header); }

    private:
        alignas(8) uint8_t _storage[kiotty::PACKET_HEADER_SIZE + 8];
        PacketHeader*      _header;
    };
}

TEST(LittleEndianField, StoresTheLeastSignificantByteFirst)
{
    LittleEndian<uint32_t> field;

    field.set(0x01020304u);

    const uint8_t* const raw = reinterpret_cast<const uint8_t*>(&field);

    EXPECT_EQ(0x04, raw[0]);
    EXPECT_EQ(0x03, raw[1]);
    EXPECT_EQ(0x02, raw[2]);
    EXPECT_EQ(0x01, raw[3]);
}

TEST(LittleEndianField, StoresSixtyFourBitValuesLeastSignificantByteFirst)
{
    LittleEndian<uint64_t> field;

    field.set(0x0102030405060708ull);

    const uint8_t* const raw = reinterpret_cast<const uint8_t*>(&field);

    for (size_t i = 0; i < sizeof(uint64_t); ++i)
    {
        EXPECT_EQ(static_cast<uint8_t>(8 - i), raw[i]) << "at byte " << i;
    }
}

// The value groups are 0, 1, the type maximum and a byte-distinct pattern, and
// every one of them is written at every alignment offset. A typed test is what
// carries the width across; the two inner loops are the value and offset
// groups, labelled so a failing combination names itself.
template <typename T>
class LittleEndianWidth : public ::testing::Test
{
};

typedef ::testing::Types<uint16_t, uint32_t, uint64_t> WireWidths;
TYPED_TEST_SUITE(LittleEndianWidth, WireWidths);

TYPED_TEST(LittleEndianWidth, RoundTripsEveryValueGroupAtEveryAlignmentOffset)
{
    typedef TypeParam Value;

    const Value values[] =
    {
        static_cast<Value>(0),
        static_cast<Value>(1),
        std::numeric_limits<Value>::max(),
        distinctBytePattern<Value>(),
    };

    for (size_t offset_index = 0; offset_index < 4; ++offset_index)
    {
        const size_t offset = kOffsets[offset_index];

        alignas(8) uint8_t storage[sizeof(Value) + 8];

        LittleEndian<Value>* const field =
            ::new (storage + offset) LittleEndian<Value>();

        for (size_t value_index = 0; value_index < 4; ++value_index)
        {
            SCOPED_TRACE(::testing::Message()
                         << "width " << sizeof(Value) << ", offset " << offset
                         << ", value index " << value_index);

            field->set(values[value_index]);

            EXPECT_EQ(values[value_index], field->get());
        }
    }
}

TYPED_TEST(LittleEndianWidth, PatternLandsByteByByteInIncreasingOrder)
{
    typedef TypeParam Value;

    LittleEndian<Value> field;

    field.set(distinctBytePattern<Value>());

    const uint8_t* const raw = reinterpret_cast<const uint8_t*>(&field);

    for (size_t i = 0; i < sizeof(Value); ++i)
    {
        EXPECT_EQ(static_cast<uint8_t>(i + 1), raw[i]) << "at byte " << i;
    }
}

TEST(PacketHeaderLayout, EveryFieldSitsWhereTheWireExpectsIt)
{
    // Offsets are part of the protocol, so they are asserted rather than
    // trusted to declaration order surviving an edit.
    EXPECT_EQ(static_cast<size_t>(0),  offsetof(PacketHeader, magic));
    EXPECT_EQ(static_cast<size_t>(4),  offsetof(PacketHeader, correlation_id));
    EXPECT_EQ(static_cast<size_t>(8),  offsetof(PacketHeader, timestamp));
    EXPECT_EQ(static_cast<size_t>(16), offsetof(PacketHeader, command));
    EXPECT_EQ(static_cast<size_t>(18), offsetof(PacketHeader, flags));
    EXPECT_EQ(static_cast<size_t>(20), offsetof(PacketHeader, version));
    EXPECT_EQ(static_cast<size_t>(22), offsetof(PacketHeader, payload_length));
}

class HeaderOffset : public ::testing::TestWithParam<size_t>
{
};

TEST_P(HeaderOffset, RoundTripsEveryFieldWhereverTheHeaderLands)
{
    const size_t offset = GetParam();

    SCOPED_TRACE(::testing::Message() << "header at offset " << offset);

    HeaderAt      placed(offset);
    PacketHeader& header = placed.header();

    header.magic.set(kiotty::PACKET_MAGIC);
    header.correlation_id.set(0xFFFFFFFFu);
    header.timestamp.set(distinctBytePattern<uint64_t>());
    header.command.set(0);
    header.flags.set(kiotty::PACKET_FLAG_EVENT | kiotty::PACKET_FLAG_LAST_FRAGMENT);
    header.version.set(kiotty::PACKET_VERSION);
    header.payload_length.set(std::numeric_limits<uint16_t>::max());

    EXPECT_EQ(kiotty::PACKET_MAGIC, header.magic.get());
    EXPECT_EQ(0xFFFFFFFFu, header.correlation_id.get());
    EXPECT_EQ(distinctBytePattern<uint64_t>(), header.timestamp.get());
    EXPECT_EQ(0u, header.command.get());
    EXPECT_EQ(static_cast<uint16_t>(kiotty::PACKET_FLAG_EVENT |
                                    kiotty::PACKET_FLAG_LAST_FRAGMENT),
              header.flags.get());
    EXPECT_EQ(kiotty::PACKET_VERSION, header.version.get());
    EXPECT_EQ(std::numeric_limits<uint16_t>::max(), header.payload_length.get());

    // The magic is the first four bytes of the header wherever the header is,
    // which is what lets a peer recognise the stream without knowing offsets.
    EXPECT_EQ(0x4B, placed.bytes()[0]);
    EXPECT_EQ(0x49, placed.bytes()[1]);
    EXPECT_EQ(0x4F, placed.bytes()[2]);
    EXPECT_EQ(0x54, placed.bytes()[3]);
}

INSTANTIATE_TEST_SUITE_P(EveryAlignmentOffset, HeaderOffset,
                         ::testing::ValuesIn(kOffsets), nameOfOffset);

TEST_P(Magic, IsAcceptedOnlyWhenEveryByteMatches)
{
    const MagicCase& sample = GetParam();

    SCOPED_TRACE(sample.label);

    PacketHeader header;
    header.magic.set(sample.magic);

    EXPECT_EQ(sample.expect_accepted, hasPacketMagic(header));
}

INSTANTIATE_TEST_SUITE_P(EveryByteOfTheMagic, Magic,
                         ::testing::ValuesIn(kMagicCases), nameOfMagic);

TEST_P(Version, IsDecidedByTheMajorByteAlone)
{
    const VersionCase& sample = GetParam();

    SCOPED_TRACE(sample.label);

    PacketHeader header;
    header.version.set(static_cast<uint16_t>(
        static_cast<uint16_t>(sample.major) |
        static_cast<uint16_t>(static_cast<uint16_t>(sample.minor) << 8)));

    EXPECT_EQ(sample.expect_supported, hasSupportedVersion(header));
}

INSTANTIATE_TEST_SUITE_P(EveryVersionGroup, Version,
                         ::testing::ValuesIn(kVersionCases), nameOfVersion);

TEST(PacketVersion, PacksTheMajorInTheLowByteAndTheMinorInTheHigh)
{
    EXPECT_EQ(kiotty::PACKET_VERSION_MAJOR,
              static_cast<uint8_t>(kiotty::PACKET_VERSION & 0xFFu));
    EXPECT_EQ(kiotty::PACKET_VERSION_MINOR,
              static_cast<uint8_t>((kiotty::PACKET_VERSION >> 8) & 0xFFu));
}

TEST(PacketFlags, EachFlagOwnsItsOwnBit)
{
    const uint16_t all = kiotty::PACKET_FLAG_EVENT |
                         kiotty::PACKET_FLAG_FRAGMENTED |
                         kiotty::PACKET_FLAG_LAST_FRAGMENT |
                         kiotty::PACKET_FLAG_COMPRESSED;

    // Four distinct bits sum to their own or; a shared bit would show up as a
    // smaller sum, and two flags sharing a bit is a protocol level defect.
    EXPECT_EQ(static_cast<uint16_t>(kiotty::PACKET_FLAG_EVENT +
                                    kiotty::PACKET_FLAG_FRAGMENTED +
                                    kiotty::PACKET_FLAG_LAST_FRAGMENT +
                                    kiotty::PACKET_FLAG_COMPRESSED),
              all);
}
