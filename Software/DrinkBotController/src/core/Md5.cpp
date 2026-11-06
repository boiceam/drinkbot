#include "core/Md5.h"

#include <cstdio>
#include <cstring>

namespace drinkbot {

namespace {
const uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const uint8_t kS[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
                        14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                        4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};
inline uint32_t rotl(uint32_t x, uint8_t n) { return (x << n) | (x >> (32 - n)); }
}  // namespace

void Md5::reset() {
  state_[0] = 0x67452301;
  state_[1] = 0xefcdab89;
  state_[2] = 0x98badcfe;
  state_[3] = 0x10325476;
  bytes_ = 0;
  used_ = 0;
}

void Md5::block(const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++) {
    m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    uint32_t next = d;
    d = c;
    c = b;
    b = b + rotl(a + f + kK[i] + m[g], kS[i]);
    a = next;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::add(const uint8_t* data, size_t len) {
  bytes_ += len;
  while (len > 0) {
    if (used_ == 0 && len >= 64) {
      block(data);
      data += 64;
      len -= 64;
      continue;
    }
    size_t n = 64 - used_;
    if (n > len) {
      n = len;
    }
    memcpy(buffer_ + used_, data, n);
    used_ += n;
    data += n;
    len -= n;
    if (used_ == 64) {
      block(buffer_);
      used_ = 0;
    }
  }
}

std::string Md5::finish() {
  uint64_t bits = bytes_ * 8;
  const uint8_t pad = 0x80;
  add(&pad, 1);
  const uint8_t zero = 0;
  while (used_ != 56) {
    add(&zero, 1);
  }
  uint8_t length[8];
  for (int i = 0; i < 8; i++) {
    length[i] = static_cast<uint8_t>(bits >> (8 * i));
  }
  add(length, 8);
  char hex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hex + i * 2, 3, "%02x", static_cast<unsigned>((state_[i / 4] >> (8 * (i % 4))) & 0xFF));
  }
  return std::string(hex, 32);
}

}  // namespace drinkbot
