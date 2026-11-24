#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Real SHA-256 (FIPS 180-4). Asset downloads compare these digests against
// catalog and companion-index hashes produced by host tooling, so a fake hash
// makes every verified download fail in the simulator.
struct mbedtls_sha256_context {
  uint32_t state[8] = {};
  uint64_t length = 0;
  uint8_t block[64] = {};
  size_t used = 0;
};

namespace sim_sha256 {
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline void compress(mbedtls_sha256_context *ctx, const uint8_t *data) {
  static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16) | (uint32_t(data[i * 4 + 2]) << 8) |
           data[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
  uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}
}  // namespace sim_sha256

inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int) {
  if (!ctx) return -1;
  static constexpr uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(ctx->state, initial, sizeof(initial));
  ctx->length = 0;
  ctx->used = 0;
  return 0;
}

inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {
  if (ctx) mbedtls_sha256_starts(ctx, 0);
}

inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen) {
  if (!ctx || (!input && ilen)) return -1;
  ctx->length += ilen;
  while (ilen) {
    const size_t room = sizeof(ctx->block) - ctx->used;
    const size_t take = room < ilen ? room : ilen;
    std::memcpy(ctx->block + ctx->used, input, take);
    ctx->used += take;
    input += take;
    ilen -= take;
    if (ctx->used == sizeof(ctx->block)) {
      sim_sha256::compress(ctx, ctx->block);
      ctx->used = 0;
    }
  }
  return 0;
}

inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]) {
  if (!ctx || !output) return -1;
  const uint64_t bits = ctx->length * 8;
  const uint8_t pad = 0x80;
  const uint8_t zero = 0;
  mbedtls_sha256_update(ctx, &pad, 1);
  while (ctx->used != 56) mbedtls_sha256_update(ctx, &zero, 1);
  uint8_t tail[8];
  for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bits >> (56 - 8 * i));
  mbedtls_sha256_update(ctx, tail, sizeof(tail));
  for (int i = 0; i < 8; ++i) {
    output[i * 4] = uint8_t(ctx->state[i] >> 24);
    output[i * 4 + 1] = uint8_t(ctx->state[i] >> 16);
    output[i * 4 + 2] = uint8_t(ctx->state[i] >> 8);
    output[i * 4 + 3] = uint8_t(ctx->state[i]);
  }
  return 0;
}

inline int mbedtls_sha256(const unsigned char *input, size_t ilen, unsigned char output[32], int is224) {
  if (is224 || !output || (!input && ilen)) return -1;
  mbedtls_sha256_context ctx;
  if (mbedtls_sha256_starts(&ctx, 0) != 0) return -1;
  if (mbedtls_sha256_update(&ctx, input, ilen) != 0) return -1;
  return mbedtls_sha256_finish(&ctx, output);
}

inline void mbedtls_sha256_free(mbedtls_sha256_context *) {}
