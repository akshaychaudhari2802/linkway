// linkway/codec.hpp
// Wire format and stream reassembly.
//
//   bytes 0..3   length   (u32 LE)  payload bytes only, header excluded.
//                                   v2: bit 31 = a CRC trailer follows (kFlagChecksum)
//   bytes 4..7   type     (u32 LE)  user id | kFlagRequest | kFlagReply
//   bytes 8..15  corr     (u64 LE)  present only when a request/reply flag is set
//   then         payload  (length bytes)
//   then         crc32c   (u32 LE)  v2, present iff bit 31 of length is set: CRC32C of
//                                   header + payload. The flag makes every frame
//                                   self-describing, so a receiver that requires
//                                   checksums rejects an unchecked frame AT ONCE.
//                                   (Without the flag, a sender with checksums off
//                                   made a checking receiver wait forever for a
//                                   4-byte trailer that never came: found by
//                                   Crc32c.MismatchedSettingsOnASocketAreRejectedAtOnce.)
#pragma once

#include "linkway/detail/byte_order.hpp"
#include "linkway/detail/crc32c.hpp"
#include "linkway/error.hpp"
#include "linkway/message.hpp"
#include "linkway/vocab.hpp"

#include <cstdint>
#include <vector>

namespace linkway {

inline constexpr std::size_t kBaseHeaderSize = 8;
inline constexpr std::size_t kRpcHeaderSize = 16;
inline constexpr std::size_t kChecksumSize = 4;
inline constexpr std::uint32_t kFlagChecksum = 1u << 31;          // in the length word
inline constexpr std::uint32_t kLengthMask = kFlagChecksum - 1;

struct FrameHeader {
    std::uint32_t length = 0;        // payload bytes (flag removed)
    bool checksummed = false;        // a CRC trailer followed (and matched)
    TypeId type = 0;                 // raw wire value, flags included
    std::uint64_t correlation = 0;   // 0 unless request/reply

    TypeId id() const noexcept { return type & kTypeMask; }
    bool is_request() const noexcept { return (type & kFlagRequest) != 0; }
    bool is_reply() const noexcept { return (type & kFlagReply) != 0; }
};

constexpr std::size_t header_size(TypeId wire_type) noexcept {
    return (wire_type & (kFlagRequest | kFlagReply)) ? kRpcHeaderSize : kBaseHeaderSize;
}

// Total bytes of one frame on the wire.
constexpr std::size_t frame_size(TypeId wire_type, std::size_t payload, bool checksum) noexcept {
    return header_size(wire_type) + payload + (checksum ? kChecksumSize : 0);
}

// Write only the header into `dst` (header_size(wire_type) bytes). Used by the
// zero-copy path, which writes the payload in place itself.
inline void encode_header(std::byte* dst, TypeId wire_type, std::uint64_t correlation, std::size_t payload,
                          bool checksum = false) noexcept {
    detail::store_u32(dst, static_cast<std::uint32_t>(payload) | (checksum ? kFlagChecksum : 0u));
    detail::store_u32(dst + 4, wire_type);
    if (header_size(wire_type) == kRpcHeaderSize) detail::store_u64(dst + 8, correlation);
}

// Append the CRC32C of frame[0, body) at frame + body.
inline void seal_frame(std::byte* frame, std::size_t body) noexcept {
    detail::store_u32(frame + body, detail::crc32c(frame, body));
}

// Serialize one frame into `out` (resized; capacity reused across calls, so a
