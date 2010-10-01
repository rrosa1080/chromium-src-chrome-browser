// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/extension_install_ui.h"

#include <map>

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/command_line.h"
#include "base/file_util.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/browser_list.h"
#include "chrome/browser/browser_window.h"
#include "chrome/browser/extensions/theme_installed_infobar_delegate.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/browser/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/extensions/extension.h"
#include "chrome/common/extensions/extension_action.h"
#include "chrome/common/extensions/extension_icon_set.h"
#include "chrome/common/extensions/extension_resource.h"
#include "chrome/common/extensions/url_pattern.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/url_constants.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"

#if defined(OS_MACOSX)
#include "chrome/browser/cocoa/extension_installed_bubble_bridge.h"
#endif

#if defined(TOOLKIT_VIEWS)
#include "chrome/browser/views/app_launcher.h"
#include "chrome/browser/views/extensions/extension_installed_bubble.h"
#endif

#if defined(TOOLKIT_GTK)
#include "chrome/browser/extensions/gtk_theme_installed_infobar_delegate.h"
#include "chrome/browser/gtk/extension_installed_bubble_gtk.h"
#include "chrome/browser/gtk/gtk_theme_provider.h"
#endif

// static
const int ExtensionInstallUI::kTitleIds[NUM_PROMPT_TYPES] = {
  IDS_EXTENSION_INSTALL_PROMPT_TITLE,
  IDS_EXTENSION_UNINSTALL_PROMPT_TITLE
};
// static
const int ExtensionInstallUI::kHeadingIds[NUM_PROMPT_TYPES] = {
  IDS_EXTENSION_INSTALL_PROMPT_HEADING,
  IDS_EXTENSION_UNINSTALL_PROMPT_HEADING
};
// static
const int ExtensionInstallUI::kButtonIds[NUM_PROMPT_TYPES] = {
  IDS_EXTENSION_PROMPT_INSTALL_BUTTON,
  IDS_EXTENSION_PROMPT_UNINSTALL_BUTTON
};

namespace {

// Size of extension icon in top left of dialog.
const int kIconSize = 69;

}  // namespace

ExtensionInstallUI::ExtensionInstallUI(Profile* profile)
    : profile_(profile),
      ui_loop_(MessageLoop::current()),
      previous_use_system_theme_(false),
      extension_(NULL),
      delegate_(NULL),
      prompt_type_(NUM_PROMPT_TYPES),
      ALLOW_THIS_IN_INITIALIZER_LIST(tracker_(this)) {}

ExtensionInstallUI::~ExtensionInstallUI() {
}

void ExtensionInstallUI::ConfirmInstall(Delegate* delegate,
                                        Extension* extension) {
  DCHECK(ui_loop_ == MessageLoop::current());
  extension_ = extension;
  delegate_ = delegate;

  // We special-case themes to not show any confirm UI. Instead they are
  // immediately installed, and then we show an infobar (see OnInstallSuccess)
  // to allow the user to revert if they don't like it.
  if (extension->is_theme()) {
    // Remember the current theme in case the user pressed undo.
    Extension* previous_theme = profile_->GetTheme();
    if (previous_theme)
      previous_theme_id_ = previous_theme->id();

#if defined(TOOLKIT_GTK)
    // On Linux, we also need to take the user's system settings into account
    // to undo theme installation.
    previous_use_system_theme_ =
        GtkThemeProvider::GetFrom(profile_)->UseGtkTheme();
#else
    DCHECK(!previous_use_system_theme_);
#endif

    delegate->InstallUIProceed();
    return;
  }

  ShowConfirmation(INSTALL_PROMPT);
}

void ExtensionInstallUI::ConfirmUninstall(Delegate* delegate,
                                          Extension* extension) {
  DCHECK(ui_loop_ == MessageLoop::current());
  extension_ = extension;
  delegate_ = delegate;

  ShowConfirmation(UNINSTALL_PROMPT);
}

void ExtensionInstallUI::OnInstallSuccess(Extension* extension) {
  // GetLastActiveWithProfile will fail on the build bots. This needs to be
  // implemented differently if any test is created which depends on
  // ExtensionInstalledBubble showing.
  Browser* browser = BrowserList::GetLastActiveWithProfile(profile_);
  if (!browser) {
    NOTREACHED() << "Could not find an active browser to show extension install"
                 << " success message in.";
    return;
  }

  // For themes, we show an infobar with a button that allows undoing.
  if (extension->is_theme()) {
    ShowThemeInfoBar(browser, previous_theme_id_, previous_use_system_theme_,
                     extension, profile_);
    return;
  }

  // For apps, we open the new tab page and show the new app there. If the
  // current browser doesn't have a tabstrip, we show an infobar instead.
  if (extension->GetFullLaunchURL().is_valid()) {
    if (browser->SupportsWindowFeature(Browser::FEATURE_TABSTRIP)) {
      std::string hash_params = "app-id=";
      hash_params += extension->id();
      std::string url(chrome::kChromeUINewTabURL);
      url += "/#";
      url += hash_params;
      browser->AddTabWithURL(GURL(url), GURL(), PageTransition::TYPED, -1,
                             TabStripModel::ADD_SELECTED, NULL, std::string(),
                             NULL);
    } else {
      ShowGenericExtensionInstalledInfoBar(browser, extension);
    }

    return;
  }

  // For extensions, we try to show a bubble that points to the newly installed
  // extension. But if there is no obvious place to point at, we show an infobar
  // instead.
  if (!browser->SupportsWindowFeature(Browser::FEATURE_TOOLBAR)) {
    ShowGenericExtensionInstalledInfoBar(browser, extension);
    return;
  }

#if defined(TOOLKIT_VIEWS)
  ExtensionInstalledBubble::Show(extension, browser, icon_);
#elif defined(OS_MACOSX)
  DCHECK(browser);
  // Note that browser actions don't appear in incognito mode initially,
  // so fall back to the generic case.
  // TODO(aa): Now that the mac always has a wrench menu, maybe we should do
  // what the other platforms do. Or maybe on the other platforms, we should
  // show an infobar like mac does, which in some ways is simpler.
  if ((extension->browser_action() && !browser->profile()->IsOffTheRecord()) ||
      (extension->page_action() &&
      !extension->page_action()->default_icon_path().empty())) {
    ExtensionInstalledBubbleCocoa::ShowExtensionInstalledBubble(
        browser->window()->GetNativeHandle(),
        extension, browser, icon_);
  }  else {
    // If the extension is of type GENERIC, meaning it doesn't have a UI
    // surface to display for this window, launch infobar instead of popup
    // bubble, because we have no guaranteed wrench menu button to point to.
    ShowGenericExtensionInstalledInfoBar(browser, extension);
  }
#elif defined(TOOLKIT_GTK)
  ExtensionInstalledBubbleGtk::Show(extension, browser, icon_);
#endif  // TOOLKIT_VIEWS
}

void ExtensionInstallUI::OnInstallFailure(const std::string& error) {
  DCHECK(ui_loop_ == MessageLoop::current());

  Browser* browser = BrowserList::GetLastActiveWithProfile(profile_);
  platform_util::SimpleErrorBox(
      browser ? browser->window()->GetNativeHandle() : NULL,
      l10n_util::GetStringUTF16(IDS_EXTENSION_INSTALL_FAILURE_TITLE),
      UTF8ToUTF16(error));
}

void ExtensionInstallUI::OnImageLoaded(
    SkBitmap* image, ExtensionResource resource, int index) {
  if (image)
    icon_ = *image;
  else
    icon_ = SkBitmap();
  if (icon_.empty()) {
    if (extension_->is_app()) {
      icon_ = *ResourceBundle::GetSharedInstance().GetBitmapNamed(
          IDR_APP_DEFAULT_ICON);
    } else {
      icon_ = *ResourceBundle::GetSharedInstance().GetBitmapNamed(
          IDR_EXTENSION_DEFAULT_ICON);
    }
  }

  switch (prompt_type_) {
    case INSTALL_PROMPT: {
      // TODO(jcivelli): http://crbug.com/44771 We should not show an install
      //                 dialog when installing an app from the gallery.
      NotificationService* service = NotificationService::current();
      service->Notify(NotificationType::EXTENSION_WILL_SHOW_CONFIRM_DIALOG,
          Source<ExtensionInstallUI>(this),
          NotificationService::NoDetails());

      std::vector<string16> warnings = extension_->GetPermissionMessages();
      ShowExtensionInstallUIPrompt2Impl(profile_, delegate_, extension_, &icon_,
                                        warnings);
      break;
    }
    case UNINSTALL_PROMPT: {
      ShowExtensionInstallUIPromptImpl(profile_, delegate_, extension_, &icon_,
                                       UNINSTALL_PROMPT);
      break;
    }
    default:
      NOTREACHED() << "Unknown message";
      break;
  }
}

void ExtensionInstallUI::ShowThemeInfoBar(
    Browser* browser, const std::string& previous_theme_id,
    bool previous_use_system_theme, Extension* new_theme, Profile* profile) {
  if (!new_theme->is_theme())
    return;

  TabContents* tab_contents = browser->GetSelectedTabContents();
  if (!tab_contents)
    return;

  // First find any previous theme preview infobars.
  InfoBarDelegate* old_delegate = NULL;
  for (int i = 0; i < tab_contents->infobar_delegate_count(); ++i) {
    InfoBarDelegate* delegate = tab_contents->GetInfoBarDelegateAt(i);
    ThemeInstalledInfoBarDelegate* theme_infobar =
        delegate->AsThemePreviewInfobarDelegate();
    if (theme_infobar) {
      // If the user installed the same theme twice, ignore the second install
      // and keep the first install info bar, so that they can easily undo to
      // get back the previous theme.
      if (theme_infobar->MatchesTheme(new_theme))
        return;
      old_delegate = delegate;
      break;
    }
  }

  // Then either replace that old one or add a new one.
  InfoBarDelegate* new_delegate =
      GetNewThemeInstalledInfoBarDelegate(
          tab_contents, new_theme,
          previous_theme_id, previous_use_system_theme);

  if (old_delegate)
    tab_contents->ReplaceInfoBar(old_delegate, new_delegate);
  else
    tab_contents->AddInfoBar(new_delegate);
}

void ExtensionInstallUI::ShowConfirmation(PromptType prompt_type) {
  // Load the image asynchronously. For the response, check OnImageLoaded.
  prompt_type_ = prompt_type;
  ExtensionResource image =
      extension_->GetIconResource(Extension::EXTENSION_ICON_LARGE,
                                  ExtensionIconSet::MATCH_EXACTLY);
  tracker_.LoadImage(extension_, image,
                     gfx::Size(kIconSize, kIconSize),
                     ImageLoadingTracker::DONT_CACHE);
}

void ExtensionInstallUI::ShowGenericExtensionInstalledInfoBar(
    Browser* browser, Extension* new_extension) {
  TabContents* tab_contents = browser->GetSelectedTabContents();
  if (!tab_contents)
    return;

  string16 msg =
      l10n_util::GetStringFUTF16(IDS_EXTENSION_INSTALLED_HEADING,
                                 UTF8ToUTF16(new_extension->name()));

  if (!new_extension->is_app()) {
    msg += UTF8ToUTF16(" ") +
#if defined(OS_MACOSX)
      l10n_util::GetStringUTF16(IDS_EXTENSION_INSTALLED_MANAGE_INFO_MAC);
#else
      l10n_util::GetStringUTF16(IDS_EXTENSION_INSTALLED_MANAGE_INFO);
#endif
  }

  // TODO(aa): We should be using the smaller icons from the extension if they
  // are available.
  const int kInfobarIconSize = 24;
  SkBitmap infobar_icon = skia::ImageOperations::Resize(
      icon_,
      skia::ImageOperations::RESIZE_LANCZOS3,
      kInfobarIconSize,
      kInfobarIconSize);

  InfoBarDelegate* delegate = new SimpleAlertInfoBarDelegate(
      tab_contents, msg, &infobar_icon, true);
  tab_contents->AddInfoBar(delegate);
}

InfoBarDelegate* ExtensionInstallUI::GetNewThemeInstalledInfoBarDelegate(
    TabContents* tab_contents, Extension* new_theme,
    const std::string& previous_theme_id, bool previous_use_system_theme) {
#if defined(TOOLKIT_GTK)
  return new GtkThemeInstalledInfoBarDelegate(tab_contents, new_theme,
      previous_theme_id, previous_use_system_theme);
#else
  return new ThemeInstalledInfoBarDelegate(tab_contents, new_theme,
      previous_theme_id);
#endif
}
