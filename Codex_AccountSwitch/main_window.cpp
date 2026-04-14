#include "main_window.h"

#include "resource.h"
#include "webview_host.h"

namespace {
WebViewHost g_webviewHost;
constexpr wchar_t kSettingsRegistryPath[] = L"Software\\Codex_AccountSwitch";
constexpr int kBaseMinTrackWidth = 1024;
constexpr int kBaseMinTrackHeight = 700;
constexpr int kDefaultClientWidth = 1400;
constexpr int kDefaultClientHeight = 900;
constexpr UINT kDefaultDpi = 96;

bool g_windowPlacementPersistenceReady = false;

using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
using GetDpiForSystemFn = UINT(WINAPI *)();
using AdjustWindowRectExForDpiFn = BOOL(WINAPI *)(LPRECT, DWORD, BOOL, DWORD,
                                                  UINT);
using EnableNonClientDpiScalingFn = BOOL(WINAPI *)(HWND);

void *GetUser32Proc(const char *name) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<void *>(GetProcAddress(user32, name));
}

UINT GetSystemDpiSafe() {
  auto *getDpiForSystem =
      reinterpret_cast<GetDpiForSystemFn>(GetUser32Proc("GetDpiForSystem"));
  if (getDpiForSystem != nullptr) {
    const UINT dpi = getDpiForSystem();
    if (dpi != 0) {
      return dpi;
    }
  }

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return kDefaultDpi;
  }
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi > 0 ? static_cast<UINT>(dpi) : kDefaultDpi;
}

UINT GetWindowDpiSafe(HWND hwnd) {
  auto *getDpiForWindow =
      reinterpret_cast<GetDpiForWindowFn>(GetUser32Proc("GetDpiForWindow"));
  if (getDpiForWindow == nullptr) {
    return GetSystemDpiSafe();
  }
  const UINT dpi = getDpiForWindow(hwnd);
  return dpi == 0 ? GetSystemDpiSafe() : dpi;
}

void EnableNonClientDpiScalingSafe(HWND hwnd) {
  auto *enableNonClientDpiScaling = reinterpret_cast<EnableNonClientDpiScalingFn>(
      GetUser32Proc("EnableNonClientDpiScaling"));
  if (enableNonClientDpiScaling != nullptr) {
    enableNonClientDpiScaling(hwnd);
  }
}

bool AdjustWindowRectForDpiSafe(RECT *rect, DWORD style, DWORD exStyle,
                                UINT dpi) {
  auto *adjustWindowRectExForDpi =
      reinterpret_cast<AdjustWindowRectExForDpiFn>(
          GetUser32Proc("AdjustWindowRectExForDpi"));
  if (adjustWindowRectExForDpi != nullptr) {
    return adjustWindowRectExForDpi(rect, style, FALSE, exStyle, dpi) != FALSE;
  }
  return AdjustWindowRectEx(rect, style, FALSE, exStyle) != FALSE;
}

bool IsPlacementRectVisible(const RECT &rect) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return false;
  }
  return MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr;
}

void NormalizeWindowPlacementForSave(HWND hwnd, WINDOWPLACEMENT *wp) {
  if (wp == nullptr) {
    return;
  }

  if (wp->showCmd == SW_SHOWMINIMIZED || wp->showCmd == SW_MINIMIZE ||
      wp->showCmd == SW_SHOWMINNOACTIVE) {
    const bool restoreToMax =
        (wp->flags & WPF_RESTORETOMAXIMIZED) != 0 || IsZoomed(hwnd);
    wp->showCmd = restoreToMax ? SW_MAXIMIZE : SW_SHOWNORMAL;
  }

  if (wp->showCmd != SW_MAXIMIZE) {
    wp->showCmd = SW_SHOWNORMAL;
  }

  wp->flags = 0;
  wp->ptMinPosition = POINT{-1, -1};
}

bool LoadWindowPlacementFromRegistry(WINDOWPLACEMENT *outPlacement) {
  if (outPlacement == nullptr) {
    return false;
  }

  HKEY hkey = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, KEY_READ,
                    &hkey) != ERROR_SUCCESS) {
    return false;
  }

  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  DWORD type = 0;
  DWORD size = sizeof(wp);
  const LONG queryResult =
      RegQueryValueExW(hkey, L"WindowPlacement", nullptr, &type,
                       reinterpret_cast<LPBYTE>(&wp), &size);
  RegCloseKey(hkey);

  if (queryResult != ERROR_SUCCESS || type != REG_BINARY ||
      size != sizeof(wp) || wp.length != sizeof(wp)) {
    return false;
  }

  if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE ||
      wp.showCmd == SW_SHOWMINNOACTIVE) {
    const bool restoreToMax = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
    wp.showCmd = restoreToMax ? SW_MAXIMIZE : SW_SHOWNORMAL;
  }

  if (wp.showCmd != SW_MAXIMIZE) {
    wp.showCmd = SW_SHOWNORMAL;
  }

  wp.flags = 0;
  if (!IsPlacementRectVisible(wp.rcNormalPosition)) {
    return false;
  }

  *outPlacement = wp;
  return true;
}

void SaveWindowPlacementToRegistry(HWND hwnd) {
  if (!g_windowPlacementPersistenceReady) {
    return;
  }

  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  if (GetWindowPlacement(hwnd, &wp)) {
    NormalizeWindowPlacementForSave(hwnd, &wp);

    HKEY hkey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, nullptr, 0,
                       KEY_WRITE, nullptr, &hkey, nullptr) == ERROR_SUCCESS) {
      RegSetValueExW(hkey, L"WindowPlacement", 0, REG_BINARY,
                     reinterpret_cast<const BYTE *>(&wp), sizeof(wp));
      RegCloseKey(hkey);
    }
  }
}

}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  if (g_webviewHost.HandleWindowMessage(msg, wParam, lParam)) {
    return 0;
  }

  switch (msg) {
  case kActivateExistingInstanceMessage:
    if (IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_RESTORE);
    } else if (IsZoomed(hwnd)) {
      ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    } else {
      ShowWindow(hwnd, SW_SHOW);
    }
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    return 0;

  case WM_CREATE:
    EnableNonClientDpiScalingSafe(hwnd);
    g_webviewHost.Initialize(hwnd);
    return 0;

  case WM_DPICHANGED: {
    const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
    if (suggested != nullptr) {
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_webviewHost.Resize(hwnd);
    SaveWindowPlacementToRegistry(hwnd);
    return 0;
  }

  case WM_SIZE:
    g_webviewHost.Resize(hwnd);
    if (wParam != SIZE_MINIMIZED) {
      SaveWindowPlacementToRegistry(hwnd);
    }
    return 0;

  case WM_EXITSIZEMOVE:
    SaveWindowPlacementToRegistry(hwnd);
    return 0;

  case WM_CLOSE:
    if (GetPropW(hwnd, kExitWindowPropName) == nullptr) {
      if (g_webviewHost.ShouldCloseToTray()) {
        SaveWindowPlacementToRegistry(hwnd);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
    }
    RemovePropW(hwnd, kExitWindowPropName);
    break;

  case WM_GETMINMAXINFO: {
    auto *mm = reinterpret_cast<MINMAXINFO *>(lParam);
    if (mm != nullptr) {
      const UINT dpi = GetWindowDpiSafe(hwnd);
      mm->ptMinTrackSize.x =
          MulDiv(kBaseMinTrackWidth, static_cast<int>(dpi), kDefaultDpi);
      mm->ptMinTrackSize.y =
          MulDiv(kBaseMinTrackHeight, static_cast<int>(dpi), kDefaultDpi);
    }
    return 0;
  }

  case WM_DESTROY:
    SaveWindowPlacementToRegistry(hwnd);
    g_webviewHost.Cleanup();
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterMainWindowClass(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = MainWindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = kMainWindowClassName;
  wc.hIcon = static_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32,
                 LR_DEFAULTCOLOR | LR_SHARED));
  wc.hIconSm = static_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16,
                 LR_DEFAULTCOLOR | LR_SHARED));
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

  return RegisterClassExW(&wc) != 0;
}

HWND CreateMainWindow(HINSTANCE instance, int nCmdShow) {
  constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
  constexpr DWORD kWindowExStyle = 0;

  RECT initialRect{0, 0, kDefaultClientWidth, kDefaultClientHeight};
  const UINT initialDpi = GetSystemDpiSafe();
  AdjustWindowRectForDpiSafe(&initialRect, kWindowStyle, kWindowExStyle,
                             initialDpi);

  const int initialWidth = initialRect.right - initialRect.left;
  const int initialHeight = initialRect.bottom - initialRect.top;

  HWND hwnd = CreateWindowExW(kWindowExStyle, kMainWindowClassName,
                              kMainWindowTitle, kWindowStyle, CW_USEDEFAULT,
                              CW_USEDEFAULT, initialWidth, initialHeight,
                              nullptr, nullptr, instance, nullptr);

  if (hwnd == nullptr) {
    return nullptr;
  }

  WINDOWPLACEMENT savedPlacement{};
  const bool hasSavedPlacement = LoadWindowPlacementFromRegistry(&savedPlacement);
  if (hasSavedPlacement) {
    SetWindowPlacement(hwnd, &savedPlacement);
  }

  int showCmd = nCmdShow;
  if (hasSavedPlacement && savedPlacement.showCmd == SW_MAXIMIZE) {
    showCmd = SW_SHOWMAXIMIZED;
  } else if (showCmd == SW_SHOWMINIMIZED || showCmd == SW_MINIMIZE ||
             showCmd == SW_SHOWMINNOACTIVE || showCmd == SW_SHOWDEFAULT) {
    showCmd = SW_SHOWNORMAL;
  }

  ShowWindow(hwnd, showCmd);
  UpdateWindow(hwnd);
  g_windowPlacementPersistenceReady = true;
  SaveWindowPlacementToRegistry(hwnd);
  return hwnd;
}
