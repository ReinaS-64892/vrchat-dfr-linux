// SPDX-License-Identifier: MIT
#define XR_USE_TIMESPEC
#include <ctime>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "gaze_projection.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
using Clock = std::chrono::steady_clock;
using gaze_bridge::Sample;

struct Output {
  int fd = -1;
  gaze_bridge::SharedGaze* memory = nullptr;
  explicit Output(const char* path) {
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) throw std::runtime_error(std::string("open output: ") + std::strerror(errno));
    // Do not truncate or replace an inode: existing DXVK mappings survive restarts.
    try {
      struct stat stat{};
      if (fstat(fd, &stat) || !S_ISREG(stat.st_mode) || stat.st_uid != getuid())
        throw std::runtime_error("Output must be a regular file owned by this user");
      if (flock(fd, LOCK_EX | LOCK_NB)) throw std::runtime_error("Another writer holds the output lock");
      if (stat.st_size != 0 && stat.st_size != sizeof(gaze_bridge::SharedGaze))
        throw std::runtime_error("Output has an unexpected size; use a different --file");
      if (fchmod(fd, 0600) || (stat.st_size == 0 && ftruncate(fd, sizeof(gaze_bridge::SharedGaze))))
        throw std::runtime_error("Cannot initialize output file");
      void* mapped = mmap(nullptr, sizeof(gaze_bridge::SharedGaze), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mapped == MAP_FAILED) throw std::runtime_error("Cannot map output file");
      memory = static_cast<gaze_bridge::SharedGaze*>(mapped);
      publish({});
    } catch (...) { close(fd); fd = -1; throw; }
  }
  void publish(const Sample& sample) { gaze_bridge::publish(memory, sample); }
  ~Output() {
    if (memory) { publish({}); munmap(memory, sizeof(gaze_bridge::SharedGaze)); }
    if (fd >= 0) close(fd);
  }
};

class Tracking {
public:
  XrInstance instance = XR_NULL_HANDLE;
  XrSession session = XR_NULL_HANDLE;
  XrActionSet set = XR_NULL_HANDLE;
  XrAction action = XR_NULL_HANDLE;
  XrSpace gaze = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
  PFN_xrConvertTimespecTimeToTimeKHR convert = nullptr;
  bool running = false;
  const char* reason = "waiting for session";
  XrTime sampleTime = 0, queryTime = 0;

  void check(XrResult result, const char* operation) {
    if (XR_FAILED(result)) {
      char name[XR_MAX_RESULT_STRING_SIZE] = {};
      if (instance) xrResultToString(instance, result, name);
      throw std::runtime_error(std::string(operation) + ": " + name + " (" + std::to_string(result) + ")");
    }
  }
#define CHECK(call) check((call), #call)
  void initialize() {
    uint32_t count = 0;
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()));
    const char* required[] = {XR_MND_HEADLESS_EXTENSION_NAME,
      XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
    for (auto name : required) {
      bool found = false;
      for (auto ext : extensions) found |= std::strcmp(ext.extensionName, name) == 0;
      if (!found) throw std::runtime_error(std::string("Missing extension: ") + name);
    }
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(create.applicationInfo.applicationName, "vrchat-dfr-gaze-bridge");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = 3;
    create.enabledExtensionNames = required;
    CHECK(xrCreateInstance(&create, &instance));
    XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
    CHECK(xrGetInstanceProperties(instance, &runtime));
    std::fprintf(stderr, "OpenXR runtime: %s\n", runtime.runtimeName);
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system;
    CHECK(xrGetSystem(instance, &systemInfo, &system));
    XrSystemEyeGazeInteractionPropertiesEXT eye{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &eye};
    CHECK(xrGetSystemProperties(instance, system, &properties));
    if (!eye.supportsEyeGazeInteraction) throw std::runtime_error("HMD does not expose eye gaze");
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.systemId = system;
    CHECK(xrCreateSession(instance, &sessionInfo, &session));
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gaze_bridge");
    std::strcpy(setInfo.localizedActionSetName, "Gaze Bridge");
    CHECK(xrCreateActionSet(instance, &setInfo, &set));
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(actionInfo.actionName, "gaze");
    std::strcpy(actionInfo.localizedActionName, "Gaze");
    CHECK(xrCreateAction(set, &actionInfo, &action));
    XrPath profile, path;
    CHECK(xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile));
    CHECK(xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &path));
    XrActionSuggestedBinding binding{action, path};
    XrInteractionProfileSuggestedBinding bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    bindings.interactionProfile = profile;
    bindings.countSuggestedBindings = 1;
    bindings.suggestedBindings = &binding;
    CHECK(xrSuggestInteractionProfileBindings(instance, &bindings));
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &set;
    CHECK(xrAttachSessionActionSets(session, &attach));
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = action;
    spaceInfo.poseInActionSpace.orientation.w = 1;
    CHECK(xrCreateActionSpace(session, &spaceInfo, &gaze));
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    reference.poseInReferenceSpace.orientation.w = 1;
    CHECK(xrCreateReferenceSpace(session, &reference, &view));
    CHECK(xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&convert)));
  }

  Sample read(float distance, double maxAgeMs) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while ((result = xrPollEvent(instance, &event)) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) throw std::runtime_error("Instance lost");
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
        if (state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
          begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          CHECK(xrBeginSession(session, &begin));
          running = true;
        } else if (state == XR_SESSION_STATE_STOPPING) {
          CHECK(xrEndSession(session));
          running = false;
        } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
          throw std::runtime_error("Session lost or exiting");
        }
      }
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    check(result, "xrPollEvent");
    sampleTime = queryTime = 0;
    reason = "waiting for session";
    if (!running) return {};
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now)) throw std::runtime_error("clock_gettime failed");
    CHECK(convert(instance, &now, &queryTime));
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    result = xrSyncActions(session, &sync);
    check(result, "xrSyncActions");
    reason = "session not focused";
    if (result != XR_SUCCESS) return {};
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
    CHECK(xrGetActionStatePose(session, &get, &state));
    reason = "gaze action inactive";
    if (!state.isActive) return {};
    XrEyeGazeSampleTimeEXT sample{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &sample};
    CHECK(xrLocateSpace(gaze, view, queryTime, &location));
    sampleTime = sample.time;
    constexpr XrSpaceLocationFlags gazeFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
      XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
      XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    reason = "gaze pose invalid or untracked";
    if ((location.locationFlags & gazeFlags) != gazeFlags) return {};
    reason = "gaze sample too old";
    if (sampleTime && double(queryTime - sampleTime)/1e6 > maxAgeMs) return {};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = queryTime;
    locate.space = view;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t viewCount = 0;
    CHECK(xrLocateViews(session, &locate, &viewState, 2, &viewCount, views));
    constexpr XrViewStateFlags viewFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    reason = "stereo view poses invalid";
    if (viewCount != 2 || (viewState.viewStateFlags & viewFlags) != viewFlags) return {};
    auto projected = gaze_bridge::project(location.pose, views, distance);
    reason = projected.valid ? "tracking" : "gaze projection invalid";
    return projected;
  }

  ~Tracking() {
    if (gaze) xrDestroySpace(gaze);
    if (view) xrDestroySpace(view);
    if (session) xrDestroySession(session);
    if (set) xrDestroyActionSet(set);
    if (instance) xrDestroyInstance(instance);
  }
};
#undef CHECK
} // namespace

int main(int argc, char** argv) {
  const char* file = "/dev/shm/vrchat_dfr_gaze";
  double hz = 250, distance = 0, maxAgeMs = 100, seconds = 0;
  bool verbose = false;
  const auto usage = [&] {
    std::fprintf(stderr, "Usage: %s [--file PATH] [--hz 20..1000] [--distance 0..100]\n"
      "  [--max-age-ms 1..1000] [--seconds N] [--verbose]\n"
      "Default: /dev/shm/vrchat_dfr_gaze, 250 Hz, infinite fixation distance.\n"
      "Retries OpenXR connection every 2 seconds. Ctrl-C stops and invalidates gaze.\n", argv[0]);
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help") { usage(); return 0; }
    if (arg == "--verbose") { verbose = true; continue; }
    if (i+1 == argc) { usage(); return 2; }
    const char* value = argv[++i];
    if (arg == "--file") { file = value; continue; }
    char* end;
    double parsed = std::strtod(value, &end);
    if (!*value || *end || !std::isfinite(parsed)) { usage(); return 2; }
    if (arg == "--hz" && parsed >= 20 && parsed <= 1000) hz = parsed;
    else if (arg == "--distance" && parsed >= 0 && parsed <= 100) distance = parsed;
    else if (arg == "--max-age-ms" && parsed >= 1 && parsed <= 1000) maxAgeMs = parsed;
    else if (arg == "--seconds" && parsed > 0 && parsed <= 86400) seconds = parsed;
    else { usage(); return 2; }
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  try {
    Output output(file);
    std::fprintf(stderr, "Gaze output: %s; %.0f Hz; fixation distance %.3f m (0=infinity)\n", file, hz, distance);
    std::unique_ptr<Tracking> tracking;
    auto retry = Clock::now(), nextLog = Clock::now(), started = Clock::now();
    unsigned long long samples = 0, valid = 0;
    bool wasValid = false;
    while (!stopping && (seconds == 0 || std::chrono::duration<double>(Clock::now()-started).count() < seconds)) {
      auto tick = Clock::now();
      Sample sample;
      try {
        if (!tracking && tick >= retry) {
          tracking = std::make_unique<Tracking>();
          tracking->initialize();
        }
        if (tracking) sample = tracking->read(float(distance), maxAgeMs);
      } catch (const std::exception& e) {
        // Invalidate before runtime cleanup, which may itself block.
        output.publish({});
        std::fprintf(stderr, "OpenXR: %s; retrying in 2s\n", e.what());
        tracking.reset();
        retry = Clock::now() + std::chrono::seconds(2);
      }
      output.publish(sample);
      ++samples;
      valid += sample.valid;
      if (sample.valid != wasValid) {
        std::fprintf(stderr, "Gaze %s\n", sample.valid ? "valid" : "unavailable (VRS disabled)");
        wasValid = sample.valid;
      }
      if (verbose && tick >= nextLog) {
        std::fprintf(stderr, "valid=%d reason=%s UV L=(%.4f,%.4f) R=(%.4f,%.4f) sample_age_ms=",
          sample.valid, tracking ? tracking->reason : "runtime unavailable",
          sample.u[0], sample.v[0], sample.u[1], sample.v[1]);
        if (tracking && tracking->sampleTime)
          std::fprintf(stderr, "%.3f\n", double(tracking->queryTime-tracking->sampleTime)/1e6);
        else std::fprintf(stderr, "unknown\n");
        nextLog = tick + std::chrono::seconds(1);
      }
      std::this_thread::sleep_until(tick + std::chrono::nanoseconds(static_cast<int64_t>(1e9/hz)));
    }
    output.publish({});
    std::fprintf(stderr, "Stopped: samples=%llu valid=%llu\n", samples, valid);
    // Invalidate before destroying the XR connection.
    tracking.reset();
    return seconds > 0 && !valid ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
