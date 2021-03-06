// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/apps/ephemeral_app_launcher.h"

#include "base/command_line.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/extensions/extension_install_checker.h"
#include "chrome/browser/extensions/extension_install_prompt.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/extensions/application_launch.h"
#include "chrome/browser/ui/extensions/extension_enable_flow.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/management_policy.h"
#include "extensions/common/permissions/permissions_data.h"

using content::WebContents;
using extensions::Extension;
using extensions::ExtensionInstallChecker;
using extensions::ExtensionPrefs;
using extensions::ExtensionRegistry;
using extensions::ExtensionSystem;
using extensions::ManagementPolicy;
using extensions::WebstoreInstaller;
namespace webstore_install = extensions::webstore_install;

namespace {

const char kInvalidManifestError[] = "Invalid manifest";
const char kExtensionTypeError[] = "Cannot launch an extension";
const char kUserCancelledError[] = "Launch cancelled by the user";
const char kBlacklistedError[] = "App is blacklisted for malware";
const char kRequirementsError[] = "App has missing requirements";
const char kFeatureDisabledError[] = "Launching ephemeral apps is not enabled";
const char kMissingAppError[] = "App is not installed";
const char kAppDisabledError[] = "App is disabled";

Profile* ProfileForWebContents(content::WebContents* contents) {
  if (!contents)
    return NULL;

  return Profile::FromBrowserContext(contents->GetBrowserContext());
}

gfx::NativeWindow NativeWindowForWebContents(content::WebContents* contents) {
  if (!contents)
    return NULL;

  return contents->GetTopLevelNativeWindow();
}

// Check whether an extension can be launched. The extension does not need to
// be currently installed.
bool CheckCommonLaunchCriteria(Profile* profile,
                               const extensions::Extension* extension,
                               webstore_install::Result* reason,
                               std::string* error) {
  // Only apps can be launched.
  if (!extension->is_app()) {
    *reason = webstore_install::LAUNCH_UNSUPPORTED_EXTENSION_TYPE;
    *error = kExtensionTypeError;
    return false;
  }

  // Do not launch apps blocked by management policies.
  ManagementPolicy* management_policy =
      ExtensionSystem::Get(profile)->management_policy();
  base::string16 policy_error;
  if (!management_policy->UserMayLoad(extension, &policy_error)) {
    *reason = webstore_install::BLOCKED_BY_POLICY;
    *error = base::UTF16ToUTF8(policy_error);
    return false;
  }

  return true;
}

}  // namespace

// static
bool EphemeralAppLauncher::IsFeatureEnabled() {
  return CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kEnableEphemeralApps);
}

// static
scoped_refptr<EphemeralAppLauncher> EphemeralAppLauncher::CreateForLauncher(
    const std::string& webstore_item_id,
    Profile* profile,
    gfx::NativeWindow parent_window,
    const LaunchCallback& callback) {
  scoped_refptr<EphemeralAppLauncher> installer =
      new EphemeralAppLauncher(webstore_item_id,
                               profile,
                               parent_window,
                               callback);
  installer->set_install_source(WebstoreInstaller::INSTALL_SOURCE_APP_LAUNCHER);
  return installer;
}

// static
scoped_refptr<EphemeralAppLauncher> EphemeralAppLauncher::CreateForWebContents(
    const std::string& webstore_item_id,
    content::WebContents* web_contents,
    const LaunchCallback& callback) {
  scoped_refptr<EphemeralAppLauncher> installer =
      new EphemeralAppLauncher(webstore_item_id, web_contents, callback);
  installer->set_install_source(WebstoreInstaller::INSTALL_SOURCE_OTHER);
  return installer;
}

void EphemeralAppLauncher::Start() {
  if (!IsFeatureEnabled()) {
    InvokeCallback(webstore_install::LAUNCH_FEATURE_DISABLED,
                   kFeatureDisabledError);
    return;
  }

  // Check whether the app already exists in extension system before downloading
  // from the webstore.
  const Extension* extension =
      ExtensionRegistry::Get(profile())
          ->GetExtensionById(id(), ExtensionRegistry::EVERYTHING);
  if (extension) {
    webstore_install::Result result = webstore_install::UNKNOWN_ERROR;
    std::string error;
    if (!CanLaunchInstalledApp(extension, &result, &error)) {
      InvokeCallback(result, error);
      return;
    }

    if (extensions::util::IsAppLaunchableWithoutEnabling(extension->id(),
                                                         profile())) {
      LaunchApp(extension);
      InvokeCallback(webstore_install::SUCCESS, std::string());
      return;
    }

    EnableInstalledApp(extension);
    return;
  }

  // Install the app ephemerally and launch when complete.
  BeginInstall();
}

EphemeralAppLauncher::EphemeralAppLauncher(const std::string& webstore_item_id,
                                           Profile* profile,
                                           gfx::NativeWindow parent_window,
                                           const LaunchCallback& callback)
    : WebstoreStandaloneInstaller(webstore_item_id, profile, Callback()),
      launch_callback_(callback),
      parent_window_(parent_window),
      dummy_web_contents_(
          WebContents::Create(WebContents::CreateParams(profile))) {
}

EphemeralAppLauncher::EphemeralAppLauncher(const std::string& webstore_item_id,
                                           content::WebContents* web_contents,
                                           const LaunchCallback& callback)
    : WebstoreStandaloneInstaller(webstore_item_id,
                                  ProfileForWebContents(web_contents),
                                  Callback()),
      content::WebContentsObserver(web_contents),
      launch_callback_(callback),
      parent_window_(NativeWindowForWebContents(web_contents)) {
}

EphemeralAppLauncher::~EphemeralAppLauncher() {}

bool EphemeralAppLauncher::CanLaunchInstalledApp(
    const extensions::Extension* extension,
    webstore_install::Result* reason,
    std::string* error) {
  if (!CheckCommonLaunchCriteria(profile(), extension, reason, error))
    return false;

  // Do not launch blacklisted apps.
  if (ExtensionPrefs::Get(profile())->IsExtensionBlacklisted(extension->id())) {
    *reason = webstore_install::BLACKLISTED;
    *error = kBlacklistedError;
    return false;
  }

  // If the app has missing requirements, it cannot be launched.
  if (!extensions::util::IsAppLaunchable(extension->id(), profile())) {
    *reason = webstore_install::REQUIREMENT_VIOLATIONS;
    *error = kRequirementsError;
    return false;
  }

  return true;
}

void EphemeralAppLauncher::EnableInstalledApp(const Extension* extension) {
  extension_enable_flow_.reset(
      new ExtensionEnableFlow(profile(), extension->id(), this));
  if (web_contents())
    extension_enable_flow_->StartForWebContents(web_contents());
  else
    extension_enable_flow_->StartForNativeWindow(parent_window_);

  // Keep this object alive until the enable flow is complete. Either
  // ExtensionEnableFlowFinished() or ExtensionEnableFlowAborted() will be
  // called.
  AddRef();
}

void EphemeralAppLauncher::MaybeLaunchApp() {
  webstore_install::Result result = webstore_install::UNKNOWN_ERROR;
  std::string error;

  ExtensionRegistry* registry = ExtensionRegistry::Get(profile());
  const Extension* extension =
      registry->GetExtensionById(id(), ExtensionRegistry::EVERYTHING);
  if (extension) {
    // Although the installation was successful, the app may not be
    // launchable.
    if (registry->enabled_extensions().Contains(extension->id())) {
      result = webstore_install::SUCCESS;
      LaunchApp(extension);
    } else {
      error = kAppDisabledError;
      // Determine why the app cannot be launched.
      CanLaunchInstalledApp(extension, &result, &error);
    }
  } else {
    // The extension must be present in the registry if installed.
    NOTREACHED();
    error = kMissingAppError;
  }

  InvokeCallback(result, error);
}

void EphemeralAppLauncher::LaunchApp(const Extension* extension) const {
  DCHECK(extension && extension->is_app() &&
         ExtensionRegistry::Get(profile())
             ->GetExtensionById(extension->id(), ExtensionRegistry::ENABLED));

  AppLaunchParams params(profile(), extension, NEW_FOREGROUND_TAB);
  params.desktop_type =
      chrome::GetHostDesktopTypeForNativeWindow(parent_window_);
  OpenApplication(params);
}

void EphemeralAppLauncher::InvokeCallback(webstore_install::Result result,
                                          const std::string& error) {
  if (!launch_callback_.is_null()) {
    launch_callback_.Run(result, error);
    launch_callback_.Reset();
  }
}

void EphemeralAppLauncher::AbortLaunch(webstore_install::Result result,
                                       const std::string& error) {
  InvokeCallback(result, error);
  WebstoreStandaloneInstaller::CompleteInstall(webstore_install::ABORTED,
                                               std::string());
}

scoped_ptr<extensions::ExtensionInstallChecker>
EphemeralAppLauncher::CreateInstallChecker() {
  return make_scoped_ptr(new ExtensionInstallChecker(profile()));
}

void EphemeralAppLauncher::CheckEphemeralInstallPermitted() {
  scoped_refptr<const Extension> extension = GetLocalizedExtensionForDisplay();
  DCHECK(extension.get());  // Checked in OnManifestParsed().

  install_checker_ = CreateInstallChecker();
  DCHECK(install_checker_.get());

  install_checker_->set_extension(extension);
  install_checker_->Start(ExtensionInstallChecker::CHECK_BLACKLIST |
                              ExtensionInstallChecker::CHECK_REQUIREMENTS,
                          true,
                          base::Bind(&EphemeralAppLauncher::OnInstallChecked,
                                     base::Unretained(this)));
}

void EphemeralAppLauncher::OnInstallChecked(int check_failures) {
  if (!CheckRequestorAlive()) {
    AbortLaunch(webstore_install::UNKNOWN_ERROR, std::string());
    return;
  }

  if (install_checker_->blacklist_state() == extensions::BLACKLISTED_MALWARE) {
    AbortLaunch(webstore_install::BLACKLISTED, kBlacklistedError);
    return;
  }

  if (!install_checker_->requirement_errors().empty()) {
    AbortLaunch(webstore_install::REQUIREMENT_VIOLATIONS,
                install_checker_->requirement_errors().front());
    return;
  }

  // Proceed with the normal install flow.
  ProceedWithInstallPrompt();
}

bool EphemeralAppLauncher::CheckRequestorAlive() const {
  return dummy_web_contents_.get() != NULL || web_contents() != NULL;
}

const GURL& EphemeralAppLauncher::GetRequestorURL() const {
  return GURL::EmptyGURL();
}

bool EphemeralAppLauncher::ShouldShowPostInstallUI() const {
  return false;
}

bool EphemeralAppLauncher::ShouldShowAppInstalledBubble() const {
  return false;
}

WebContents* EphemeralAppLauncher::GetWebContents() const {
  return web_contents() ? web_contents() : dummy_web_contents_.get();
}

scoped_refptr<ExtensionInstallPrompt::Prompt>
EphemeralAppLauncher::CreateInstallPrompt() const {
  const Extension* extension = localized_extension_for_display();
  DCHECK(extension);  // Checked in OnManifestParsed().

  // Skip the prompt by returning null if the app does not need to display
  // permission warnings.
  extensions::PermissionMessages permissions =
      extension->permissions_data()->GetPermissionMessages();
  if (permissions.empty())
    return NULL;

  return make_scoped_refptr(new ExtensionInstallPrompt::Prompt(
      ExtensionInstallPrompt::LAUNCH_PROMPT));
}

bool EphemeralAppLauncher::CheckInlineInstallPermitted(
    const base::DictionaryValue& webstore_data,
    std::string* error) const {
  *error = "";
  return true;
}

bool EphemeralAppLauncher::CheckRequestorPermitted(
    const base::DictionaryValue& webstore_data,
    std::string* error) const {
  *error = "";
  return true;
}

void EphemeralAppLauncher::OnManifestParsed() {
  const Extension* extension = GetLocalizedExtensionForDisplay();
  if (!extension) {
    AbortLaunch(webstore_install::INVALID_MANIFEST, kInvalidManifestError);
    return;
  }

  webstore_install::Result result = webstore_install::UNKNOWN_ERROR;
  std::string error;
  if (!CheckCommonLaunchCriteria(profile(), extension, &result, &error)) {
    AbortLaunch(result, error);
    return;
  }

  CheckEphemeralInstallPermitted();
}

scoped_ptr<ExtensionInstallPrompt> EphemeralAppLauncher::CreateInstallUI() {
  if (web_contents())
    return make_scoped_ptr(new ExtensionInstallPrompt(web_contents()));

  return make_scoped_ptr(
      new ExtensionInstallPrompt(profile(), parent_window_, NULL));
}

scoped_ptr<WebstoreInstaller::Approval>
EphemeralAppLauncher::CreateApproval() const {
  scoped_ptr<WebstoreInstaller::Approval> approval =
      WebstoreStandaloneInstaller::CreateApproval();
  approval->is_ephemeral = true;
  return approval.Pass();
}

void EphemeralAppLauncher::CompleteInstall(webstore_install::Result result,
                                           const std::string& error) {
  if (result == webstore_install::SUCCESS)
    MaybeLaunchApp();
  else if (!launch_callback_.is_null())
    InvokeCallback(result, error);

  WebstoreStandaloneInstaller::CompleteInstall(result, error);
}

void EphemeralAppLauncher::WebContentsDestroyed() {
  launch_callback_.Reset();
  AbortInstall();
}

void EphemeralAppLauncher::ExtensionEnableFlowFinished() {
  MaybeLaunchApp();
  Release();  // Matches the AddRef in EnableInstalledApp().
}

void EphemeralAppLauncher::ExtensionEnableFlowAborted(bool user_initiated) {
  InvokeCallback(webstore_install::USER_CANCELLED, kUserCancelledError);
  Release();  // Matches the AddRef in EnableInstalledApp().
}
