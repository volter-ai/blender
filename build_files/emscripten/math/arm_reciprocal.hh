/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>

namespace blender::web {
/** ARM's 8-bit reciprocal estimate, followed by Eigen NEON's two refinement
 * steps. Implemented with integer significands and explicit fused operations
 * so Wasm uses the native arm64 reference's rounding, including subnormals.
 * This is the round-to-nearest, gradual-underflow profile used by Blender. */
inline float arm_reciprocal(float value)
{
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t sign = bits & 0x80000000u;
  const uint32_t magnitude = bits & 0x7fffffffu;
  if (magnitude > 0x7f800000u) return std::bit_cast<float>(bits | 0x00400000u);
  uint32_t estimate_bits;
  if (magnitude == 0x7f800000u) estimate_bits = sign;
  else if (magnitude < 0x00200000u) estimate_bits = sign | 0x7f800000u;
  else {
    int exponent = int(magnitude >> 23);
    uint32_t fraction = magnitude & 0x007fffffu;
    if (exponent == 0) {
      if (fraction & 0x00400000u) fraction <<= 1;
      else { fraction <<= 2; exponent = -1; }
    }
    const uint32_t significand = 256u | ((fraction >> 15) & 255u);
    const uint32_t estimate = (((1u << 19) / (2 * significand + 1)) + 1) / 2;
    int result_exponent = 253 - exponent;
    uint32_t result_fraction = (estimate & 255u) << 15;
    if (result_exponent == 0) result_fraction = (result_fraction >> 1) | 0x00400000u;
    else if (result_exponent == -1) {
      result_fraction = (result_fraction >> 2) | 0x00200000u;
      result_exponent = 0;
    }
    estimate_bits = sign | (uint32_t(result_exponent) << 23) | result_fraction;
  }
  float guess = std::bit_cast<float>(estimate_bits);
  for (int step = 0; step < 2; step++) {
    // FRECPS defines infinity * zero as a correction of two, not NaN.
    const float correction = ((std::isinf(value) && guess == 0.0f) ||
                              (value == 0.0f && std::isinf(guess))) ?
                                 2.0f : std::fma(-value, guess, 2.0f);
    guess *= correction;
  }
  return guess;
}
}  // namespace blender::web
