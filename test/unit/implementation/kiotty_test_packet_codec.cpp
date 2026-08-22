// DefaultPacketCodec is the wire <-> entity boundary. decode is a field copy
// plus one move, encode is writePacket with the flags decided by the entity
// type. What is worth pinning is which header field lands where (timestamp
// becomes state_sequence, sequence becomes correlation_id on events) and that
// the payload is moved rather than copied - a copy would double the pool
// traffic per request without any test noticing.
//
// encode is compared with writePacket byte for byte, except the timestamp:
// both read the clock, so those 8 bytes are the one thing that can differ
// between two correct calls.

#include <core/kiotty_connection_buffer.h>
#include <core/kiotty_packet_concept.h>
#include <core/kiotty_packet_writer.h>
#include <domain/codec/kiotty_packet_codec.h>

#include "support/kiotty_test_pools.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using kiotty::BlockPool;
using kiotty::Bytes;
using kiotty::ChannelId;
using kiotty::DefaultPacketCodec;
using kiotty::GameEvent;
using kiotty::GameRequest;
using kiotty::GameResponse;
using kiotty::makeChannelId;
using kiotty::PACKET_FLAG_EVENT;
using kiotty::PACKET_HEADER_SIZE;
using kiotty::PACKET_MAGIC;
using kiotty::PACKET_MAX_PAYLOAD;
using kiotty::PACKET_VERSION;
using kiotty::PacketHeader;
using kiotty::ReceivedPacket;
using kiotty_test::oneBlockPerClass;
using kiotty_test::oneBlockPerClassCount;

namespace
{
    // A payload whose bytes are all distinct modulo 251 (a prime), so a
    // truncated or shifted copy cannot match by coincidence.
    Bytes patternBytes(BlockPool& pool, size_t length)
    {
        Bytes bytes(pool, length);

        if (bytes)
        {
            uint8_t* const out = bytes.writableSpan().data();

            for (size_t i = 0; i < length; ++i)
            {
                out[i] = static_cast<uint8_t>(i % 251);
            }
        }
        return bytes;
    }

    bool hasPattern(const uint8_t* data, size_t length)
    {
        for (size_t i = 0; i < length; ++i)
        {
            if (data[i] != static_cast<uint8_t>(i % 251))
            {
                return false;
            }
        }
        return true;
    }

    PacketHeader headerOf(const Bytes& packet)
    {
        PacketHeader header;

        std::memcpy(&header, packet.data(), PACKET_HEADER_SIZE);
        return header;
    }

    // Byte offsets of the timestamp within the header - the field the
    // byte-for-byte comparison has to skip.
    const size_t kTimestampOffset = 8;
    const size_t kTimestampSize   = 8;

    bool equalExceptTimestamp(const Bytes& lhs, const Bytes& rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            if (i >= kTimestampOffset && i < kTimestampOffset + kTimestampSize)
            {
                continue;
            }
            if (lhs.data()[i] != rhs.data()[i])
            {
                return false;
            }
        }
        return true;
    }

    struct PayloadCase
    {
        size_t      length;
        bool        expect_encoded;
        const char* name;
    };

    // 0 and 1 are the two ends of "nothing" and "something"; the last two sit
    // on either side of the wire bound. PACKET_MAX_PAYLOAD + 1 needs a 65536
    // byte block, which is exactly the largest class in oneBlockPerClass.
    const PayloadCase kPayloadCases[] =
    {
        { 0,                      true,  "Empty" },
        { 1,                      true,  "OneByte" },
        { 100,                    true,  "Typical" },
        { PACKET_MAX_PAYLOAD,     true,  "ExactlyMax" },
        { PACKET_MAX_PAYLOAD + 1, false, "OnePastMax" },
    };

    class Payload : public ::testing::TestWithParam<PayloadCase>
    {
    protected:
        Payload() :
            _pool(oneBlockPerClass(), oneBlockPerClassCount())
        {
        }

        BlockPool _pool;
    };

    std::string nameOf(const ::testing::TestParamInfo<PayloadCase>& info)
    {
        return info.param.name;
    }

    class Codec : public ::testing::Test
    {
    protected:
        Codec() :
            _pool(oneBlockPerClass(), oneBlockPerClassCount())
        {
        }

        BlockPool _pool;
    };
}

// -----------------------------------------------------------------------------
// decode
// -----------------------------------------------------------------------------

TEST_F(Codec, DecodeCopiesEveryHeaderFieldToItsEntityField)
{
    DefaultPacketCodec codec;
    ReceivedPacket     packet;

    packet.header.magic.set(PACKET_MAGIC);
    packet.header.correlation_id.set(0x11223344u);
    packet.header.timestamp.set(0x0102030405060708ull);
    packet.header.command.set(0xABCD);
    packet.header.flags.set(0);
    packet.header.version.set(PACKET_VERSION);
    packet.header.payload_length.set(4);
    packet.payload = patternBytes(_pool, 4);
    ASSERT_TRUE(static_cast<bool>(packet.payload));

    const ChannelId channel_id = makeChannelId(2, 9);
    GameRequest     out;

    EXPECT_TRUE(codec.decode(packet, channel_id, out));

    EXPECT_EQ(0xABCDu, out.command);
    EXPECT_EQ(0x11223344u, out.correlation_id);
    EXPECT_EQ(0x0102030405060708ull, out.state_sequence);
    EXPECT_EQ(channel_id, out.channel_id);
    ASSERT_EQ(4u, out.payload.size());
    EXPECT_TRUE(hasPattern(out.payload.data(), 4));
}

TEST_F(Codec, DecodeMovesThePayloadInsteadOfCopyingIt)
{
    DefaultPacketCodec codec;
    ReceivedPacket     packet;

    packet.payload = patternBytes(_pool, 16);
    ASSERT_TRUE(static_cast<bool>(packet.payload));

    const uint8_t* const original = packet.payload.data();
    GameRequest          out;

    ASSERT_TRUE(codec.decode(packet, ChannelId(), out));

    EXPECT_EQ(original, out.payload.data());
    EXPECT_FALSE(static_cast<bool>(packet.payload));
    EXPECT_EQ(0u, packet.payload.size());
}

TEST_F(Codec, DecodeWithEmptyPayloadLeavesAnEmptyPayload)
{
    DefaultPacketCodec codec;
    ReceivedPacket     packet;
    GameRequest        out;

    packet.header.command.set(1);

    ASSERT_TRUE(codec.decode(packet, ChannelId(), out));
    EXPECT_EQ(1u, out.command);
    EXPECT_FALSE(static_cast<bool>(out.payload));
    EXPECT_EQ(0u, out.payload.size());
}

TEST_F(Codec, DecodeIgnoresFlagsVersionAndMagic)
{
    DefaultPacketCodec codec;
    ReceivedPacket     packet;
    GameRequest        out;

    // Connection has already validated the envelope by the time the codec
    // runs; the codec must not second-guess it and must not copy those
    // fields anywhere - there is nowhere in GameRequest for them to go.
    packet.header.magic.set(0);
    packet.header.version.set(0xFFFF);
    packet.header.flags.set(0xFFFF);
    packet.header.command.set(7);

    EXPECT_TRUE(codec.decode(packet, ChannelId(), out));
    EXPECT_EQ(7u, out.command);
}

// -----------------------------------------------------------------------------
// encode(response)
// -----------------------------------------------------------------------------

TEST_F(Codec, EncodeResponseWritesHeaderFieldsWithFlagsZero)
{
    DefaultPacketCodec codec;
    GameResponse       response;

    response.command        = 0x1234;
    response.correlation_id = 0xDEADBEEFu;
    response.payload        = patternBytes(_pool, 10);
    ASSERT_TRUE(static_cast<bool>(response.payload));

    Bytes packet = codec.encode(_pool, response);

    ASSERT_TRUE(static_cast<bool>(packet));
    ASSERT_EQ(PACKET_HEADER_SIZE + 10, packet.size());

    const PacketHeader header = headerOf(packet);

    EXPECT_EQ(PACKET_MAGIC, header.magic.get());
    EXPECT_EQ(0xDEADBEEFu, header.correlation_id.get());
    EXPECT_EQ(0x1234u, header.command.get());
    EXPECT_EQ(0u, header.flags.get());
    EXPECT_EQ(PACKET_VERSION, header.version.get());
    EXPECT_EQ(10u, header.payload_length.get());
    EXPECT_TRUE(hasPattern(packet.data() + PACKET_HEADER_SIZE, 10));
}

TEST_F(Codec, EncodeResponseMatchesWritePacketByteForByte)
{
    DefaultPacketCodec codec;
    GameResponse       response;

    response.command        = 42;
    response.correlation_id = 7;
    response.payload        = patternBytes(_pool, 33);
    ASSERT_TRUE(static_cast<bool>(response.payload));

    Bytes from_codec  = codec.encode(_pool, response);
    Bytes from_writer = kiotty::writePacket(_pool, 42, 0, 7, response.payload.view());

    ASSERT_TRUE(static_cast<bool>(from_codec));
    ASSERT_TRUE(static_cast<bool>(from_writer));
    EXPECT_TRUE(equalExceptTimestamp(from_codec, from_writer));
}

TEST_F(Codec, EncodeResponseLeavesTheEntityPayloadIntact)
{
    DefaultPacketCodec codec;
    GameResponse       response;

    response.payload = patternBytes(_pool, 8);
    ASSERT_TRUE(static_cast<bool>(response.payload));

    const uint8_t* const original = response.payload.data();
    Bytes                packet   = codec.encode(_pool, response);

    // The response is const on the way in and may be delivered to several
    // listeners; encode must read it, not take it.
    ASSERT_TRUE(static_cast<bool>(packet));
    EXPECT_EQ(original, response.payload.data());
    EXPECT_EQ(8u, response.payload.size());
    EXPECT_NE(original, packet.data());
}

// -----------------------------------------------------------------------------
// encode(event, sequence)
// -----------------------------------------------------------------------------

TEST_F(Codec, EncodeEventSetsTheEventFlagAndUsesSequenceAsCorrelationId)
{
    DefaultPacketCodec codec;
    GameEvent          event;

    event.command = 0x5678;
    event.payload = patternBytes(_pool, 5);
    ASSERT_TRUE(static_cast<bool>(event.payload));

    Bytes packet = codec.encode(_pool, event, 0xCAFEBABEu);

    ASSERT_TRUE(static_cast<bool>(packet));
    ASSERT_EQ(PACKET_HEADER_SIZE + 5, packet.size());

    const PacketHeader header = headerOf(packet);

    EXPECT_EQ(PACKET_MAGIC, header.magic.get());
    EXPECT_EQ(0xCAFEBABEu, header.correlation_id.get());
    EXPECT_EQ(0x5678u, header.command.get());
    EXPECT_EQ(PACKET_FLAG_EVENT, header.flags.get());
    EXPECT_EQ(PACKET_VERSION, header.version.get());
    EXPECT_EQ(5u, header.payload_length.get());
    EXPECT_TRUE(hasPattern(packet.data() + PACKET_HEADER_SIZE, 5));
}

TEST_F(Codec, EncodeEventMatchesWritePacketByteForByte)
{
    DefaultPacketCodec codec;
    GameEvent          event;

    event.command = 3;
    event.payload = patternBytes(_pool, 20);
    ASSERT_TRUE(static_cast<bool>(event.payload));

    Bytes from_codec  = codec.encode(_pool, event, 99);
    Bytes from_writer = kiotty::writePacket(_pool, 3, PACKET_FLAG_EVENT, 99, event.payload.view());

    ASSERT_TRUE(static_cast<bool>(from_codec));
    ASSERT_TRUE(static_cast<bool>(from_writer));
    EXPECT_TRUE(equalExceptTimestamp(from_codec, from_writer));
}

TEST_F(Codec, EncodeEventSequenceZeroIsWrittenAsZero)
{
    DefaultPacketCodec codec;
    GameEvent          event;

    Bytes packet = codec.encode(_pool, event, 0);

    ASSERT_TRUE(static_cast<bool>(packet));
    EXPECT_EQ(0u, headerOf(packet).correlation_id.get());
    EXPECT_EQ(PACKET_FLAG_EVENT, headerOf(packet).flags.get());
}

// -----------------------------------------------------------------------------
// payload bound, both encodes
// -----------------------------------------------------------------------------

TEST_P(Payload, EncodeResponseHonoursTheWireBound)
{
    const PayloadCase& c = GetParam();
    DefaultPacketCodec codec;
    GameResponse       response;

    response.payload = patternBytes(_pool, c.length);
    ASSERT_EQ(c.length, response.payload.size());

    Bytes packet = codec.encode(_pool, response);

    EXPECT_EQ(c.expect_encoded, static_cast<bool>(packet));

    if (c.expect_encoded)
    {
        ASSERT_EQ(PACKET_HEADER_SIZE + c.length, packet.size());
        EXPECT_EQ(c.length, headerOf(packet).payload_length.get());
        EXPECT_TRUE(hasPattern(packet.data() + PACKET_HEADER_SIZE, c.length));
    }
    else
    {
        EXPECT_EQ(0u, packet.size());
    }
}

TEST_P(Payload, EncodeEventHonoursTheWireBound)
{
    const PayloadCase& c = GetParam();
    DefaultPacketCodec codec;
    GameEvent          event;

    event.payload = patternBytes(_pool, c.length);
    ASSERT_EQ(c.length, event.payload.size());

    Bytes packet = codec.encode(_pool, event, 1);

    EXPECT_EQ(c.expect_encoded, static_cast<bool>(packet));

    if (c.expect_encoded)
    {
        ASSERT_EQ(PACKET_HEADER_SIZE + c.length, packet.size());
        EXPECT_EQ(c.length, headerOf(packet).payload_length.get());
        EXPECT_TRUE(hasPattern(packet.data() + PACKET_HEADER_SIZE, c.length));
    }
    else
    {
        EXPECT_EQ(0u, packet.size());
    }
}

INSTANTIATE_TEST_SUITE_P(Bounds, Payload, ::testing::ValuesIn(kPayloadCases), nameOf);
