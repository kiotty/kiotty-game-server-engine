#if !defined(KIOTTY_CORE_PACKET_CONCEPT_H)
#define KIOTTY_CORE_PACKET_CONCEPT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(_WIN32) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
     __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  define KIOTTY_HOST_LITTLE_ENDIAN 1
#endif

namespace kiotty
{

    template <typename T>
    class LittleEndian
    {
    public:
        static_assert(std::is_integral<T>::value &&
                      std::is_unsigned<T>::value &&
                      !std::is_same<T, bool>::value,
                      "a wire field is a fixed-width unsigned integer - bool, "
                      "enum, pointer and size_t all change size across platforms");

        T get() const
        {
#if defined(KIOTTY_HOST_LITTLE_ENDIAN)
            T value;
            std::memcpy(&value, _bytes, sizeof(T));
            return value;
#else
            T value = 0;

            for (size_t i = 0; i < sizeof(T); ++i)
            {
                value = static_cast<T>(value |
                        static_cast<T>(static_cast<T>(_bytes[i]) << (8u * i)));
            }
            return value;
#endif
        }

        void set(T value)
        {
#if defined(KIOTTY_HOST_LITTLE_ENDIAN)
            std::memcpy(_bytes, &value, sizeof(T));
#else
            for (size_t i = 0; i < sizeof(T); ++i)
            {
                _bytes[i] = static_cast<uint8_t>((value >> (8u * i)) & 0xFFu);
            }
#endif
        }

    private:
        uint8_t _bytes[sizeof(T)];
    };

    template <typename T, size_t N>
    struct WireStruct
    {
        static const size_t WIRE_SIZE = N;
    };

    struct PacketHeader : WireStruct<PacketHeader, 24>
    {
        LittleEndian<uint32_t> magic;
        LittleEndian<uint32_t> correlation_id;
        LittleEndian<uint64_t> timestamp;
        LittleEndian<uint16_t> command;
        LittleEndian<uint16_t> flags;
        LittleEndian<uint16_t> version;
        LittleEndian<uint16_t> payload_length;
    };

    static_assert(sizeof(PacketHeader) == PacketHeader::WIRE_SIZE,
                  "PacketHeader must be exactly 24 bytes on the wire");
    static_assert(alignof(PacketHeader) == 1,
                  "PacketHeader must need no alignment - it is cast from an "
                  "arbitrary offset in the receive buffer");
    static_assert(std::is_standard_layout<PacketHeader>::value,
                  "PacketHeader must be standard layout for that cast to name "
                  "the fields it is supposed to name");

    static const size_t PACKET_HEADER_SIZE = 24;

    static const size_t PACKET_MAX_PAYLOAD = 65535;

    static const uint32_t PACKET_MAGIC = 0x544F494Bu;

    static const uint8_t PACKET_VERSION_MAJOR = 1;
    static const uint8_t PACKET_VERSION_MINOR = 0;

    static const uint16_t PACKET_VERSION =
        static_cast<uint16_t>(PACKET_VERSION_MAJOR) |
        static_cast<uint16_t>(static_cast<uint16_t>(PACKET_VERSION_MINOR) << 8);

    inline bool hasPacketMagic(const PacketHeader& header)
    {
        return header.magic.get() == PACKET_MAGIC;
    }

    inline bool hasSupportedVersion(const PacketHeader& header)
    {
        return (header.version.get() & 0xFFu) == PACKET_VERSION_MAJOR;
    }

    static const uint16_t PACKET_FLAG_EVENT         = 1u << 0;
    static const uint16_t PACKET_FLAG_FRAGMENTED    = 1u << 1;
    static const uint16_t PACKET_FLAG_LAST_FRAGMENT = 1u << 2;
    static const uint16_t PACKET_FLAG_COMPRESSED    = 1u << 3;
}

#endif
