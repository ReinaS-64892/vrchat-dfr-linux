// SPDX-License-Identifier: MIT
// Link-time OpenXR test double for the actual bridge executable. No HMD needed.
#define XR_USE_TIMESPEC
#include <ctime>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int phase = 0, attempts = 0, syncs = 0;
XrTime lastTime = 0;
bool mode(const char* value) {
  const char* selected = std::getenv("GAZE_TEST_MODE");
  return selected && !std::strcmp(selected, value);
}
template<typename T> T handle(unsigned n) { return reinterpret_cast<T>(uintptr_t(n)); }
XrResult convert(XrInstance, const timespec* time, XrTime* out) {
  *out = time->tv_sec*1000000000LL+time->tv_nsec;
  lastTime = *out;
  return XR_SUCCESS;
}
}
#define API extern "C" XRAPI_ATTR XrResult XRAPI_CALL
API xrEnumerateInstanceExtensionProperties(const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* props) {
  const char* names[] = {XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
    XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
  *count = mode("missing-extension") ? 2 : 3;
  if (capacity) {
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    for (unsigned i=0; i<*count; ++i) std::strcpy(props[i].extensionName, names[i]);
  }
  return XR_SUCCESS;
}
API xrCreateInstance(const XrInstanceCreateInfo* info, XrInstance* out) {
  ++attempts;
  if (mode("retry") && attempts == 1) return XR_ERROR_RUNTIME_UNAVAILABLE;
  if (info->enabledExtensionCount != 3) return XR_ERROR_VALIDATION_FAILURE;
  *out = handle<XrInstance>(1); phase = 0; syncs = 0;
  return XR_SUCCESS;
}
API xrGetInstanceProperties(XrInstance, XrInstanceProperties* out) {
  std::strcpy(out->runtimeName, "Fake gaze runtime"); return XR_SUCCESS;
}
API xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* out) { *out = 1; return XR_SUCCESS; }
API xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* out) {
  static_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(out->next)->supportsEyeGazeInteraction = !mode("no-eye");
  return XR_SUCCESS;
}
API xrCreateSession(XrInstance, const XrSessionCreateInfo* info, XrSession* out) {
  if (info->next) return XR_ERROR_VALIDATION_FAILURE; // headless only
  *out = handle<XrSession>(2); return XR_SUCCESS;
}
API xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out) {
  *out = handle<XrActionSet>(3); return XR_SUCCESS;
}
API xrCreateAction(XrActionSet, const XrActionCreateInfo* info, XrAction* out) {
  if (info->actionType != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_VALIDATION_FAILURE;
  *out = handle<XrAction>(4); return XR_SUCCESS;
}
API xrStringToPath(XrInstance, const char* name, XrPath* out) {
  if (!std::strcmp(name, "/interaction_profiles/ext/eye_gaze_interaction")) *out = 1;
  else if (!std::strcmp(name, "/user/eyes_ext/input/gaze_ext/pose")) *out = 2;
  else return XR_ERROR_PATH_UNSUPPORTED;
  return XR_SUCCESS;
}
API xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
  return info->interactionProfile == 1 && info->countSuggestedBindings == 1 &&
    info->suggestedBindings[0].binding == 2 ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}
API xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) { return XR_SUCCESS; }
API xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* out) {
  *out = handle<XrSpace>(5); return XR_SUCCESS;
}
API xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* out) {
  if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW) return XR_ERROR_VALIDATION_FAILURE;
  *out = handle<XrSpace>(6); return XR_SUCCESS;
}
API xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* out) {
  if (std::strcmp(name, "xrConvertTimespecTimeToTimeKHR")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  *out = reinterpret_cast<PFN_xrVoidFunction>(convert); return XR_SUCCESS;
}
API xrPollEvent(XrInstance, XrEventDataBuffer* out) {
  if (phase >= 2) return XR_EVENT_UNAVAILABLE;
  auto* event = reinterpret_cast<XrEventDataSessionStateChanged*>(out);
  event->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
  event->session = handle<XrSession>(2);
  event->state = phase++ ? XR_SESSION_STATE_FOCUSED : XR_SESSION_STATE_READY;
  return XR_SUCCESS;
}
API xrBeginSession(XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; }
API xrEndSession(XrSession) { return XR_SUCCESS; }
API xrSyncActions(XrSession, const XrActionsSyncInfo*) {
  ++syncs;
  if (mode("disconnect") && attempts == 1 && syncs == 20) return XR_ERROR_SESSION_LOST;
  return mode("unfocused") ? XR_SESSION_NOT_FOCUSED : XR_SUCCESS;
}
API xrGetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* out) {
  out->isActive = !mode("inactive"); return XR_SUCCESS;
}
API xrLocateSpace(XrSpace, XrSpace, XrTime time, XrSpaceLocation* out) {
  if (time != lastTime || time <= 0) return XR_ERROR_TIME_INVALID;
  out->pose = {{0,0,0,1},{0,0,0}};
  out->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
  if (mode("untracked")) out->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  auto* sample = static_cast<XrEyeGazeSampleTimeEXT*>(out->next);
  sample->time = mode("unknown-time") ? 0 : time - (mode("stale") ? 200000000 : 1000000);
  return XR_SUCCESS;
}
API xrLocateViews(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                 uint32_t capacity, uint32_t* count, XrView* out) {
  if (info->displayTime != lastTime || capacity != 2) return XR_ERROR_VALIDATION_FAILURE;
  *count = mode("bad-count") ? 1 : 2;
  state->viewStateFlags = mode("bad-view") ? 0 : XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
  for (int i=0; i<2; ++i) {
    out[i].pose = {{0,0,0,1},{i ? .032f : -.032f,0,0}};
    out[i].fov = {-.785398163f,.785398163f,.785398163f,-.785398163f};
  }
  return XR_SUCCESS;
}
API xrResultToString(XrInstance, XrResult result, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
  std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "fake error %d", result); return XR_SUCCESS;
}
API xrDestroySpace(XrSpace) { return XR_SUCCESS; }
API xrDestroySession(XrSession) { return XR_SUCCESS; }
API xrDestroyActionSet(XrActionSet) { return XR_SUCCESS; }
API xrDestroyInstance(XrInstance) { return XR_SUCCESS; }
