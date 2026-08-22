// Scratch verification for ReceiveBuffer. Proper unit tests are cpp-tester's
// job; this exists so nothing gets reported as working before it was run.
//
// ReceiveBuffer holds no step of its own, so this file has to play the part
// Connection will play: it owns the state machine and drives the two spaces.

#include <core/kiotty_block_pool.h>
#include <core/kiotty_connection_buffer.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace kiotty;

static int g_failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    }
}

static size_t makePacket(uint8_t* out, uint16_t command, const uint8_t* payload,
                         uint16_t payload_length, uint32_t magic = PACKET_MAGIC,
                         uint16_t version = PACKET_VERSION)
{
    PacketHeader* const header = reinterpret_cast<PacketHeader*>(out);

    header->magic.set(magic);
    header->correlation_id.set(7);
    header->timestamp.set(1234567);
    header->command.set(command);
    header->flags.set(0);
    header->version.set(version);
    header->payload_length.set(payload_length);

    if (payload_length > 0)
    {
        std::memcpy(out + PACKET_HEADER_SIZE, payload, payload_length);
    }
    return PACKET_HEADER_SIZE + payload_length;
}

// Stands in for Connection. The step lives here, not in the buffer.
class PacketReader
{
public:
    enum class Step
    {
        HEADER,
        PAYLOAD,
        READY,
        BROKEN,
    };

    explicit PacketReader(BlockPool& pool) :
        _buffer(pool),
        _step(Step::HEADER)
    {
    }

    Step step() const { return _step; }

    ByteSpan spaceToPost()
    {
        switch (_step)
        {
        case Step::HEADER:  return _buffer.headerSpace();
        case Step::PAYLOAD: return _buffer.payloadSpace();
        default:            return ByteSpan();
        }
    }

    void onReceived(size_t length)
    {
        if (_step == Step::HEADER)
        {
            _buffer.addHeaderReceived(length);

            if (_buffer.isHeaderComplete())
            {
                openPayload();
            }
            return;
        }

        if (_step == Step::PAYLOAD)
        {
            _buffer.addPayloadReceived(length);

            if (_buffer.isPayloadComplete())
            {
                _step = Step::READY;
            }
        }
    }

    const PacketHeader& header() const { return _buffer.header(); }

    Bytes take()
    {
        _step = Step::HEADER;
        return _buffer.takePayload();
    }

private:
    void openPayload()
    {
        if (!hasPacketMagic(_buffer.header()) || !hasSupportedVersion(_buffer.header()))
        {
            _step = Step::BROKEN;
            return;
        }

        if (!_buffer.openPayload())
        {
            _step = Step::BROKEN;
            return;
        }

        _step = _buffer.isPayloadComplete() ? Step::READY : Step::PAYLOAD;
    }

    ReceiveBuffer _buffer;
    Step          _step;
};

// Hands over at most step_limit bytes at a time, never more than the reader
// asked for. That second half is the whole point of the design - the kernel is
// told exactly how much room there is, so it can never spill into the next
// packet.
static size_t deliver(PacketReader& reader, const uint8_t* wire, size_t available,
                      size_t step_limit)
{
    size_t consumed = 0;

    while (consumed < available &&
           reader.step() != PacketReader::Step::READY &&
           reader.step() != PacketReader::Step::BROKEN)
    {
        const ByteSpan space = reader.spaceToPost();

        if (space.size() == 0)
        {
            break;
        }

        size_t step = available - consumed;

        if (step > space.size()) { step = space.size(); }
        if (step > step_limit)   { step = step_limit; }

        std::memcpy(space.data(), wire + consumed, step);
        reader.onReceived(step);
        consumed += step;
    }
    return consumed;
}

static void checkOnePacket()
{
    BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
    PacketReader reader(pool);

    uint8_t       wire[128];
    const uint8_t payload[5] = { 1, 2, 3, 4, 5 };

    check(reader.spaceToPost().size() == PACKET_HEADER_SIZE,
          "the first ask is exactly one header");

    const size_t length = makePacket(wire, 42, payload, 5);

    deliver(reader, wire, length, length);

    check(reader.step() == PacketReader::Step::READY, "the packet is complete");
    check(reader.header().command.get() == 42, "the header is readable");
    check(reader.header().correlation_id.get() == 7, "and so are its other fields");

    Bytes taken = reader.take();

    check(taken.size() == 5, "the payload length");
    check(std::memcmp(taken.data(), payload, 5) == 0, "the payload bytes");
    check(pool.fallbackCount() == 0, "the payload block came from the pool");

    check(reader.spaceToPost().size() == PACKET_HEADER_SIZE,
          "and the reader is asking for the next header");
}

// The buffer must never offer room beyond the packet being read. If it did, a
// fast sender's next packet would land inside this one's payload.
static void checkNeverAsksBeyondThePacket()
{
    BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
    PacketReader reader(pool);

    uint8_t       wire[256];
    const uint8_t payload[100] = { 0 };

    makePacket(wire, 1, payload, 100);

    check(reader.spaceToPost().size() == PACKET_HEADER_SIZE, "header phase asks for 24");

    deliver(reader, wire, PACKET_HEADER_SIZE, PACKET_HEADER_SIZE);

    check(reader.spaceToPost().size() == 100, "payload phase asks for exactly 100");

    deliver(reader, wire + PACKET_HEADER_SIZE, 60, 60);

    check(reader.spaceToPost().size() == 40, "then for exactly what is left");
}

// TCP is a stream: every split has to behave the same, and one byte at a time
// is the hardest peer.
static void checkArrivesInPieces()
{
    uint8_t       wire[256];
    const uint8_t payload[40] = { 0 };

    const size_t length = makePacket(wire, 3, payload, 40);

    for (size_t step = 1; step <= length; ++step)
    {
        BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
        PacketReader reader(pool);

        deliver(reader, wire, length, step);

        if (reader.step() != PacketReader::Step::READY)
        {
            check(false, "the packet must complete whatever the read size");
            break;
        }

        Bytes taken = reader.take();

        if (taken.size() != 40)
        {
            check(false, "the packet must survive being split");
            break;
        }
    }
}

static void checkBackToBack()
{
    BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
    PacketReader reader(pool);

    uint8_t       wire[512];
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    size_t total = 0;

    for (uint16_t i = 0; i < 4; ++i)
    {
        total += makePacket(wire + total, i, payload, 8);
    }

    size_t offset = 0;

    for (uint16_t i = 0; i < 4; ++i)
    {
        offset += deliver(reader, wire + offset, total - offset, total);

        check(reader.step() == PacketReader::Step::READY, "a packet completed");
        check(reader.header().command.get() == i, "in order");

        Bytes taken = reader.take();

        check(taken.size() == 8, "with its payload");
        check(std::memcmp(taken.data(), payload, 8) == 0, "intact");
    }

    check(offset == total, "every byte was consumed and none was over-read");
}

static void checkBrokenStream()
{
    uint8_t       wire[128];
    const uint8_t payload[4] = { 0 };

    {
        BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
        PacketReader reader(pool);
        const size_t length = makePacket(wire, 1, payload, 4, 0xDEADBEEFu);

        deliver(reader, wire, length, length);
        check(reader.step() == PacketReader::Step::BROKEN, "a wrong magic breaks it");
    }

    {
        BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
        PacketReader reader(pool);
        const size_t length = makePacket(wire, 1, payload, 4, PACKET_MAGIC, 0x0002u);

        deliver(reader, wire, length, length);
        check(reader.step() == PacketReader::Step::BROKEN,
              "a different major version breaks it");
    }

    {
        BlockPool      pool(defaultBlockClasses(), defaultBlockClassCount());
        PacketReader   reader(pool);
        const uint16_t newer_minor =
            static_cast<uint16_t>(PACKET_VERSION_MAJOR | (9u << 8));
        const size_t   length = makePacket(wire, 1, payload, 4, PACKET_MAGIC, newer_minor);

        deliver(reader, wire, length, length);
        check(reader.step() == PacketReader::Step::READY,
              "a newer minor version is still ours");
    }
}

static void checkEmptyPayload()
{
    BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
    PacketReader reader(pool);
    uint8_t      wire[64];

    const size_t length = makePacket(wire, 5, nullptr, 0);

    deliver(reader, wire, length, length);

    check(reader.step() == PacketReader::Step::READY, "an empty payload is still a packet");

    Bytes taken = reader.take();

    check(taken.size() == 0, "with nothing in it");
    check(pool.fallbackCount() == 0, "and it took no block at all");
}

static void checkLargestPayload()
{
    BlockPool    pool(defaultBlockClasses(), defaultBlockClassCount());
    PacketReader reader(pool);

    std::vector<uint8_t> wire(PACKET_HEADER_SIZE + PACKET_MAX_PAYLOAD);
    std::vector<uint8_t> payload(PACKET_MAX_PAYLOAD);

    for (size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>(i);
    }

    const size_t length = makePacket(wire.data(), 9, payload.data(),
                                     static_cast<uint16_t>(PACKET_MAX_PAYLOAD));

    deliver(reader, wire.data(), length, 4096);

    check(reader.step() == PacketReader::Step::READY, "a maximum size payload completes");

    Bytes taken = reader.take();

    check(taken.size() == PACKET_MAX_PAYLOAD, "at full length");
    check(std::memcmp(taken.data(), payload.data(), PACKET_MAX_PAYLOAD) == 0, "intact");
}

// The point of handing the payload over: the buffer is ready for the next
// packet while a worker still holds the previous one.
static void checkPayloadOutlivesTheBuffer()
{
    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    uint8_t       wire[256];
    const uint8_t first_payload[16]  = { 1 };
    const uint8_t second_payload[16] = { 2 };

    std::vector<Bytes> held_by_workers;

    {
        PacketReader reader(pool);

        size_t length = makePacket(wire, 1, first_payload, 16);
        deliver(reader, wire, length, length);
        held_by_workers.push_back(reader.take());

        length = makePacket(wire, 2, second_payload, 16);
        deliver(reader, wire, length, length);

        check(reader.step() == PacketReader::Step::READY,
              "the next packet arrived while the first was held");
        held_by_workers.push_back(reader.take());

        check(held_by_workers[0].data() != held_by_workers[1].data(),
              "the two payloads are separate blocks");
    }

    check(held_by_workers[0].size() == 16 && held_by_workers[1].size() == 16,
          "both payloads outlive the buffer that received them");
    check(held_by_workers[0].data()[0] == 1 && held_by_workers[1].data()[0] == 2,
          "and still hold the right bytes");
}

static void checkBlocksAreReturned()
{
    const BlockClass classes[] = { { 128, 2 } };
    BlockPool        pool(classes, 1);

    uint8_t       wire[256];
    const uint8_t payload[64] = { 0 };
    PacketReader  reader(pool);

    for (int round = 0; round < 1000; ++round)
    {
        const size_t length = makePacket(wire, 1, payload, 64);

        deliver(reader, wire, length, length);

        if (reader.step() != PacketReader::Step::READY)
        {
            check(false, "round did not complete");
            break;
        }

        Bytes taken = reader.take();

        if (taken.size() != 64)
        {
            check(false, "payload lost");
            break;
        }
    }
    check(pool.fallbackCount() == 0, "1000 packets ran on two pooled blocks");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    checkOnePacket();
    checkNeverAsksBeyondThePacket();
    checkArrivesInPieces();
    checkBackToBack();
    checkBrokenStream();
    checkEmptyPayload();
    checkLargestPayload();
    checkPayloadOutlivesTheBuffer();
    checkBlocksAreReturned();

    std::printf("receive buffer check failures=%d\n%s\n",
                g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
