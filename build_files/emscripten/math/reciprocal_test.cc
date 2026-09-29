/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "arm_reciprocal.hh"
#if !defined(__EMSCRIPTEN__)
#include <arm_neon.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

int main()
{
  uint64_t tested = 0;
  uint32_t digest = 2166136261u;
  auto check = [&](uint32_t bits) {
    const float value = std::bit_cast<float>(bits);
#if !defined(__EMSCRIPTEN__)
    const float32x4_t input = vdupq_n_f32(value);
    float32x4_t native = vrecpeq_f32(input);
    native = vmulq_f32(vrecpsq_f32(input, native), native);
    native = vmulq_f32(vrecpsq_f32(input, native), native);
    const float expected = vgetq_lane_f32(native, 0);
#endif
    const float actual = blender::web::arm_reciprocal(value);
    ++tested;
#if !defined(__EMSCRIPTEN__)
    if (std::bit_cast<uint32_t>(expected) != std::bit_cast<uint32_t>(actual) &&
        !(std::isnan(expected) && std::isnan(actual))) {
      std::fprintf(stderr, "reciprocal %08x: native=%08x portable=%08x\n", bits,
                   std::bit_cast<uint32_t>(expected), std::bit_cast<uint32_t>(actual));
      std::exit(1);
    }
#endif
    const uint32_t canonical = std::isnan(actual) ? 0x7fc00000u : std::bit_cast<uint32_t>(actual);
    digest = (digest ^ canonical) * 16777619u;
  };
  // Every estimate-bin boundary, adjacent floats, signs and exponents; plus
  // deterministic full-bit-range samples, zeros, infinities and NaNs.
  for (uint32_t sign : {0u, 0x80000000u}) {
    for (uint32_t exponent = 0; exponent < 256; ++exponent)
      for (uint32_t fraction = 0; fraction < 256; ++fraction) {
        const uint32_t bits = sign | (exponent << 23) | (fraction << 15);
        check(bits); check(bits + 1); if (bits) check(bits - 1);
      }
  }
  uint32_t bits = 1;
  for (int i = 0; i < 1000000; ++i) {
    bits ^= bits << 13; bits ^= bits >> 17; bits ^= bits << 5; check(bits);
  }
  std::printf("%llu reciprocal cases; digest=%08x\n", (unsigned long long)tested, digest);
}
