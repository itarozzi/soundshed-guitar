#include "util/Sha256.h"

#include <array>

namespace guitarfx::util
{
namespace
{
constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t RotateRight(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void CompressBlock(std::array<std::uint32_t, 8>& state, const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> w{};

    for (int i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }

    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
} // namespace

std::string Sha256Hex(std::span<const std::uint8_t> bytes)
{
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const std::size_t fullBlocks = bytes.size() / 64;

    for (std::size_t i = 0; i < fullBlocks; ++i)
    {
        CompressBlock(state, bytes.data() + i * 64);
    }

    // The tail, a 0x80 marker, zeros, and the length in bits as a big-endian 64-bit number:
    // one more block, or two when the tail leaves no room for the length.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t remaining = bytes.size() - fullBlocks * 64;

    for (std::size_t i = 0; i < remaining; ++i)
    {
        tail[i] = bytes[fullBlocks * 64 + i];
    }

    tail[remaining] = 0x80;
    const std::size_t tailSize = remaining + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;

    for (int i = 0; i < 8; ++i)
    {
        tail[tailSize - 1 - i] = static_cast<std::uint8_t>(bitLength >> (8 * i));
    }

    for (std::size_t offset = 0; offset < tailSize; offset += 64)
    {
        CompressBlock(state, tail.data() + offset);
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);

    for (const std::uint32_t word : state)
    {
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            hex.push_back(kHex[(word >> shift) & 0xf]);
        }
    }

    return hex;
}
} // namespace guitarfx::util
