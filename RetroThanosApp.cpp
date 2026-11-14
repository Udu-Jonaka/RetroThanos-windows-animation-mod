#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#define WM_TRAYICON (WM_USER + 100)

// Menu Command IDs
#define ID_TRAY_TITLE        1001
#define ID_TRAY_TOGGLE       1002
#define ID_SPEED_SLOW        1003
#define ID_SPEED_NORMAL      1004
#define ID_SPEED_FAST        1005
#define ID_SIZE_SMALL        1006
#define ID_SIZE_MEDIUM       1007
#define ID_SIZE_LARGE        1008
#define ID_TRAY_EXIT         1009

// Function pointer signatures from RetroThanosEngine.dll
using InstallHook_t    = BOOL (*)();
using UninstallHook_t  = void (*)();
using SetEnabled_t     = void (*)(BOOL);
using IsHookEnabled_t  = BOOL (*)();
using SetDurations_t   = void (*)(int, int);
using SetParticleSize_t= void (*)(int);

static InstallHook_t     fnInstallHook     = nullptr;
static UninstallHook_t   fnUninstallHook   = nullptr;
static SetEnabled_t      fnSetEnabled      = nullptr;
static IsHookEnabled_t   fnIsHookEnabled   = nullptr;
static SetDurations_t    fnSetDurations    = nullptr;
static SetParticleSize_t fnSetParticleSize = nullptr;

static HMODULE g_hEngineDll = nullptr;
static NOTIFYICONDATAW g_nid{};
static HWND g_hWnd = nullptr;

// Current state
static BOOL g_bEnabled = TRUE;
static int g_currentSpeed = ID_SPEED_NORMAL;
static int g_currentSize  = ID_SIZE_MEDIUM;

static void ApplySettings() {
    if (!fnSetDurations || !fnSetParticleSize || !fnSetEnabled) return;

    fnSetEnabled(g_bEnabled);

    int minDuration = 750, closeDuration = 700;
    if (g_currentSpeed == ID_SPEED_SLOW) {
        minDuration = 1100; closeDuration = 1000;
    } else if (g_currentSpeed == ID_SPEED_FAST) {
        minDuration = 450; closeDuration = 450;
    }
    fnSetDurations(minDuration, closeDuration);

    int pSize = 4;
    if (g_currentSize == ID_SIZE_SMALL) pSize = 2;
    else if (g_currentSize == ID_SIZE_LARGE) pSize = 6;
    fnSetParticleSize(pSize);
}

static void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    HMENU hSpeedMenu = CreatePopupMenu();
    HMENU hSizeMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, ID_TRAY_TITLE, L"⚡ Retro TV & Thanos Animations");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING | (g_bEnabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_TOGGLE, L"&Enabled");

    AppendMenuW(hSpeedMenu, MF_STRING | (g_currentSpeed == ID_SPEED_SLOW ? MF_CHECKED : MF_UNCHECKED), ID_SPEED_SLOW, L"Cinematic Slow (1000 ms)");
    AppendMenuW(hSpeedMenu, MF_STRING | (g_currentSpeed == ID_SPEED_NORMAL ? MF_CHECKED : MF_UNCHECKED), ID_SPEED_NORMAL, L"Normal (750 ms)");
    AppendMenuW(hSpeedMenu, MF_STRING | (g_currentSpeed == ID_SPEED_FAST ? MF_CHECKED : MF_UNCHECKED), ID_SPEED_FAST, L"Snappy Fast (450 ms)");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSpeedMenu, L"&Animation Speed");

    AppendMenuW(hSizeMenu, MF_STRING | (g_currentSize == ID_SIZE_SMALL ? MF_CHECKED : MF_UNCHECKED), ID_SIZE_SMALL, L"Fine Dust (2 px)");
    AppendMenuW(hSizeMenu, MF_STRING | (g_currentSize == ID_SIZE_MEDIUM ? MF_CHECKED : MF_UNCHECKED), ID_SIZE_MEDIUM, L"Standard Particles (4 px)");
    AppendMenuW(hSizeMenu, MF_STRING | (g_currentSize == ID_SIZE_LARGE ? MF_CHECKED : MF_UNCHECKED), ID_SIZE_LARGE, L"Chunky Blocks (6 px)");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSizeMenu, L"&Particle Size");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"E&xit");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hSizeMenu);
    DestroyMenu(hSpeedMenu);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
            ShowContextMenu(hWnd);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_TOGGLE:
            g_bEnabled = !g_bEnabled;
            ApplySettings();
            break;

        case ID_SPEED_SLOW:
        case ID_SPEED_NORMAL:
        case ID_SPEED_FAST:
            g_currentSpeed = LOWORD(wParam);
            ApplySettings();
            break;

        case ID_SIZE_SMALL:
        case ID_SIZE_MEDIUM:
        case ID_SIZE_LARGE:
            g_currentSize = LOWORD(wParam);
            ApplySettings();
            break;

        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;
        }
        break;

    case WM_DESTROY:
        if (fnUninstallHook) fnUninstallHook();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Single instance mutex
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"RetroThanos_SingleInstance_App_Mutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Retro TV & Thanos Animations is already running in your System Tray!", L"Already Running", MB_ICONINFORMATION | MB_OK);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Load Engine DLL using full absolute path
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }
    wcscat_s(dllPath, L"RetroThanosEngine.dll");

    g_hEngineDll = LoadLibraryW(dllPath);
    if (!g_hEngineDll) {
        DWORD err = GetLastError();
        WCHAR errBuf[512];
        wsprintfW(errBuf, L"Could not load:\n%s\n\nWindows Error Code: %lu", dllPath, err);
        MessageBoxW(nullptr, errBuf, L"Missing DLL", MB_ICONERROR | MB_OK);
        return 1;
    }

    fnInstallHook     = (InstallHook_t)GetProcAddress(g_hEngineDll, "InstallHook");
    fnUninstallHook   = (UninstallHook_t)GetProcAddress(g_hEngineDll, "UninstallHook");
    fnSetEnabled      = (SetEnabled_t)GetProcAddress(g_hEngineDll, "SetEnabled");
    fnIsHookEnabled   = (IsHookEnabled_t)GetProcAddress(g_hEngineDll, "IsHookEnabled");
    fnSetDurations    = (SetDurations_t)GetProcAddress(g_hEngineDll, "SetDurations");
    fnSetParticleSize = (SetParticleSize_t)GetProcAddress(g_hEngineDll, "SetParticleSize");

    if (!fnInstallHook || !fnUninstallHook) {
        MessageBoxW(nullptr, L"Failed to locate hook functions in RetroThanosEngine.dll!", L"DLL Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    // Register Window Class for Tray message pump
    const WCHAR kClassName[] = L"RetroThanos_TrayMsgReceiver";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, kClassName, L"RetroThanosReceiver", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!g_hWnd) return 1;

    // Install System Tray Icon
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hWnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon  = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Retro TV & Thanos Animations (Running)");

    // Notification balloon on launch
    wcscpy_s(g_nid.szInfoTitle, L"Retro TV & Thanos Snap Active");
    wcscpy_s(g_nid.szInfo, L"Running in background! Minimize windows for Retro TV, close for Thanos Snap.");
    g_nid.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Start Hooks
    if (!fnInstallHook()) {
        MessageBoxW(nullptr, L"Failed to install system hook!", L"Hook Error", MB_ICONERROR | MB_OK);
        DestroyWindow(g_hWnd);
        return 1;
    }

    ApplySettings();

    // Message Loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hEngineDll) {
        FreeLibrary(g_hEngineDll);
        g_hEngineDll = nullptr;
    }
    if (hMutex) {
        CloseHandle(hMutex);
    }

    return 0;
}
