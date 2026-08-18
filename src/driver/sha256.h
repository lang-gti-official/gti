// Internal driver SHA-256 used for cache and dependency content identity.
// This header is private to gti_driver translation units and is not
// installed; the toolchain has no external hashing dependency.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lang::driver {

class Sha256 final {
public:
  Sha256()
      : state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

  void update(std::string_view bytes) {
    totalBytes += bytes.size();
    for (const unsigned char byte : bytes) {
      buffer[bufferSize++] = byte;
      if (bufferSize == buffer.size()) {
        transform(buffer);
        bufferSize = 0;
      }
    }
  }

  [[nodiscard]] std::string finishHex() const {
    Sha256 copy = *this;
    const std::uint64_t bitLength = copy.totalBytes * 8U;
    copy.appendPaddingByte(0x80U);
    while (copy.bufferSize != 56U) {
      copy.appendPaddingByte(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      copy.appendPaddingByte(
          static_cast<std::uint8_t>((bitLength >> shift) & 0xffU));
    }

    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const std::uint32_t word : copy.state) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        result.push_back(hexadecimal[(word >> shift) & 0x0fU]);
      }
    }
    return result;
  }

private:
  static constexpr std::array<std::uint32_t, 64> roundConstants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
  };

  static std::uint32_t rotateRight(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32U - amount));
  }

  void appendPaddingByte(std::uint8_t byte) {
    buffer[bufferSize++] = byte;
    if (bufferSize == buffer.size()) {
      transform(buffer);
      bufferSize = 0;
    }
  }

  void transform(const std::array<std::uint8_t, 64> &block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t offset = index * 4;
      words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                     (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                     (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                     static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const std::uint32_t s0 = rotateRight(words[index - 15], 7U) ^
                               rotateRight(words[index - 15], 18U) ^
                               (words[index - 15] >> 3U);
      const std::uint32_t s1 = rotateRight(words[index - 2], 17U) ^
                               rotateRight(words[index - 2], 19U) ^
                               (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 =
          rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
      const std::uint32_t choice = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 =
          h + sum1 + choice + roundConstants[index] + words[index];
      const std::uint32_t sum0 =
          rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
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

  std::array<std::uint32_t, 8> state;
  std::array<std::uint8_t, 64> buffer{};
  std::uint64_t totalBytes = 0;
  std::size_t bufferSize = 0;
};

} // namespace lang::driver
