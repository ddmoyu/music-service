#include "providers/qianqian/client.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <limits>

namespace music_service::providers::qianqian {
std::string decode_body(std::string body, std::string encoding, std::size_t limit, const ports::OperationContext& context) {
    if (limit == 0 || body.size() > limit || body.size() > std::numeric_limits<uInt>::max())
        throw std::invalid_argument{"compressed body limit"};
    while (!encoding.empty() && (encoding.back() == ' ' || encoding.back() == '\t')) encoding.pop_back();
    const auto first = encoding.find_first_not_of(" \t");
    encoding.erase(0, first == encoding.npos ? encoding.size() : first);
    std::transform(encoding.begin(), encoding.end(), encoding.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (encoding.empty() || encoding == "identity") return body;
    if (encoding != "gzip") throw std::invalid_argument{"unsupported content encoding"};

    // 上游忽略 identity 也会压缩。压缩前后分别设限，校验 gzip CRC 和结束位置。
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK)
        throw Error{ErrorCode::internal, "Qianqian gzip initialization failed", false, {}};
    struct End { z_stream* stream; ~End() { inflateEnd(stream); } } end{&stream};
    stream.next_in = reinterpret_cast<Bytef*>(body.data());
    stream.avail_in = static_cast<uInt>(body.size());
    std::array<char, 8192> buffer{};
    std::string result;
    while (true) {
        if (context.stop.stop_requested()) throw Error{ErrorCode::canceled, "Qianqian decompression canceled", false, {}};
        if (context.expired(std::chrono::steady_clock::now()))
            throw Error{ErrorCode::deadline_exceeded, "Qianqian decompression deadline exceeded", false, {}};
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        const auto status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = buffer.size() - stream.avail_out;
        if (produced > limit - result.size()) throw std::invalid_argument{"decompressed body limit"};
        result.append(buffer.data(), produced);
        if (status == Z_STREAM_END) {
            if (stream.avail_in != 0) throw std::invalid_argument{"trailing gzip data"};
            return result;
        }
        if (status != Z_OK || (produced == 0 && stream.avail_in == 0)) throw std::invalid_argument{"incomplete or corrupt gzip"};
    }
}
}
