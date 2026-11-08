// SavanXP capture-side cursor for the polled OpenBSD stream adapter.
#include "capture.hpp"

namespace azalia_port {
namespace { constexpr int kIoError = 5, kAgain = 11, kInvalid = 22; }
uint32_t capture_available(const stream_t* stream, const CaptureCursor& cursor) {
    if (cursor.failed || !stream->blk) return 0;
    const uint64_t periods = cursor.produced / stream->blk;
    const uint64_t safe = periods ? (periods - 1) * stream->blk : 0;
    return safe > cursor.consumed ? static_cast<uint32_t>(safe - cursor.consumed) : 0;
}
int capture_poll(stream_t* stream, CaptureCursor& cursor, uint64_t now) {
    if (!stream || !stream->az || !stream->bufsize || !stream->blk || cursor.failed) return kIoError;
    auto& io = stream->az->io;
    const uint32_t raw_position = io.read(io.context,stream->regbase+HDA_SD_LPIB,4);
    const uint32_t position = raw_position & ~3u;
    const uint32_t status = io.read(io.context,stream->regbase+HDA_SD_STS,1);
    if (raw_position >= stream->bufsize ||
        (status & (HDA_SD_STS_DESE|HDA_SD_STS_FIFOE)) || now < cursor.last_poll_ns ||
        now - cursor.last_poll_ns >= static_cast<uint64_t>(stream->bufsize) * 1000000000 / 192000) {
        cursor.failed = true; return kIoError;
    }
    cursor.produced += (position + stream->bufsize - cursor.position) % stream->bufsize;
    cursor.position = position; cursor.last_poll_ns = now;
    if (cursor.produced - cursor.consumed >= stream->bufsize - stream->blk) { cursor.failed = true; return kIoError; }
    return 0;
}
int capture_copy(stream_t* stream, CaptureCursor& cursor, uint64_t now, void* output, uint32_t bytes) {
    if (!stream || !output || !bytes || bytes > stream->blk || bytes % 4) return kInvalid;
    const int error = capture_poll(stream,cursor,now);
    if (error) return error;
    if (capture_available(stream,cursor) < bytes) return kAgain;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const auto* source = static_cast<volatile const uint8_t*>(stream->buffer.addr);
    auto* destination = static_cast<uint8_t*>(output);
    const uint32_t offset = cursor.consumed % stream->bufsize;
    for (uint32_t i = 0; i < bytes; ++i) destination[i] = source[(offset+i)%stream->bufsize];
    cursor.consumed += bytes;
    return 0;
}
} // namespace azalia_port
