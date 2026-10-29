// SPDX-License-Identifier: MIT
#include "../tools/gaze_projection.h"
#include <cassert>
#include <limits>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace gaze_bridge;
static void near(float a, float b) { assert(std::abs(a-b) < 0.0001f); }
int main() {
  XrPosef gaze{{0,0,0,1},{0,0,0}};
  XrView eyes[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (auto& eye : eyes) {
    eye.pose.orientation.w = 1;
    eye.fov = {-0.785398163f, 0.785398163f, 0.785398163f, -0.785398163f};
  }
  eyes[0].pose.position.x = -0.032f;
  eyes[1].pose.position.x = 0.032f;
  auto sample = project(gaze, eyes, 0);
  assert(sample.valid);
  near(sample.u[0], .5f); near(sample.u[1], .5f); near(sample.v[0], .5f);
  sample = project(gaze, eyes, 1);
  assert(sample.valid);
  near(sample.u[0], .516f); near(sample.u[1], .484f);
  // Asymmetric FOVs give different UVs for the same ray.
  eyes[0].fov.angleLeft = -std::atan(2.f);
  sample = project(gaze, eyes, 0);
  near(sample.u[0], 2.f/3.f); near(sample.u[1], .5f);
  eyes[0].fov = eyes[1].fov;
  // Canted eye: inverse eye orientation must be applied.
  eyes[0].pose.orientation = {0,std::sin(.1f),0,std::cos(.1f)};
  sample = project(gaze, eyes, 0);
  near(sample.u[0], .5f + std::tan(.2f)/2);
  eyes[0].pose.orientation = {0,0,0,1};
  // Upward gaze must move V toward the top.
  gaze.orientation = {std::sin(.1f),0,0,std::cos(.1f)};
  sample = project(gaze, eyes, 0);
  near(sample.v[0], .5f-std::tan(.2f)/2);
  gaze.orientation = {0,1,0,0};
  assert(!project(gaze, eyes, 0).valid); // behind the eye
  gaze.orientation = {0,0,0,1};
  eyes[0].fov.angleLeft = eyes[0].fov.angleRight;
  assert(!project(gaze, eyes, 0).valid);
  eyes[0].fov = eyes[1].fov;
  gaze.position.x = std::numeric_limits<float>::quiet_NaN();
  assert(!project(gaze, eyes, 0).valid);

  SharedGaze packet{};
  Sample good{true, {.25f,.75f}, {.1f,.9f}}, read;
  uint32_t seq = 0;
  assert(!snapshot(&packet, read, seq));
  publish(&packet, good);
  assert(snapshot(&packet, read, seq) && read.valid);
  near(read.u[0], .25f);
  const auto now = Freshness::Clock::now();
  Freshness freshness(now);
  assert(!freshness.observe(seq, now)); // don't accept an abandoned file
  publish(&packet, good);
  assert(snapshot(&packet, read, seq));
  assert(freshness.observe(seq, now+std::chrono::milliseconds(4)));
  assert(!freshness.observe(seq, now+std::chrono::milliseconds(104)));
  publish(&packet, {});
  assert(snapshot(&packet, read, seq) && !read.valid);
  store(&packet.words[1], 999);
  assert(!snapshot(&packet, read, seq));
  publish(&packet, good);
  store(&packet.words[3], 5); // crashed mid-write
  assert(!snapshot(&packet, read, seq));
  publish(&packet, good); // restart recovers an odd sequence
  assert(snapshot(&packet, read, seq) && read.valid && !(seq & 1));
  store(&packet.words[3], 0xfffffffeu);
  publish(&packet, good); // sequence wrap
  assert(snapshot(&packet, read, seq) && seq == 0);
  good.u[0] = std::numeric_limits<float>::infinity();
  publish(&packet, good);
  assert(snapshot(&packet, read, seq) && !read.valid);

  // Exercise the protocol across processes with correlated fields. A torn
  // snapshot would violate these relationships even when every float is valid.
  auto* memory = static_cast<SharedGaze*>(mmap(nullptr, sizeof(SharedGaze),
    PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    for (unsigned i=0; i<50000; ++i) {
      float x = float(i % 1000)/1000;
      publish(memory, {true, {x,1-x}, {x,1-x}});
    }
    _exit(0);
  }
  unsigned reads = 0;
  for (unsigned i=0; i<100000; ++i) {
    if (snapshot(memory, read, seq) && read.valid) {
      near(read.u[0]+read.u[1], 1); near(read.u[0], read.v[0]); near(read.u[1], read.v[1]);
      ++reads;
    }
  }
  int status;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(snapshot(memory, read, seq) && read.valid);
  (void)reads;
  munmap(memory, sizeof(SharedGaze));
}
