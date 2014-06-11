// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GEOLOCATION_CHROME_GEOLOCATION_PERMISSION_CONTEXT_H_
#define CHROME_BROWSER_GEOLOCATION_CHROME_GEOLOCATION_PERMISSION_CONTEXT_H_

#include <map>
#include <string>

#include "base/containers/scoped_ptr_hash_map.h"
#include "base/memory/scoped_ptr.h"
#include "chrome/browser/content_settings/permission_queue_controller.h"
#include "chrome/browser/geolocation/chrome_geolocation_permission_context_extensions.h"
#include "content/public/browser/geolocation_permission_context.h"

namespace content {
class WebContents;
}

class GeolocationPermissionRequest;
class PermissionRequestID;
class Profile;

// Chrome specific implementation of GeolocationPermissionContext; manages
// Geolocation permissions flow, and delegates UI handling via
// PermissionQueueController.
class ChromeGeolocationPermissionContext
    : public content::GeolocationPermissionContext {
 public:
  explicit ChromeGeolocationPermissionContext(Profile* profile);

  // GeolocationPermissionContext:
  virtual void RequestGeolocationPermission(
      content::WebContents* web_contents,
      int bridge_id,
      const GURL& requesting_frame,
      bool user_gesture,
      base::Callback<void(bool)> callback) OVERRIDE;
  virtual void CancelGeolocationPermissionRequest(
      content::WebContents* web_contents,
      int bridge_id,
      const GURL& requesting_frame) OVERRIDE;

  // Called on the UI thread when the profile is about to be destroyed.
  void ShutdownOnUIThread();

  // Notifies whether or not the corresponding bridge is allowed to use
  // geolocation via
  // GeolocationPermissionContext::SetGeolocationPermissionResponse().
  // Called on the UI thread.
  void NotifyPermissionSet(const PermissionRequestID& id,
                           const GURL& requesting_frame,
                           base::Callback<void(bool)> callback,
                           bool allowed);

 protected:
  virtual ~ChromeGeolocationPermissionContext();

  Profile* profile() const { return profile_; }

  // Return an instance of the infobar queue controller, creating it
  // if necessary.
  PermissionQueueController* QueueController();

  // ChromeGeolocationPermissionContext implementation:
  // Decide whether the geolocation permission should be granted.
  // Calls PermissionDecided if permission can be decided non-interactively,
  // or NotifyPermissionSet if permission decided by presenting an
  // infobar to the user. Called on the UI thread.
  virtual void DecidePermission(content::WebContents* web_contents,
                                const PermissionRequestID& id,
                                const GURL& requesting_frame,
                                bool user_gesture,
                                const GURL& embedder,
                                const std::string& accept_button_label,
                                base::Callback<void(bool)> callback);

  // Called when permission is granted without interactively asking
  // the user. Can be overridden to introduce additional UI flow.
  // Should ultimately ensure that NotifyPermissionSet is called.
  // Called on the UI thread.
  virtual void PermissionDecided(const PermissionRequestID& id,
                                 const GURL& requesting_frame,
                                 const GURL& embedder,
                                 base::Callback<void(bool)> callback,
                                 bool allowed);

  // Create an PermissionQueueController. overriden in derived classes to
  // provide additional UI flow.  Called on the UI thread.
  virtual PermissionQueueController* CreateQueueController();

 private:
  friend class GeolocationPermissionRequest;

  // Removes any pending InfoBar request.
  void CancelPendingInfobarRequest(const PermissionRequestID& id);

  // Creates and show an info bar.
  void CreateInfoBarRequest(const PermissionRequestID& id,
                            const GURL& requesting_frame,
                            const GURL& embedder,
                            const std::string accept_button_label,
                            base::Callback<void(bool)> callback);

  // Notify the context that a particular request object is no longer needed.
  void RequestFinished(GeolocationPermissionRequest* request);

  // These must only be accessed from the UI thread.
  Profile* const profile_;
  bool shutting_down_;
  scoped_ptr<PermissionQueueController> permission_queue_controller_;
  ChromeGeolocationPermissionContextExtensions extensions_context_;

  base::ScopedPtrHashMap<std::string, GeolocationPermissionRequest>
      pending_requests_;

  DISALLOW_COPY_AND_ASSIGN(ChromeGeolocationPermissionContext);
};

#endif  // CHROME_BROWSER_GEOLOCATION_CHROME_GEOLOCATION_PERMISSION_CONTEXT_H_
