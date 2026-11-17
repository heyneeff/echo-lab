// base64 of little-endian float32 (how the face hands dropped audio to the plugin)
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace echolab {

inline std::vector<float> decodeFloats(const std::string& b64) {  // base64 of little-endian float32
    struct Map { int8_t v[256]; Map() { std::memset(v, -1, sizeof v); const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) v[(uint8_t)a[i]] = (int8_t)i; } };
    static const Map m;
    const int8_t* map = m.v;
    std::vector<uint8_t> bytes; bytes.reserve(b64.size() * 3 / 4);
    uint32_t acc = 0; int bits = 0;
    for (char c : b64) { const int v = map[(uint8_t)c]; if (v < 0) continue; acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back((uint8_t)(acc >> bits)); } }
    std::vector<float> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), out.size() * 4);
    return out;
}

}  // namespace echolab
