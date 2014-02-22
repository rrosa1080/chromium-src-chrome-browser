// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/apps/native_app_window_views.h"

#include "apps/ui/views/app_window_frame_view.h"
#include "base/command_line.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/app_mode/app_mode_utils.h"
#include "chrome/browser/chrome_page_zoom.h"
#include "chrome/browser/favicon/favicon_tab_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/host_desktop.h"
#include "chrome/browser/ui/views/apps/shaped_app_window_targeter.h"
#include "chrome/browser/ui/views/extensions/extension_keybinding_registry_views.h"
#include "chrome/browser/ui/views/frame/taskbar_decorator.h"
#include "chrome/browser/web_applications/web_app.h"
#include "chrome/common/chrome_switches.h"
#include "extensions/common/extension.h"
#include "ui/base/hit_test.h"
#include "ui/base/models/simple_menu_model.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/widget/widget.h"
#include "ui/wm/public/easy_resize_window_targeter.h"

#if defined(OS_LINUX)
#include "chrome/browser/shell_integration_linux.h"
#endif

#if defined(USE_ASH)
#include "ash/ash_constants.h"
#include "ash/ash_switches.h"
#include "ash/screen_util.h"
#include "ash/shell.h"
#include "ash/wm/custom_frame_view_ash.h"
#include "ash/wm/immersive_fullscreen_controller.h"
#include "ash/wm/panels/panel_frame_view.h"
#include "ash/wm/window_state.h"
#include "ash/wm/window_state_delegate.h"
#include "ash/wm/window_state_observer.h"
#include "chrome/browser/ui/ash/ash_util.h"
#include "chrome/browser/ui/ash/multi_user/multi_user_context_menu.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/client/window_tree_client.h"
#include "ui/aura/window.h"
#include "ui/aura/window_observer.h"
#endif

#if defined(USE_AURA)
#include "ui/aura/window.h"
#endif

using apps::AppWindow;

namespace {

const int kMinPanelWidth = 100;
const int kMinPanelHeight = 100;
const int kDefaultPanelWidth = 200;
const int kDefaultPanelHeight = 300;
const int kResizeInsideBoundsSize = 5;
const int kResizeAreaCornerSize = 16;

struct AcceleratorMapping {
  ui::KeyboardCode keycode;
  int modifiers;
  int command_id;
};

const AcceleratorMapping kAppWindowAcceleratorMap[] = {
  { ui::VKEY_W, ui::EF_CONTROL_DOWN, IDC_CLOSE_WINDOW },
  { ui::VKEY_W, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN, IDC_CLOSE_WINDOW },
  { ui::VKEY_F4, ui::EF_ALT_DOWN, IDC_CLOSE_WINDOW },
};

// These accelerators will only be available in kiosk mode. These allow the
// user to manually zoom app windows. This is only necessary in kiosk mode
// (in normal mode, the user can zoom via the screen magnifier).
// TODO(xiyuan): Write a test for kiosk accelerators.
const AcceleratorMapping kAppWindowKioskAppModeAcceleratorMap[] = {
  { ui::VKEY_OEM_MINUS, ui::EF_CONTROL_DOWN, IDC_ZOOM_MINUS },
  { ui::VKEY_OEM_MINUS, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN,
    IDC_ZOOM_MINUS },
  { ui::VKEY_SUBTRACT, ui::EF_CONTROL_DOWN, IDC_ZOOM_MINUS },
  { ui::VKEY_OEM_PLUS, ui::EF_CONTROL_DOWN, IDC_ZOOM_PLUS },
  { ui::VKEY_OEM_PLUS, ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN, IDC_ZOOM_PLUS },
  { ui::VKEY_ADD, ui::EF_CONTROL_DOWN, IDC_ZOOM_PLUS },
  { ui::VKEY_0, ui::EF_CONTROL_DOWN, IDC_ZOOM_NORMAL },
  { ui::VKEY_NUMPAD0, ui::EF_CONTROL_DOWN, IDC_ZOOM_NORMAL },
};

void AddAcceleratorsFromMapping(const AcceleratorMapping mapping[],
                                size_t mapping_length,
                                std::map<ui::Accelerator, int>* accelerators) {
  for (size_t i = 0; i < mapping_length; ++i) {
    ui::Accelerator accelerator(mapping[i].keycode, mapping[i].modifiers);
    (*accelerators)[accelerator] = mapping[i].command_id;
  }
}

const std::map<ui::Accelerator, int>& GetAcceleratorTable() {
  typedef std::map<ui::Accelerator, int> AcceleratorMap;
  CR_DEFINE_STATIC_LOCAL(AcceleratorMap, accelerators, ());
  if (accelerators.empty()) {
    AddAcceleratorsFromMapping(
        kAppWindowAcceleratorMap,
        arraysize(kAppWindowAcceleratorMap),
        &accelerators);

    // Add accelerators for kiosk mode.
    if (chrome::IsRunningInForcedAppMode()) {
      AddAcceleratorsFromMapping(
          kAppWindowKioskAppModeAcceleratorMap,
          arraysize(kAppWindowKioskAppModeAcceleratorMap),
          &accelerators);
    }
  }
  return accelerators;
}

#if defined(USE_ASH)
// This class handles a user's fullscreen request (Shift+F4/F4).
class NativeAppWindowStateDelegate : public ash::wm::WindowStateDelegate,
                                     public ash::wm::WindowStateObserver,
                                     public aura::WindowObserver {
 public:
  NativeAppWindowStateDelegate(AppWindow* app_window,
                               apps::NativeAppWindow* native_app_window)
      : app_window_(app_window),
        window_state_(
            ash::wm::GetWindowState(native_app_window->GetNativeWindow())) {
    // Add a window state observer to exit fullscreen properly in case
    // fullscreen is exited without going through AppWindow::Restore(). This
    // is the case when exiting immersive fullscreen via the "Restore" window
    // control.
    // TODO(pkotwicz): This is a hack. Remove ASAP. http://crbug.com/319048
    window_state_->AddObserver(this);
    window_state_->window()->AddObserver(this);
  }
  virtual ~NativeAppWindowStateDelegate(){
    if (window_state_) {
      window_state_->RemoveObserver(this);
      window_state_->window()->RemoveObserver(this);
    }
  }

 private:
  // Overridden from ash::wm::WindowStateDelegate.
  virtual bool ToggleFullscreen(ash::wm::WindowState* window_state) OVERRIDE {
    // Windows which cannot be maximized should not be fullscreened.
    DCHECK(window_state->IsFullscreen() || window_state->CanMaximize());
    if (window_state->IsFullscreen())
      app_window_->Restore();
    else if (window_state->CanMaximize())
      app_window_->OSFullscreen();
    return true;
  }

  // Overridden from ash::wm::WindowStateObserver:
  virtual void OnPostWindowShowTypeChange(
      ash::wm::WindowState* window_state,
      ash::wm::WindowShowType old_type) OVERRIDE {
    if (!window_state->IsFullscreen() && !window_state->IsMinimized() &&
        app_window_->GetBaseWindow()->IsFullscreenOrPending()) {
      app_window_->Restore();
      // Usually OnNativeWindowChanged() is called when the window bounds are
      // changed as a result of a show type change. Because the change in show
      // type has already occurred, we need to call OnNativeWindowChanged()
      // explicitly.
      app_window_->OnNativeWindowChanged();
    }
  }

  // Overridden from aura::WindowObserver:
  virtual void OnWindowDestroying(aura::Window* window) OVERRIDE {
    window_state_->RemoveObserver(this);
    window_state_->window()->RemoveObserver(this);
    window_state_ = NULL;
  }

  // Not owned.
  AppWindow* app_window_;
  ash::wm::WindowState* window_state_;

  DISALLOW_COPY_AND_ASSIGN(NativeAppWindowStateDelegate);
};
#endif  // USE_ASH

}  // namespace

NativeAppWindowViews::NativeAppWindowViews()
    : is_fullscreen_(false),
      has_frame_color_(false),
      frame_color_(SK_ColorBLACK) {}

NativeAppWindowViews::~NativeAppWindowViews() {}

void NativeAppWindowViews::OnBeforeWidgetInit(
    views::Widget::InitParams* init_params,
    views::Widget* widget) {}

void NativeAppWindowViews::InitializeDefaultWindow(
    const AppWindow::CreateParams& create_params) {
  std::string app_name =
      web_app::GenerateApplicationNameFromExtensionId(
          app_window()->extension()->id());

  views::Widget::InitParams init_params(views::Widget::InitParams::TYPE_WINDOW);
  init_params.delegate = this;
  init_params.remove_standard_frame = !ShouldUseNativeFrame();
#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
  // On Linux, remove the standard frame. Instead, we will use CustomFrameView
  // to draw a native-like frame.
  // TODO(mgiuca): Remove this during fix for http://crbug.com/322256.
  init_params.remove_standard_frame = true;
#endif
  init_params.use_system_default_icon = true;
  // TODO(erg): Conceptually, these are toplevel windows, but we theoretically
  // could plumb context through to here in some cases.
  init_params.top_level = true;
  if (create_params.transparent_background)
    init_params.opacity = views::Widget::InitParams::TRANSLUCENT_WINDOW;
  init_params.keep_on_top = create_params.always_on_top;
  gfx::Rect window_bounds = create_params.bounds;
  bool position_specified =
      window_bounds.x() != INT_MIN && window_bounds.y() != INT_MIN;
  if (position_specified && !window_bounds.IsEmpty())
    init_params.bounds = window_bounds;

#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
  // Set up a custom WM_CLASS for app windows. This allows task switchers in
  // X11 environments to distinguish them from main browser windows.
  init_params.wm_class_name = web_app::GetWMClassFromAppName(app_name);
  init_params.wm_class_class = ShellIntegrationLinux::GetProgramClassName();
  const char kX11WindowRoleApp[] = "app";
  init_params.wm_role_name = std::string(kX11WindowRoleApp);
#endif

  OnBeforeWidgetInit(&init_params, window());
  window()->Init(init_params);

  gfx::Rect adjusted_bounds = window_bounds;
  adjusted_bounds.Inset(-GetFrameInsets());
  // Center window if no position was specified.
  if (!position_specified)
    window()->CenterWindow(adjusted_bounds.size());
  else if (!adjusted_bounds.IsEmpty() && adjusted_bounds != window_bounds)
    window()->SetBounds(adjusted_bounds);

  // Register accelarators supported by app windows.
  // TODO(jeremya/stevenjb): should these be registered for panels too?
  views::FocusManager* focus_manager = GetFocusManager();
  const std::map<ui::Accelerator, int>& accelerator_table =
      GetAcceleratorTable();
  const bool is_kiosk_app_mode = chrome::IsRunningInForcedAppMode();

  // Ensures that kiosk mode accelerators are enabled when in kiosk mode (to be
  // future proof). This is needed because GetAcceleratorTable() uses a static
  // to store data and only checks kiosk mode once. If a platform app is
  // launched before kiosk mode starts, the kiosk accelerators will not be
  // registered. This DCHECK catches the case.
  DCHECK(!is_kiosk_app_mode ||
         accelerator_table.size() ==
             arraysize(kAppWindowAcceleratorMap) +
                 arraysize(kAppWindowKioskAppModeAcceleratorMap));

  for (std::map<ui::Accelerator, int>::const_iterator iter =
           accelerator_table.begin();
       iter != accelerator_table.end(); ++iter) {
    if (is_kiosk_app_mode && !chrome::IsCommandAllowedInAppMode(iter->second))
      continue;

    focus_manager->RegisterAccelerator(
        iter->first, ui::AcceleratorManager::kNormalPriority, this);
  }
}

void NativeAppWindowViews::InitializePanelWindow(
    const AppWindow::CreateParams& create_params) {
  views::Widget::InitParams params(views::Widget::InitParams::TYPE_PANEL);
  params.delegate = this;

  preferred_size_ = gfx::Size(create_params.bounds.width(),
                              create_params.bounds.height());
  if (preferred_size_.width() == 0)
    preferred_size_.set_width(kDefaultPanelWidth);
  else if (preferred_size_.width() < kMinPanelWidth)
    preferred_size_.set_width(kMinPanelWidth);

  if (preferred_size_.height() == 0)
    preferred_size_.set_height(kDefaultPanelHeight);
  else if (preferred_size_.height() < kMinPanelHeight)
    preferred_size_.set_height(kMinPanelHeight);
#if defined(USE_ASH)
  if (ash::Shell::HasInstance()) {
    // Open a new panel on the target root.
    aura::Window* target = ash::Shell::GetTargetRootWindow();
    params.bounds = ash::ScreenUtil::ConvertRectToScreen(
        target, gfx::Rect(preferred_size_));
  } else {
    params.bounds = gfx::Rect(preferred_size_);
  }
#else
  params.bounds = gfx::Rect(preferred_size_);
#endif
  // TODO(erg): Conceptually, these are toplevel windows, but we theoretically
  // could plumb context through to here in some cases.
  params.top_level = true;
  window()->Init(params);
  window()->set_focus_on_creation(create_params.focused);

#if defined(USE_ASH)
  if (create_params.state == ui::SHOW_STATE_DETACHED) {
    gfx::Rect window_bounds(create_params.bounds.x(),
                            create_params.bounds.y(),
                            preferred_size_.width(),
                            preferred_size_.height());
    aura::Window* native_window = GetNativeWindow();
    ash::wm::GetWindowState(native_window)->set_panel_attached(false);
    aura::client::ParentWindowWithContext(native_window,
                                          native_window->GetRootWindow(),
                                          native_window->GetBoundsInScreen());
    window()->SetBounds(window_bounds);
  }
#else
  // TODO(stevenjb): NativeAppWindow panels need to be implemented for other
  // platforms.
#endif
}

bool NativeAppWindowViews::ShouldUseNativeFrame() const {
  return !IsFrameless() & !has_frame_color_;
}

void NativeAppWindowViews::InstallEasyResizeTargeterOnContainer() const {
  aura::Window* root_window = window()->GetNativeWindow()->GetRootWindow();
  gfx::Insets inset(kResizeInsideBoundsSize, kResizeInsideBoundsSize,
                    kResizeInsideBoundsSize, kResizeInsideBoundsSize);
  root_window->SetEventTargeter(scoped_ptr<ui::EventTargeter>(
      new wm::EasyResizeWindowTargeter(root_window, inset, inset)));
}

apps::AppWindowFrameView*
NativeAppWindowViews::CreateAppWindowFrameView() {
  // By default the user can resize the window from slightly inside the bounds.
  int resize_inside_bounds_size = kResizeInsideBoundsSize;
  int resize_outside_bounds_size = 0;
  int resize_outside_scale_for_touch = 1;
  int resize_area_corner_size = kResizeAreaCornerSize;
#if defined(USE_ASH)
  // For Aura windows on the Ash desktop the sizes are different and the user
  // can resize the window from slightly outside the bounds as well.
  if (chrome::IsNativeWindowInAsh(window()->GetNativeWindow())) {
    resize_inside_bounds_size = ash::kResizeInsideBoundsSize;
    resize_outside_bounds_size = ash::kResizeOutsideBoundsSize;
    resize_outside_scale_for_touch = ash::kResizeOutsideBoundsScaleForTouch;
    resize_area_corner_size = ash::kResizeAreaCornerSize;
  }
#endif
  apps::AppWindowFrameView* frame_view = new apps::AppWindowFrameView(this);
  frame_view->Init(window(),
                   frame_color_,
                   resize_inside_bounds_size,
                   resize_outside_bounds_size,
                   resize_outside_scale_for_touch,
                   resize_area_corner_size);
  return frame_view;
}

// ui::BaseWindow implementation.

ui::WindowShowState NativeAppWindowViews::GetRestoredState() const {
  if (IsMaximized())
    return ui::SHOW_STATE_MAXIMIZED;
  if (IsFullscreen()) {
#if defined(USE_ASH)
    if (immersive_fullscreen_controller_.get() &&
        immersive_fullscreen_controller_->IsEnabled()) {
      // Restore windows which were previously in immersive fullscreen to
      // maximized. Restoring the window to a different fullscreen type
      // makes for a bad experience.
      return ui::SHOW_STATE_MAXIMIZED;
    }
#endif
    return ui::SHOW_STATE_FULLSCREEN;
  }
#if defined(USE_ASH)
  // Use kRestoreShowStateKey in case a window is minimized/hidden.
  ui::WindowShowState restore_state =
      window()->GetNativeWindow()->GetProperty(
          aura::client::kRestoreShowStateKey);
  // Whitelist states to return so that invalid and transient states
  // are not saved and used to restore windows when they are recreated.
  switch (restore_state) {
    case ui::SHOW_STATE_NORMAL:
    case ui::SHOW_STATE_MAXIMIZED:
    case ui::SHOW_STATE_FULLSCREEN:
    case ui::SHOW_STATE_DETACHED:
      return restore_state;

    case ui::SHOW_STATE_DEFAULT:
    case ui::SHOW_STATE_MINIMIZED:
    case ui::SHOW_STATE_INACTIVE:
    case ui::SHOW_STATE_END:
      return ui::SHOW_STATE_NORMAL;
  }
#endif
  return ui::SHOW_STATE_NORMAL;
}

bool NativeAppWindowViews::IsAlwaysOnTop() const {
  if (app_window()->window_type_is_panel()) {
#if defined(USE_ASH)
    return ash::wm::GetWindowState(window()->GetNativeWindow())->
        panel_attached();
#else
    return true;
#endif
  } else {
    return window()->IsAlwaysOnTop();
  }
}

// views::ContextMenuController implementation.

void NativeAppWindowViews::ShowContextMenuForView(
    views::View* source,
    const gfx::Point& p,
    ui::MenuSourceType source_type) {
#if defined(USE_ASH) && defined(OS_CHROMEOS)
  scoped_ptr<ui::MenuModel> model =
      CreateMultiUserContextMenu(app_window()->GetNativeWindow());
  if (!model.get())
    return;

  // Only show context menu if point is in caption.
  gfx::Point point_in_view_coords(p);
  views::View::ConvertPointFromScreen(window()->non_client_view(),
                                      &point_in_view_coords);
  int hit_test = window()->non_client_view()->NonClientHitTest(
      point_in_view_coords);
  if (hit_test == HTCAPTION) {
    menu_runner_.reset(new views::MenuRunner(model.get()));
    if (menu_runner_->RunMenuAt(source->GetWidget(), NULL,
          gfx::Rect(p, gfx::Size(0,0)), views::MenuItemView::TOPLEFT,
          source_type,
          views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU) ==
        views::MenuRunner::MENU_DELETED)
      return;
  }
#endif
}

// views::WidgetDelegate implementation.

gfx::ImageSkia NativeAppWindowViews::GetWindowAppIcon() {
  gfx::Image app_icon = app_window()->app_icon();
  if (app_icon.IsEmpty())
    return GetWindowIcon();
  else
    return *app_icon.ToImageSkia();
}

gfx::ImageSkia NativeAppWindowViews::GetWindowIcon() {
  content::WebContents* web_contents = app_window()->web_contents();
  if (web_contents) {
    FaviconTabHelper* favicon_tab_helper =
        FaviconTabHelper::FromWebContents(web_contents);
    gfx::Image app_icon = favicon_tab_helper->GetFavicon();
    if (!app_icon.IsEmpty())
      return *app_icon.ToImageSkia();
  }
  return gfx::ImageSkia();
}

views::NonClientFrameView* NativeAppWindowViews::CreateNonClientFrameView(
    views::Widget* widget) {
#if defined(USE_ASH)
  if (chrome::IsNativeViewInAsh(widget->GetNativeView())) {
    // Set the delegate now because CustomFrameViewAsh sets the
    // WindowStateDelegate if one is not already set.
    ash::wm::GetWindowState(GetNativeWindow())->SetDelegate(
        scoped_ptr<ash::wm::WindowStateDelegate>(
            new NativeAppWindowStateDelegate(app_window(), this)).Pass());

    if (app_window()->window_type_is_panel()) {
      ash::PanelFrameView::FrameType frame_type = IsFrameless() ?
          ash::PanelFrameView::FRAME_NONE : ash::PanelFrameView::FRAME_ASH;
      views::NonClientFrameView* frame_view =
          new ash::PanelFrameView(widget, frame_type);
      frame_view->set_context_menu_controller(this);
      return frame_view;
    }

    if (!IsFrameless()) {
      ash::CustomFrameViewAsh* custom_frame_view =
          new ash::CustomFrameViewAsh(widget);
#if defined(OS_CHROMEOS)
      // Non-frameless app windows can be put into immersive fullscreen.
      // TODO(pkotwicz): Investigate if immersive fullscreen can be enabled for
      // Windows Ash.
      if (ash::switches::UseImmersiveFullscreenForAllWindows()) {
        immersive_fullscreen_controller_.reset(
            new ash::ImmersiveFullscreenController());
        custom_frame_view->InitImmersiveFullscreenControllerForView(
            immersive_fullscreen_controller_.get());
      }
#endif
      custom_frame_view->GetHeaderView()->set_context_menu_controller(this);
      return custom_frame_view;
    }
  }
#endif
  if (!ShouldUseNativeFrame())
    return CreateAppWindowFrameView();
  return views::WidgetDelegateView::CreateNonClientFrameView(widget);
}

bool NativeAppWindowViews::WidgetHasHitTestMask() const {
  return shape_ != NULL;
}

void NativeAppWindowViews::GetWidgetHitTestMask(gfx::Path* mask) const {
  shape_->getBoundaryPath(mask);
}

// views::View implementation.

gfx::Size NativeAppWindowViews::GetPreferredSize() {
  if (!preferred_size_.IsEmpty())
    return preferred_size_;
  return BaseNativeAppWindowViews::GetPreferredSize();
}

bool NativeAppWindowViews::AcceleratorPressed(
    const ui::Accelerator& accelerator) {
  const std::map<ui::Accelerator, int>& accelerator_table =
      GetAcceleratorTable();
  std::map<ui::Accelerator, int>::const_iterator iter =
      accelerator_table.find(accelerator);
  DCHECK(iter != accelerator_table.end());
  int command_id = iter->second;
  switch (command_id) {
    case IDC_CLOSE_WINDOW:
      Close();
      return true;
    case IDC_ZOOM_MINUS:
      chrome_page_zoom::Zoom(web_view()->GetWebContents(),
                             content::PAGE_ZOOM_OUT);
      return true;
    case IDC_ZOOM_NORMAL:
      chrome_page_zoom::Zoom(web_view()->GetWebContents(),
                             content::PAGE_ZOOM_RESET);
      return true;
    case IDC_ZOOM_PLUS:
      chrome_page_zoom::Zoom(web_view()->GetWebContents(),
                             content::PAGE_ZOOM_IN);
      return true;
    default:
      NOTREACHED() << "Unknown accelerator sent to app window.";
  }
  return BaseNativeAppWindowViews::AcceleratorPressed(accelerator);
}

// NativeAppWindow implementation.

void NativeAppWindowViews::SetFullscreen(int fullscreen_types) {
  // Fullscreen not supported by panels.
  if (app_window()->window_type_is_panel())
    return;
  is_fullscreen_ = (fullscreen_types != AppWindow::FULLSCREEN_TYPE_NONE);
  window()->SetFullscreen(is_fullscreen_);

#if defined(USE_ASH)
  if (immersive_fullscreen_controller_.get()) {
    // |immersive_fullscreen_controller_| should only be set if immersive
    // fullscreen is the fullscreen type used by the OS.
    immersive_fullscreen_controller_->SetEnabled(
        ash::ImmersiveFullscreenController::WINDOW_TYPE_PACKAGED_APP,
        (fullscreen_types & AppWindow::FULLSCREEN_TYPE_OS) != 0);
    // Autohide the shelf instead of hiding the shelf completely when only in
    // OS fullscreen.
    ash::wm::WindowState* window_state =
        ash::wm::GetWindowState(window()->GetNativeWindow());
    window_state->set_hide_shelf_when_fullscreen(fullscreen_types !=
                                                 AppWindow::FULLSCREEN_TYPE_OS);
    DCHECK(ash::Shell::HasInstance());
    ash::Shell::GetInstance()->UpdateShelfVisibility();
  }
#endif

  // TODO(jeremya) we need to call RenderViewHost::ExitFullscreen() if we
  // ever drop the window out of fullscreen in response to something that
  // wasn't the app calling webkitCancelFullScreen().
}

bool NativeAppWindowViews::IsFullscreenOrPending() const {
  return is_fullscreen_;
}

bool NativeAppWindowViews::IsDetached() const {
  if (!app_window()->window_type_is_panel())
    return false;
#if defined(USE_ASH)
  return !ash::wm::GetWindowState(window()->GetNativeWindow())
              ->panel_attached();
#else
  return false;
#endif
}

void NativeAppWindowViews::UpdateBadgeIcon() {
  const gfx::Image* icon = NULL;
  if (!app_window()->badge_icon().IsEmpty()) {
    icon = &app_window()->badge_icon();
    // chrome::DrawTaskbarDecoration can do interesting things with non-square
    // bitmaps.
    // TODO(benwells): Refactor chrome::DrawTaskbarDecoration to not be avatar
    // specific, and lift this restriction.
    if (icon->Width() != icon->Height()) {
      LOG(ERROR) << "Attempt to set a non-square badge; request ignored.";
      return;
    }
  }
  chrome::DrawTaskbarDecoration(GetNativeWindow(), icon);
}

void NativeAppWindowViews::UpdateShape(scoped_ptr<SkRegion> region) {
  bool had_shape = shape_;
  shape_ = region.Pass();

  aura::Window* native_window = window()->GetNativeWindow();
  if (shape_) {
    window()->SetShape(new SkRegion(*shape_));
    if (!had_shape) {
      native_window->SetEventTargeter(scoped_ptr<ui::EventTargeter>(
          new ShapedAppWindowTargeter(native_window, this)));
    }
  } else {
    window()->SetShape(NULL);
    if (had_shape)
      native_window->SetEventTargeter(scoped_ptr<ui::EventTargeter>());
  }
}

bool NativeAppWindowViews::HasFrameColor() const {
  return has_frame_color_;
}

SkColor NativeAppWindowViews::FrameColor() const { return frame_color_; }

// BaseNativeAppWindowViews implementation.

void NativeAppWindowViews::InitializeWindow(
    AppWindow* app_window,
    const AppWindow::CreateParams& create_params) {
  DCHECK(window());
  has_frame_color_ = create_params.has_frame_color;
  frame_color_ = create_params.frame_color;
  if (create_params.window_type == AppWindow::WINDOW_TYPE_PANEL ||
      create_params.window_type == AppWindow::WINDOW_TYPE_V1_PANEL) {
    InitializePanelWindow(create_params);
  } else {
    InitializeDefaultWindow(create_params);
  }
  extension_keybinding_registry_.reset(new ExtensionKeybindingRegistryViews(
      Profile::FromBrowserContext(app_window->browser_context()),
      window()->GetFocusManager(),
      extensions::ExtensionKeybindingRegistry::PLATFORM_APPS_ONLY,
      app_window));

#if defined(OS_WIN)
  if (ShouldUseNativeFrame() &&
      chrome::GetHostDesktopTypeForNativeWindow(window()->GetNativeWindow()) !=
      chrome::HOST_DESKTOP_TYPE_ASH) {
    InstallEasyResizeTargeterOnContainer();
  }
#endif
}
