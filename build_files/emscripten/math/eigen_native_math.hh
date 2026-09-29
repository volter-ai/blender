/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "arm_reciprocal.hh"
#include <Eigen/Core>

#if defined(__EMSCRIPTEN__)
namespace Eigen::internal {
template<> EIGEN_STRONG_INLINE Packet4f preciprocal<Packet4f>(const Packet4f &value)
{
  alignas(16) float lanes[4];
  _mm_store_ps(lanes, value);
  for (float &lane : lanes) lane = blender::web::arm_reciprocal(lane);
  return _mm_load_ps(lanes);
}
}  // namespace Eigen::internal
#endif
