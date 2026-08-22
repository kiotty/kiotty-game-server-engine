// Scratch verification for the core types. Not part of the repo - proper unit
// tests are cpp-tester's job. This only proves the runtime behaviour of what
// was just written before it gets reported as working.

#include <core/kiotty_block_pool.h>
#include <core/kiotty_bytes.h>
#include <core/kiotty_connection_buffer.h>
#include <core/kiotty_packet_concept.h>
#include <core/kiotty_ring_buffer.h>

#include <cstdio>
#include <cstring>
#include <thread>
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

static void fill(Bytes& target, const void* source)
{
    if (target.size() > 0)
    {
        std::memcpy(target.writableSpan().data(), source, target.size());
    }
}

static void checkByteView()
{
    const uint8_t raw[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    ByteView empty;
    check(!static_cast<bool>(empty), "default ByteView is false");
    check(empty.size() == 0, "default ByteView is empty");

    ByteView all(raw, sizeof(raw));
    check(static_cast<bool>(all), "ByteView over data is true");
    check(all.size() == 8, "ByteView size");

    const ByteView mid = all.slice(2, 3);
    check(mid.size() == 3, "slice size");
    check(mid.data()[0] == 2 && mid.data()[2] == 4, "slice contents");

    const ByteView tail = all.slice(8, 0);
    check(tail.size() == 0, "slice at the very end is legal and empty");

    uint8_t  scratch[4] = { 0, 0, 0, 0 };
    ByteSpan span(scratch, sizeof(scratch));

    span.data()[1] = 9;
    check(span.view().data()[1] == 9, "ByteSpan writes are visible through view()");
    check(span.slice(1, 2).size() == 2, "ByteSpan slice size");
}

static void checkBytes()
{
    const uint8_t raw[5] = { 10, 20, 30, 40, 50 };

    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    Bytes empty;
    check(!static_cast<bool>(empty), "default Bytes is false");
    check(empty.size() == 0 && empty.data() == nullptr, "default Bytes reads as nothing");

    Bytes owned(pool, sizeof(raw));
    fill(owned, raw);

    check(static_cast<bool>(owned), "a length produces a block");
    check(owned.size() == 5, "the size is kept");
    check(std::memcmp(owned.data(), raw, 5) == 0, "what was written is there");
    check(owned.data() != raw, "and it is a block of its own");
    check(pool.fallbackCount() == 0, "the block came from the pool");

    const uint8_t* const block = owned.data();

    Bytes moved = std::move(owned);
    check(moved.data() == block, "a move keeps the block");
    check(!static_cast<bool>(owned), "a moved-from Bytes is empty");

    Bytes assigned;
    assigned = std::move(moved);
    check(assigned.data() == block, "move assignment keeps the block");
    check(!static_cast<bool>(moved), "and empties the source");

    // Only two states exist: no pool and no block, or both.
    Bytes zero_length(pool, 0);
    check(!static_cast<bool>(zero_length), "zero length is empty");
    check(zero_length.writableSpan().size() == 0, "and offers no room to write");
    check(zero_length.writableSpan().data() == nullptr, "and no pointer to write through");
}

// A block has exactly one owner now, so it goes back the moment that owner lets
// go - there is no counting left to get wrong.
static void checkBlocksComeBack()
{
    const BlockClass classes[] = { { 64, 1 } };
    BlockPool        pool(classes, 1);

    {
        Bytes only(pool, 64);

        check(static_cast<bool>(only), "the one block was taken");
        check(pool.fallbackCount() == 0, "from the region");

        Bytes second(pool, 64);
        check(pool.fallbackCount() == 1, "the region is empty, so this fell back");
    }

    Bytes reused(pool, 64);
    check(static_cast<bool>(reused) && pool.fallbackCount() == 1,
          "the block came back and was reused");

    // Move assignment has to release what it replaces, or that block leaks.
    Bytes replaced(pool, 64);
    check(pool.fallbackCount() == 2, "a second one falls back while the first is held");

    replaced = Bytes();
    reused   = Bytes();

    Bytes after(pool, 64);
    check(static_cast<bool>(after) && pool.fallbackCount() == 2,
          "both were released, so this came from the region");
}

// Many threads each building and dropping their own blocks. Nothing is shared
// between them any more, but the pool underneath still is.
static void checkAcrossThreads()
{
    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t)
    {
        threads.push_back(std::thread([&pool, t]()
        {
            for (int i = 0; i < 20000; ++i)
            {
                const size_t length = 64 + static_cast<size_t>((t * 37 + i) % 900);

                Bytes local(pool, length);

                if (!static_cast<bool>(local) || local.size() != length)
                {
                    std::printf("[FAIL] a thread could not get a block\n");
                    ++g_failures;
                    return;
                }

                local.writableSpan().data()[0]          = static_cast<uint8_t>(t);
                local.writableSpan().data()[length - 1] = static_cast<uint8_t>(t);

                if (local.data()[0] != static_cast<uint8_t>(t) ||
                    local.data()[length - 1] != static_cast<uint8_t>(t))
                {
                    std::printf("[FAIL] a block was handed to two threads at once\n");
                    ++g_failures;
                    return;
                }
            }
        }));
    }

    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
}

static void checkRingBuffer()
{
    RingBuffer<int> ring(3);
    int             out = -1;

    check(static_cast<bool>(ring), "ring allocated");
    check(ring.capacity() == 3 && ring.empty(), "ring starts empty");
    check(!ring.tryPop(out) && out == -1, "popping an empty ring leaves out alone");

    check(ring.tryPush(1) && ring.tryPush(2) && ring.tryPush(3), "fills to capacity");
    check(ring.full() && ring.size() == 3, "full at capacity");
    check(!ring.tryPush(4), "pushing into a full ring is refused");

    check(ring.tryPop(out) && out == 1, "FIFO order");
    check(ring.tryPush(4), "a slot freed by a pop is usable");

    check(ring.tryPop(out) && out == 2, "order across the wrap (2)");
    check(ring.tryPop(out) && out == 3, "order across the wrap (3)");
    check(ring.tryPop(out) && out == 4, "order across the wrap (4)");
    check(ring.empty(), "empty again");

    for (int i = 0; i < 100; ++i)
    {
        check(ring.tryPush(i), "lap push");
        check(ring.tryPop(out) && out == i, "lap pop");
    }

    RingBuffer<int> none(0);
    check(!static_cast<bool>(none), "zero capacity ring is false");
    check(!none.tryPush(1), "zero capacity ring takes nothing");
}

static void checkSendBuffer()
{
    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer send(3);

    check(static_cast<bool>(send), "the queue allocated");
    check(send.empty() && send.capacity() == 3, "and starts empty");
    check(!static_cast<bool>(send.pop()), "popping an empty queue gives an empty Bytes");

    const uint8_t* blocks[3];

    for (size_t i = 0; i < 3; ++i)
    {
        Bytes packet(pool, 100 + i);

        packet.writableSpan().data()[0] = static_cast<uint8_t>(i);
        blocks[i] = packet.data();

        check(send.tryPush(packet), "the queue takes a packet");
        check(!static_cast<bool>(packet), "and the caller's Bytes was moved from");
    }

    check(send.full() && send.size() == 3, "full at capacity");

    // A refused push has to leave the packet with its caller, or the payload is
    // lost in the gap between the two of them.
    Bytes                refused(pool, 50);
    const uint8_t* const refused_block = refused.data();

    check(!send.tryPush(refused), "a full queue refuses");
    check(static_cast<bool>(refused), "and gives the packet back untouched");
    check(refused.data() == refused_block, "with the same block");

    for (size_t i = 0; i < 3; ++i)
    {
        Bytes taken = send.pop();

        check(static_cast<bool>(taken), "a packet comes out");
        check(taken.data() == blocks[i], "in the order it went in");
        check(taken.size() == 100 + i, "with its length");
        check(taken.data()[0] == static_cast<uint8_t>(i), "and its bytes");
    }

    check(send.empty(), "the queue drained");
}

// A queue destroyed with packets still in it has to give their blocks back, or
// a dropped connection leaks everything it had waiting.
static void checkSendBufferReleasesOnDestruction()
{
    const BlockClass classes[] = { { 128, 4 } };
    BlockPool        pool(classes, 1);

    {
        SendBuffer send(4);

        for (int i = 0; i < 4; ++i)
        {
            Bytes packet(pool, 64);
            check(send.tryPush(packet), "queued");
        }
        check(pool.fallbackCount() == 0, "all four came from the region");
    }

    for (int i = 0; i < 4; ++i)
    {
        Bytes packet(pool, 64);
        check(static_cast<bool>(packet), "a block is available again");
    }
    check(pool.fallbackCount() == 0, "the destroyed queue gave every block back");
}

static void checkLittleEndian()
{
    LittleEndian<uint16_t> small;
    LittleEndian<uint32_t> medium;
    LittleEndian<uint64_t> large;

    small.set(0x1234u);
    check(small.get() == 0x1234u, "uint16 round trip");
    check(reinterpret_cast<const uint8_t*>(&small)[0] == 0x34, "uint16 low byte first");
    check(reinterpret_cast<const uint8_t*>(&small)[1] == 0x12, "uint16 high byte second");

    medium.set(0xDEADBEEFu);
    check(medium.get() == 0xDEADBEEFu, "uint32 round trip");
    check(reinterpret_cast<const uint8_t*>(&medium)[0] == 0xEF, "uint32 low byte first");
    check(reinterpret_cast<const uint8_t*>(&medium)[3] == 0xDE, "uint32 high byte last");

    large.set(0x0123456789ABCDEFull);
    check(large.get() == 0x0123456789ABCDEFull, "uint64 round trip");
    check(reinterpret_cast<const uint8_t*>(&large)[0] == 0xEF, "uint64 low byte first");
    check(reinterpret_cast<const uint8_t*>(&large)[7] == 0x01, "uint64 high byte last");

    small.set(0);
    check(small.get() == 0, "zero round trip");
    small.set(0xFFFFu);
    check(small.get() == 0xFFFFu, "all ones round trip");
}

// The point of the whole wire-struct arrangement: a header read out of an
// arbitrary offset in a byte buffer, with no alignment and no byte swapping at
// the call site.
static void checkPacketHeaderInPlace()
{
    check(sizeof(PacketHeader) == 24, "header is 24 bytes");
    check(PACKET_VERSION == 0x0001u, "version packs major low, minor high");

    for (size_t offset = 0; offset < 8; ++offset)
    {
        uint8_t buffer[64];
        std::memset(buffer, 0, sizeof(buffer));

        PacketHeader* const written =
            reinterpret_cast<PacketHeader*>(buffer + offset);

        written->magic.set(PACKET_MAGIC);
        written->correlation_id.set(0x11223344u);
        written->timestamp.set(0x00FF00FF00FF00FFull);
        written->command.set(0xABCDu);
        written->flags.set(PACKET_FLAG_EVENT | PACKET_FLAG_LAST_FRAGMENT);
        written->version.set(PACKET_VERSION);
        written->payload_length.set(1024u);

        const PacketHeader* const read =
            reinterpret_cast<const PacketHeader*>(buffer + offset);

        const bool ok =
            read->magic.get() == PACKET_MAGIC &&
            read->correlation_id.get() == 0x11223344u &&
            read->timestamp.get() == 0x00FF00FF00FF00FFull &&
            read->command.get() == 0xABCDu &&
            read->flags.get() == (PACKET_FLAG_EVENT | PACKET_FLAG_LAST_FRAGMENT) &&
            read->version.get() == PACKET_VERSION &&
            read->payload_length.get() == 1024u;

        check(ok, "header round trips at an arbitrary offset");
        check(hasPacketMagic(*read) && hasSupportedVersion(*read), "and reads as ours");

        check(buffer[offset + 0] == 'K' && buffer[offset + 1] == 'I' &&
              buffer[offset + 2] == 'O' && buffer[offset + 3] == 'T',
              "magic reads KIOT on the wire");

        check(buffer[offset + 24] == 0, "header wrote nothing past its 24 bytes");
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    checkByteView();
    checkBytes();
    checkBlocksComeBack();
    checkAcrossThreads();
    checkRingBuffer();
    checkSendBuffer();
    checkSendBufferReleasesOnDestruction();
    checkLittleEndian();
    checkPacketHeaderInPlace();

    std::printf("core check failures=%d\n%s\n", g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
