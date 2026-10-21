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
// thread_local buffer makes this allocation-free after the first message).
inline void encode_frame(std::vector<std::byte>& out, TypeId wire_type, std::uint64_t correlation,
                         ByteView payload, bool checksum = false) {
    const std::size_t hs = header_size(wire_type);
    out.resize(frame_size(wire_type, payload.size(), checksum));
    encode_header(out.data(), wire_type, correlation, payload.size(), checksum);
    if (!payload.empty()) std::memcpy(out.data() + hs, payload.data(), payload.size());
    if (checksum) seal_frame(out.data(), hs + payload.size());
}

// Header checks shared by both decoders. `require_checksum`: this end insists
// on CRCs, so a frame without one is refused.
inline std::error_code check_header(FrameHeader& h, const std::byte* p, std::size_t max_payload,
                                    bool require_checksum) noexcept {
    const std::uint32_t word = detail::load_u32(p);
    h.length = word & kLengthMask;
    h.checksummed = (word & kFlagChecksum) != 0;
    h.type = detail::load_u32(p + 4);
    h.correlation = 0;
    if (h.id() == 0) return ErrorCode::InvalidFrame;                       // id 0 is reserved
    if (h.is_request() && h.is_reply()) return ErrorCode::InvalidFrame;
    // Check BEFORE waiting for the payload: a corrupt length must never
    // make us buffer (or allocate) gigabytes.
    if (h.length > max_payload) return ErrorCode::MessageTooLarge;
    if (require_checksum && !h.checksummed) return ErrorCode::ChecksumMismatch;
    return {};
}

// Parse and validate ONE complete frame held in `frame` (a shared-memory slot,
// or anything else that delivers whole frames). The peer's bytes are untrusted:
// every field is checked before use. On success `payload` views inside `frame`.
inline std::error_code decode_frame(ByteView frame, std::size_t max_payload, bool require_checksum,
                                    FrameHeader& h, ByteView& payload) noexcept {
    if (frame.size() < kBaseHeaderSize) return ErrorCode::InvalidFrame;
    if (auto ec = check_header(h, frame.data(), max_payload, require_checksum)) return ec;
    const std::size_t hs = header_size(h.type);
    if (frame.size() != frame_size(h.type, h.length, h.checksummed)) return ErrorCode::InvalidFrame;
    if (hs == kRpcHeaderSize) h.correlation = detail::load_u64(frame.data() + 8);
    if (h.checksummed &&
        detail::load_u32(frame.data() + hs + h.length) != detail::crc32c(frame.data(), hs + h.length))
        return ErrorCode::ChecksumMismatch;
    payload = ByteView{frame.data() + hs, h.length};
    return {};
}

// Reassembles frames from an arbitrarily chunked byte stream.
//
// State is a single read offset into buf_. The fixed-size header is simply
// re-read when more bytes arrive, which is O(1) and removes a whole class of
// state-machine bugs.
class FrameReader {
public:
    // require_checksum: refuse frames that carry no CRC (frames that do carry
    // one are always verified).
    explicit FrameReader(std::size_t max_payload, bool require_checksum = false)
        : max_payload_(max_payload), checksum_(require_checksum) {}

    // Appends `incoming`, then calls on_frame(const FrameHeader&, ByteView payload)
    // once per COMPLETE frame. The payload view is valid only during the call.
    // Returns InvalidFrame/MessageTooLarge on a corrupt stream; the caller must
    // then close the connection (the stream cannot be resynchronised).
    template <class OnFrame>
    std::error_code feed(ByteView incoming, OnFrame&& on_frame) {
        buf_.insert(buf_.end(), incoming.begin(), incoming.end());

        for (;;) {
            const std::size_t avail = buf_.size() - start_;
            if (avail < kBaseHeaderSize) break;                       // need more bytes

            const std::byte* p = buf_.data() + start_;
            FrameHeader h;
            if (auto ec = check_header(h, p, max_payload_, checksum_)) return ec;

            const std::size_t hs = header_size(h.type);
            const std::size_t total = frame_size(h.type, h.length, h.checksummed);
            if (avail < total) break;                                 // frame incomplete
            if (hs == kRpcHeaderSize) h.correlation = detail::load_u64(p + 8);
            if (h.checksummed && detail::load_u32(p + hs + h.length) != detail::crc32c(p, hs + h.length))
                return ErrorCode::ChecksumMismatch;                   // corrupted: the stream is untrustworthy

            on_frame(static_cast<const FrameHeader&>(h), ByteView{p + hs, h.length});
            start_ += total;
        }
        compact();
        return {};
    }

    // Discard partial data. MUST be called when a connection is replaced
