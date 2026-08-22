// ReceiveBuffer keeps no state beyond two counters, and that is the design:
// deciding what a packet means belongs to Connection. So what is checked here
// is arithmetic - does the buffer ever offer more space than is actually left,
// and does it ask the pool for exactly the payload length and not a byte more.
//
// The split groups matter more than they look. A buffer that is only ever fed
// a whole header at once never exercises the offset in headerSpace(), and a
// stream does not promise whole headers.

#include <core/kiotty_block_pool.h>
#include <core/kiotty_connection_buffer.h>
#include <core/kiotty_packet_concept.h>

#include "support/kiotty_test_pools.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using kiotty::BlockPool;
using kiotty::ByteSpan;
using kiotty::Bytes;
using kiotty::defaultBlockClasses;
using kiotty::defaultBlockClassCount;
using kiotty::PacketHeader;
using kiotty::ReceiveBuffer;
using kiotty_test::SmallPool;

namespace
{
    // 0 means "as much as is on offer", which is the whole-at-once group.
    const size_t kWholeChunk = 0;

    struct SplitCase
    {
        size_t      chunk;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    // How the stream is allowed to break the packet up. One byte at a time is
    // the group that finds off-by-one arithmetic; 12 stops in the middle of the
    // header, which is where a buffer that assumes whole headers goes wrong.
    const SplitCase kSplitCases[] =
    {
        { kWholeChunk, "WholeAtOnce",   "everything the buffer offers, in one go" },
        {          12, "TwelveByTwelve", "half a header at a time" },
        {           1, "ByteByByte",    "one byte at a time" },
        {           7, "SevenBySeven",  "seven bytes at a time - never aligned to anything" },
    };

    struct PayloadCase
    {
        uint16_t    payload_length;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    // The payload length groups. 0 is the packet with no body at all, 24 is the
    // length that happens to equal the header so a confused implementation
    // could pass by accident, and 65535 is everything the field can say.
    const PayloadCase kPayloadCases[] =
    {
        {     0, "Payload0",     "no payload at all" },
        {     1, "Payload1",     "one byte" },
        {    24, "Payload24",    "as long as the header" },
        { 65535, "Payload65535", "the largest the length field can say" },
    };

    struct ReceiveCase
    {
        SplitCase   split;
        PayloadCase payload;
    };

    // The full product of the two axes: four splits by four lengths.
    std::vector<ReceiveCase> everySplitByEveryPayload()
    {
        std::vector<ReceiveCase> cases;

        for (size_t s = 0; s < 4; ++s)
        {
            for (size_t p = 0; p < 4; ++p)
            {
                ReceiveCase one;
                one.split   = kSplitCases[s];
                one.payload = kPayloadCases[p];
                cases.push_back(one);
            }
        }
        return cases;
    }

    PacketHeader makeHeader(uint16_t payload_length)
    {
        PacketHeader header;

        header.magic.set(kiotty::PACKET_MAGIC);
        header.correlation_id.set(0xABCDEF01u);
        header.timestamp.set(0x0102030405060708ull);
        header.command.set(7);
        header.flags.set(kiotty::PACKET_FLAG_EVENT);
        header.version.set(kiotty::PACKET_VERSION);
        header.payload_length.set(payload_length);

        return header;
    }

    // Feeds bytes through the span the buffer offers, in chunks, the way a
    // socket read would. Returns how many bytes went in.
    size_t feed(ReceiveBuffer& buffer, bool into_header,
                const uint8_t* source, size_t total, size_t chunk)
    {
        size_t written = 0;

        while (written < total)
        {
            const ByteSpan space = into_header ? buffer.headerSpace()
                                               : buffer.payloadSpace();

            if (space.size() == 0)
            {
                break;
            }

            size_t step = (chunk == kWholeChunk) ? space.size() : chunk;

            if (step > space.size())
            {
                step = space.size();
            }
            if (step > total - written)
            {
                step = total - written;
            }

            std::memcpy(space.data(), source + written, step);

            if (into_header)
            {
                buffer.addHeaderReceived(step);
            }
            else
            {
                buffer.addPayloadReceived(step);
            }

            written += step;
        }
        return written;
    }

    class Receive : public ::testing::TestWithParam<ReceiveCase>
    {
    };

    // Without this gtest names each row by dumping the struct as bytes, and
    // ctest -R becomes unusable on exactly the tests that need it most.
    std::string nameOf(const ::testing::TestParamInfo<ReceiveCase>& info)
    {
        return std::string(info.param.split.name) + "_" + info.param.payload.name;
    }
}

TEST(ReceiveBuffer, HeaderSpaceStartsAtTheWholeHeader)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    EXPECT_EQ(kiotty::PACKET_HEADER_SIZE, buffer.headerSpace().size());
    EXPECT_FALSE(buffer.isHeaderComplete());
}

TEST(ReceiveBuffer, HeaderSpaceOffersOnlyWhatIsStillMissing)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    buffer.addHeaderReceived(10);

    EXPECT_EQ(kiotty::PACKET_HEADER_SIZE - 10, buffer.headerSpace().size());
    EXPECT_FALSE(buffer.isHeaderComplete());
}

TEST(ReceiveBuffer, SingleByteFeedCompletesTheHeaderExactlyOnTheTwentyFourth)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    const PacketHeader   source = makeHeader(0);
    const uint8_t* const raw    = reinterpret_cast<const uint8_t*>(&source);

    for (size_t i = 0; i < kiotty::PACKET_HEADER_SIZE; ++i)
    {
        EXPECT_FALSE(buffer.isHeaderComplete()) << "already complete after " << i << " bytes";

        const ByteSpan space = buffer.headerSpace();

        ASSERT_EQ(kiotty::PACKET_HEADER_SIZE - i, space.size()) << "at byte " << i;

        space.data()[0] = raw[i];
        buffer.addHeaderReceived(1);
    }

    EXPECT_TRUE(buffer.isHeaderComplete());
    EXPECT_EQ(static_cast<size_t>(0), buffer.headerSpace().size());
}

TEST_P(Receive, RoundTripsTheHeaderAndPayloadWhateverTheSplit)
{
    const ReceiveCase& sample = GetParam();

    SCOPED_TRACE(::testing::Message()
                 << sample.split.label << " / " << sample.payload.label);

    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    const PacketHeader source = makeHeader(sample.payload.payload_length);

    ASSERT_EQ(kiotty::PACKET_HEADER_SIZE,
              feed(buffer, true, reinterpret_cast<const uint8_t*>(&source),
                   kiotty::PACKET_HEADER_SIZE, sample.split.chunk));

    ASSERT_TRUE(buffer.isHeaderComplete());

    EXPECT_EQ(kiotty::PACKET_MAGIC, buffer.header().magic.get());
    EXPECT_EQ(0xABCDEF01u, buffer.header().correlation_id.get());
    EXPECT_EQ(0x0102030405060708ull, buffer.header().timestamp.get());
    EXPECT_EQ(sample.payload.payload_length, buffer.header().payload_length.get());

    ASSERT_TRUE(buffer.openPayload());

    std::vector<uint8_t> body(sample.payload.payload_length);

    for (size_t i = 0; i < body.size(); ++i)
    {
        body[i] = static_cast<uint8_t>(i * 31u + 7u);
    }

    ASSERT_EQ(body.size(),
              feed(buffer, false, body.empty() ? nullptr : &body[0],
                   body.size(), sample.split.chunk));

    ASSERT_TRUE(buffer.isPayloadComplete());

    const Bytes taken = buffer.takePayload();

    ASSERT_EQ(body.size(), taken.size());

    if (!body.empty())
    {
        EXPECT_EQ(0, std::memcmp(taken.data(), &body[0], body.size()));
    }

    // takePayload rewinds both counters, so the buffer is ready for the next
    // packet on the same connection without anything else being called.
    EXPECT_EQ(kiotty::PACKET_HEADER_SIZE, buffer.headerSpace().size());
    EXPECT_FALSE(buffer.isHeaderComplete());
}

INSTANTIATE_TEST_SUITE_P(EverySplitByEveryPayloadLength, Receive,
                         ::testing::ValuesIn(everySplitByEveryPayload()), nameOf);

TEST(ReceiveBuffer, OpenPayloadAsksForExactlyThePayloadLength)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    const PacketHeader source = makeHeader(100);

    std::memcpy(buffer.headerSpace().data(), &source, kiotty::PACKET_HEADER_SIZE);
    buffer.addHeaderReceived(kiotty::PACKET_HEADER_SIZE);

    ASSERT_TRUE(buffer.openPayload());

    // The block behind this is 128 bytes wide, but the space on offer is 100.
    // Reading into the whole block would swallow the next packet's first bytes,
    // and the length-exact request is the reason that cannot happen.
    EXPECT_EQ(static_cast<size_t>(100), buffer.payloadSpace().size());
}

TEST(ReceiveBuffer, PayloadSpaceOffersOnlyWhatIsStillMissing)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    const PacketHeader source = makeHeader(100);

    std::memcpy(buffer.headerSpace().data(), &source, kiotty::PACKET_HEADER_SIZE);
    buffer.addHeaderReceived(kiotty::PACKET_HEADER_SIZE);

    ASSERT_TRUE(buffer.openPayload());

    buffer.addPayloadReceived(40);

    EXPECT_EQ(static_cast<size_t>(60), buffer.payloadSpace().size());
    EXPECT_FALSE(buffer.isPayloadComplete());

    buffer.addPayloadReceived(60);

    EXPECT_EQ(static_cast<size_t>(0), buffer.payloadSpace().size());
    EXPECT_TRUE(buffer.isPayloadComplete());
}

TEST(ReceiveBuffer, ZeroLengthPayloadCompletesWithoutTakingABlock)
{
    SmallPool     small(64, 1);
    ReceiveBuffer buffer(small.pool());

    // The pool has one block and it is already gone, so any request at all
    // would show up as a heap fallback.
    void* const only_block = small.pool().acquire(64);
    ASSERT_NE(nullptr, only_block);

    const PacketHeader source = makeHeader(0);

    std::memcpy(buffer.headerSpace().data(), &source, kiotty::PACKET_HEADER_SIZE);
    buffer.addHeaderReceived(kiotty::PACKET_HEADER_SIZE);

    EXPECT_TRUE(buffer.openPayload());
    EXPECT_TRUE(buffer.isPayloadComplete());
    EXPECT_EQ(static_cast<size_t>(0), buffer.payloadSpace().size());
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount())
        << "a packet with no body must not ask the pool for anything";

    small.pool().release(only_block);
}

TEST(ReceiveBuffer, TakePayloadHandsTheBlockToTheCallerAndRewindsTheBuffer)
{
    SmallPool     small(64, 1);
    ReceiveBuffer buffer(small.pool());

    const PacketHeader source = makeHeader(64);

    std::memcpy(buffer.headerSpace().data(), &source, kiotty::PACKET_HEADER_SIZE);
    buffer.addHeaderReceived(kiotty::PACKET_HEADER_SIZE);

    ASSERT_TRUE(buffer.openPayload());
    buffer.addPayloadReceived(64);

    {
        const Bytes taken = buffer.takePayload();

        EXPECT_TRUE(static_cast<bool>(taken));
        EXPECT_EQ(static_cast<size_t>(64), taken.size());

        // The buffer let go of it rather than keeping a second reference: the
        // pool still has nothing spare while the caller holds the block.
        EXPECT_EQ(kiotty::PACKET_HEADER_SIZE, buffer.headerSpace().size());
        EXPECT_FALSE(buffer.isHeaderComplete());
        EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());
    }

    // And when the caller drops it, the block is back.
    void* const reused = small.pool().acquire(64);

    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());
    small.pool().release(reused);
}

TEST(ReceiveBuffer, ResetOnAHalfReceivedHeaderRestoresTheWholeHeaderSpace)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    buffer.addHeaderReceived(12);
    ASSERT_EQ(kiotty::PACKET_HEADER_SIZE - 12, buffer.headerSpace().size());

    buffer.reset();

    EXPECT_EQ(kiotty::PACKET_HEADER_SIZE, buffer.headerSpace().size());
    EXPECT_FALSE(buffer.isHeaderComplete());
}

TEST(ReceiveBuffer, ResetReturnsAnOpenPayloadBlockToThePool)
{
    SmallPool     small(64, 1);
    ReceiveBuffer buffer(small.pool());

    const PacketHeader source = makeHeader(64);

    std::memcpy(buffer.headerSpace().data(), &source, kiotty::PACKET_HEADER_SIZE);
    buffer.addHeaderReceived(kiotty::PACKET_HEADER_SIZE);

    ASSERT_TRUE(buffer.openPayload());
    buffer.addPayloadReceived(30);

    buffer.reset();

    EXPECT_EQ(kiotty::PACKET_HEADER_SIZE, buffer.headerSpace().size());

    // A reset that dropped the counters but kept the block would leak it for
    // as long as the connection lived.
    void* const reused = small.pool().acquire(64);

    ASSERT_NE(nullptr, reused);
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    small.pool().release(reused);
}

TEST(ReceiveBuffer, ResetIsIdempotent)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    buffer.addHeaderReceived(12);

    buffer.reset();
    buffer.reset();

    EXPECT_EQ(kiotty::PACKET_HEADER_SIZE, buffer.headerSpace().size());
    EXPECT_TRUE(buffer.isPayloadComplete()) << "no payload is a complete payload";
}

TEST(ReceiveBuffer, ReopeningThePayloadRewindsWhatWasAlreadyRead)
{
    BlockPool     pool(defaultBlockClasses(), defaultBlockClassCount());
    ReceiveBuffer buffer(pool);

    const PacketHeader source = makeHeader(100);

    std::memcpy(buffer.headerSpace().data(), &source, kiotty::PACKET_HEADER_SIZE);
    buffer.addHeaderReceived(kiotty::PACKET_HEADER_SIZE);

    ASSERT_TRUE(buffer.openPayload());
    buffer.addPayloadReceived(40);

    ASSERT_TRUE(buffer.openPayload());

    EXPECT_EQ(static_cast<size_t>(100), buffer.payloadSpace().size());
    EXPECT_FALSE(buffer.isPayloadComplete());
}
