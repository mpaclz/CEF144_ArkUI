#ifndef ARKUI_CEF_CEF_OHOS_REPARENT_API_H
#define ARKUI_CEF_CEF_OHOS_REPARENT_API_H

// Contract with the CEF native side (libcef.so, implemented by agent A in
// cef-144-blue). The HAP side links against this extern "C" symbol directly.
//
// Return codes:
//   0   success
//  -1   invalid arguments / browser not found
//  -2   state not allowed (browser not ready / closing / already migrating)
//  -3   timed out waiting for the CEF UI thread (3s) — OUTCOME UNKNOWN:
//       the migration task may still arrive late on the CEF UI thread and
//       succeed (the waiter marks the task abandoned; a task that has not
//       started yet is dropped). The caller must NOT treat -3 as proof that
//       the migration itself failed — only that its result is unknown.
//  -4   internal migration failure
//
// browser_window_handle: the browser's current host window handle
//   (struct Node* returned by browser GetHost()->GetWindowHandle(),
//   nodeOwner == "cef").
// new_parent_handle: the new parent container (struct Node*, container is an
//   ArkUI STACK node) created on the napi side for the new window.
// width/height: new host window size in px.
// out_new_window_handle: optional out parameter (may be NULL). On success
//   (return 0) the NEW host window handle (struct Node*) is written here —
//   the old handle passed in browser_window_handle has been freed by CEF and
//   MUST be replaced by this one. Never written on failure or timeout.
// Synchronous blocking call — must NOT be invoked on the CEF UI thread.
extern "C" int cef_ohos_reparent_browser_host(void* browser_window_handle,
                                              void* new_parent_handle,
                                              int width, int height,
                                              void** out_new_window_handle);

#endif  // ARKUI_CEF_CEF_OHOS_REPARENT_API_H
