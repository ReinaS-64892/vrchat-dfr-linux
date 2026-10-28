// SPDX-License-Identifier: MIT
// Native Linux diagnostic: no swapchain, frame submission or shared-memory writes.
#define XR_USE_TIMESPEC
#include <ctime>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

static XrInstance instance = XR_NULL_HANDLE;

static void check(XrResult result, const char* operation) {
  if (XR_FAILED(result)) {
    char name[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance) xrResultToString(instance, result, name);
    std::fprintf(stderr, "%s: %s (%d)\n", operation, name, result);
    throw std::runtime_error(operation);
  }
}
#define XR_CHECK(call) check((call), #call)

int main(int argc, char** argv) {
  const auto usage = [&] {
    std::fprintf(stderr, "Usage: %s [--seconds 1..3600]\n"
      "Default: enumerate extensions only. --seconds creates a headless session.\n", argv[0]);
  };
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    usage();
    return 0;
  }
  int seconds = 0;
  if (argc == 3 && std::strcmp(argv[1], "--seconds") == 0) {
    char* end = nullptr;
    long parsed = std::strtol(argv[2], &end, 10);
    if (!*argv[2] || *end || parsed < 1 || parsed > 3600) {
      usage();
      return 2;
    }
    seconds = static_cast<int>(parsed);
  } else if (argc != 1) {
    usage();
    return 2;
  }
  XrSession session = XR_NULL_HANDLE;
  XrActionSet set = XR_NULL_HANDLE;
  XrSpace gaze = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
  int status = 1;
  try {
    uint32_t count = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()));
    const char* required[] = {XR_MND_HEADLESS_EXTENSION_NAME,
      XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
    bool supported = true;
    for (auto name : required) {
      bool found = false;
      for (const auto& ext : extensions) found |= std::strcmp(ext.extensionName, name) == 0;
      std::printf("%s: %s\n", name, found ? "available" : "MISSING");
      supported &= found;
    }
    if (!supported) throw std::runtime_error("Required extensions missing");
    if (!seconds) return 0;

    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(create.applicationInfo.applicationName, "vrchat-dfr-gaze-probe");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = 3;
    create.enabledExtensionNames = required;
    XR_CHECK(xrCreateInstance(&create, &instance));
    XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(instance, &runtime));
    std::printf("runtime: %s\n", runtime.runtimeName);
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system;
    XR_CHECK(xrGetSystem(instance, &systemInfo, &system));
    XrSystemEyeGazeInteractionPropertiesEXT eye{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &eye};
    XR_CHECK(xrGetSystemProperties(instance, system, &properties));
    std::printf("supportsEyeGazeInteraction: %u\n", eye.supportsEyeGazeInteraction);
    if (!eye.supportsEyeGazeInteraction) throw std::runtime_error("HMD does not expose eye gaze");

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.systemId = system;
    XR_CHECK(xrCreateSession(instance, &sessionInfo, &session));
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gaze_probe");
    std::strcpy(setInfo.localizedActionSetName, "Gaze Probe");
    XR_CHECK(xrCreateActionSet(instance, &setInfo, &set));
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(actionInfo.actionName, "gaze");
    std::strcpy(actionInfo.localizedActionName, "Gaze");
    XrAction action;
    XR_CHECK(xrCreateAction(set, &actionInfo, &action));
    XrPath profile, path;
    XR_CHECK(xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile));
    XR_CHECK(xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &path));
    XrActionSuggestedBinding binding{action, path};
    XrInteractionProfileSuggestedBinding bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    bindings.interactionProfile = profile;
    bindings.countSuggestedBindings = 1;
    bindings.suggestedBindings = &binding;
    XR_CHECK(xrSuggestInteractionProfileBindings(instance, &bindings));
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &set;
    XR_CHECK(xrAttachSessionActionSets(session, &attach));
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = action;
    spaceInfo.poseInActionSpace.orientation.w = 1;
    XR_CHECK(xrCreateActionSpace(session, &spaceInfo, &gaze));
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    reference.poseInReferenceSpace.orientation.w = 1;
    XR_CHECK(xrCreateReferenceSpace(session, &reference, &view));
    PFN_xrConvertTimespecTimeToTimeKHR convert = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&convert)));

    bool running = false, exiting = false;
    unsigned samples = 0, valid = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto nextLog = std::chrono::steady_clock::now();
    while (!exiting && std::chrono::steady_clock::now() < deadline) {
      XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
      XrResult result;
      while ((result = xrPollEvent(instance, &event)) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) exiting = true;
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
          const auto state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
          std::printf("session state: %d\n", state);
          if (state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XR_CHECK(xrBeginSession(session, &begin));
            running = true;
          } else if (state == XR_SESSION_STATE_STOPPING) {
            XR_CHECK(xrEndSession(session));
            running = false;
          } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
            exiting = true;
          }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
      }
      check(result, "xrPollEvent");
      if (running && !exiting) {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) throw std::runtime_error("clock_gettime");
        XrTime time;
        XR_CHECK(convert(instance, &now, &time));
        XrActiveActionSet active{set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        XrResult syncResult = xrSyncActions(session, &sync);
        check(syncResult, "xrSyncActions");
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        XR_CHECK(xrGetActionStatePose(session, &get, &pose));
        XrEyeGazeSampleTimeEXT sample{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &sample};
        XR_CHECK(xrLocateSpace(gaze, view, time, &location));
        constexpr XrSpaceLocationFlags flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
          XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        bool ok = syncResult == XR_SUCCESS && pose.isActive && (location.locationFlags & flags) == flags;
        ++samples;
        valid += ok;
        if (std::chrono::steady_clock::now() >= nextLog) {
          const auto q = location.pose.orientation;
          // Rotate (0,0,-1) by the gaze quaternion, relative to the head.
          std::printf("sync=%d active=%u flags=0x%llx valid=%d direction=(%.4f,%.4f,%.4f) sample_age_ms=",
            syncResult, pose.isActive, static_cast<unsigned long long>(location.locationFlags), ok,
            -2*(q.x*q.z+q.w*q.y), -2*(q.y*q.z-q.w*q.x), -(1-2*(q.x*q.x+q.y*q.y)));
          if (sample.time) std::printf("%.3f\n", double(time-sample.time)/1e6);
          else std::puts("unknown");
          nextLog = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    std::printf("samples=%u valid=%u\n", samples, valid);
    status = valid ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
  }
  if (gaze) xrDestroySpace(gaze);
  if (view) xrDestroySpace(view);
  if (session) xrDestroySession(session);
  if (set) xrDestroyActionSet(set);
  if (instance) xrDestroyInstance(instance);
  return status;
}
