// SPDX-License-Identifier: MIT
#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include "../shared/gaze_shm.h"

namespace gaze_bridge {
inline bool quaternionValid(XrQuaternionf q) {
  float n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
  return std::isfinite(n) && std::abs(n - 1.f) < 0.01f;
}
inline bool vectorValid(XrVector3f v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
  XrVector3f t{2*(q.y*v.z-q.z*v.y), 2*(q.z*v.x-q.x*v.z), 2*(q.x*v.y-q.y*v.x)};
  return {v.x+q.w*t.x+q.y*t.z-q.z*t.y, v.y+q.w*t.y+q.z*t.x-q.x*t.z,
          v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
// distance == 0 means infinity; positive values are an assumed fixation distance
// along the ray, not a measured convergence distance.
inline Sample project(const XrPosef& gaze, const XrView (&views)[2], float distance) {
  Sample result;
  if (!quaternionValid(gaze.orientation) || !vectorValid(gaze.position)) return result;
  XrVector3f direction = rotate(gaze.orientation, {0, 0, -1});
  for (unsigned eye = 0; eye < 2; ++eye) {
    auto pose = views[eye].pose;
    if (!quaternionValid(pose.orientation) || !vectorValid(pose.position)) return result;
    auto d = direction;
    if (distance > 0) {
      d = {gaze.position.x+distance*d.x-pose.position.x,
           gaze.position.y+distance*d.y-pose.position.y,
           gaze.position.z+distance*d.z-pose.position.z};
    }
    d = rotate({-pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w}, d);
    if (!vectorValid(d) || d.z >= -0.00001f) return result;
    auto fov = views[eye].fov;
    constexpr float halfPi = 1.57079632679f;
    for (float angle : {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown})
      if (!std::isfinite(angle) || std::abs(angle) >= halfPi) return result;
    float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
    float up = std::tan(fov.angleUp), down = std::tan(fov.angleDown);
    if (right-left < 0.00001f || up-down < 0.00001f) return result;
    float u = (d.x/-d.z-left)/(right-left), v = (up-d.y/-d.z)/(up-down);
    if (!std::isfinite(u) || !std::isfinite(v)) return result;
    result.u[eye] = std::clamp(u, 0.f, 1.f);
    result.v[eye] = std::clamp(v, 0.f, 1.f);
  }
  result.valid = true;
  return result;
}
} // namespace gaze_bridge
