#include <windows.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <iomanip>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// ═══════════════════════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════════════════════
const int CANVAS_WIDTH  = 10000;
const int CANVAS_HEIGHT = 10000;

WPARAM g_activateKey = VK_RCONTROL;
WPARAM g_panKey = 0;

const int THREAD_SLEEP_MS = 4; 
const int GRID_ANIM_DURATION = 600; // Длительность анимации сетки (мс)

// ═══════════════════════════════════════════════════════════════════════════════
// СТРУКТУРЫ
// ═══════════════════════════════════════════════════════════════════════════════
struct WindowSnapshot {
    HWND hwnd;
    int  baseX, baseY; 
    int  width, height;
};

struct WindowMoveOp {
    HWND hwnd;
    int x, y, w, h;
    UINT flags;
};

struct SnapshotCtx {
    std::vector<WindowSnapshot>* list;
    POINT offset;
};

// Элемент анимации сетки
struct GridAnimItem {
    HWND hwnd;
    int startX, startY;
    int endX, endY;
    int width, height;
};

// Состояние анимации сетки
struct GridAnimState {
    bool active = false;
    std::vector<GridAnimItem> items;
    std::chrono::steady_clock::time_point startTime;
    int durationMs = GRID_ANIM_DURATION;
};

// Вспомогательная структура для сортировки окон
struct SortedWindow {
    HWND hwnd;
    int w, h;
    long long area;
    int curX, curY; // Текущие координаты (для старта анимации)
};

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ═══════════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_isDragging(false);
std::atomic<WPARAM> g_panStartButton = 0; // Запоминаем, какая кнопка начала тягу (VK_MBUTTON, VK_LBUTTON и т.д.)
POINT g_dragStartMouse = {0, 0};
POINT g_currentMouse = {0, 0};
std::vector<WindowSnapshot> g_snapshots;
std::vector<HWND> g_newWindowsFound; // Нееповторяющийся буфер новых окон
std::wstring g_newWindowNotice;
HWND g_autoFocusTarget = NULL; // Флаг для запроса автослежения за новым окном
CRITICAL_SECTION g_lock;
std::thread g_worker;
std::atomic<bool> g_stop(false);

HHOOK g_mouseHook = NULL;
HHOOK g_kbHook = NULL;
HWND g_hwnd = NULL;       
HWND g_debugHwnd = NULL;  

POINT g_camOffset = {0, 0};

// Анимация камеры (зум/пан к окну)
std::atomic<bool> g_isCamAnim(false);
POINT g_camAnimStart = {0, 0};
POINT g_camAnimTarget = {0, 0};
std::chrono::steady_clock::time_point g_camAnimTime;
const int CAM_ANIM_DURATION = 400;

// Авто-анимация камеры для активных окон (блокировка ввода)
std::atomic<bool> g_autoCamAnim(false);

// Анимация сетки
GridAnimState g_gridAnim;

std::wstring g_debugText = L"";
CRITICAL_SECTION g_debugLock;

// Double-Tap Globals
DWORD g_lastActivateKeyPress = 0;
DWORD g_lastActivateKeyRelease = 0;
bool g_wasActivateKeyDown = false;
bool g_doubleTapHandled = false;
const DWORD DOUBLE_TAP_TIMEOUT = 300;
const DWORD HOLD_THRESHOLD = 200;

// Key Binding Globals
bool g_bindingMode = false;
bool g_bindingPanKey = false;
RECT g_btnBindRect = {0};
RECT g_btnPanRect = {0};

// ═══════════════════════════════════════════════════════════════════════════════
// СТРУКТУРЫ ДЛЯ ОТЛОЖЕННОЙ СТЫКОВКИ
// ═══════════════════════════════════════════════════════════════════════════════
struct PendingWindow {
    HWND hwnd;
    int targetAbsX, targetAbsY; // Абсолютные целевые координаты в сетке
    int width, height;
};

std::vector<PendingWindow> g_pendingWindows;

// ═══════════════════════════════════════════════════════════════════════════════
// УТИЛИТЫ
// ═══════════════════════════════════════════════════════════════════════════════
bool IsAdmin() {
    BOOL res = FALSE;
    PSID grp = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &grp)) {
        CheckTokenMembership(NULL, grp, &res);
        FreeSid(grp);
    }
    return res == TRUE;
}

void RunAsAdmin() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(NULL, L"Нужны права админа!", L"Error", MB_ICONERROR);
        ExitProcess(1);
    }
    ExitProcess(0);
}

std::wstring GetKeyNameStr(WPARAM vkCode) {
    if (vkCode == 0) return L"None (Not Set)";
    switch (vkCode) {
        case VK_LBUTTON: return L"Left Mouse";
        case VK_RBUTTON: return L"Right Mouse";
        case VK_MBUTTON: return L"Middle Mouse";
        case VK_XBUTTON1: return L"Mouse X1";
        case VK_XBUTTON2: return L"Mouse X2";
    }
    LONG scanCode = MapVirtualKeyExW((UINT)vkCode, 0, GetKeyboardLayout(0));
    LONG lParam = (scanCode << 16);
    wchar_t name[64] = {0};
    if (GetKeyNameTextW(lParam, name, 63) && wcslen(name) > 0) return std::wstring(name);
    switch (vkCode) {
        case VK_CONTROL: return L"Ctrl"; case VK_MENU: return L"Alt"; case VK_SHIFT: return L"Shift";
        case VK_LCONTROL: return L"Left Ctrl"; case VK_RCONTROL: return L"Right Ctrl";
        case VK_NUMPAD0: return L"Numpad 0"; case VK_NUMPAD5: return L"Numpad 5";
        default:
            if (vkCode >= 'A' && vkCode <= 'Z') return std::wstring(1, (wchar_t)vkCode);
            if (vkCode >= VK_F1 && vkCode <= VK_F12) return L"F" + std::to_wstring(vkCode - VK_F1 + 1);
            return L"Key " + std::to_wstring(vkCode);
    }
}

bool IsValidWnd(HWND h) {
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 1: IMMEDIATE REJECTS (Handle validity, self-exclusion)
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    if (!h || !IsWindow(h)) return false;
    
    // Exclude our own windows by pointer identity (most reliable check)
    if (h == g_hwnd) return false;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 2: TOP-LEVEL WINDOW VERIFICATION
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Get window and extended styles
    LONG style = GetWindowLongW(h, GWL_STYLE);
    LONG exStyle = GetWindowLongW(h, GWL_EXSTYLE);
    
    // Reject child windows (we only want top-level)
    if (style & WS_CHILD) return false;
    
    // Reject tool windows and tool-window-like windows
    // These are helper/support windows, not primary application windows
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 3: OWNERSHIP & HIERARCHY CHECKS (Catches UWP ghost windows)
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // If window has an owner, it's not truly top-level (likely a modal dialog or owned window)
    // UWP ghost windows often have ownership/parent relationships
    HWND owner = GetWindow(h, GW_OWNER);
    if (owner != NULL) {
        // Owner exists. This is an owned window (like a dialog).
        // Exclude UNLESS it's a visible, substantial window (edge case: some apps use owned windows)
        // For safety, reject owned windows as they're typically not primary application windows
        return false;
    }
    
    // Verify this window is truly the root of its hierarchy
    // Some system windows are deeply nested; GA_ROOT finds the topmost ancestor
    HWND root = GetAncestor(h, GA_ROOT);
    if (root != h) {
        // This window is part of a hierarchy, not a true top-level
        // Reject it (this catches some shell-integrated windows)
        return false;
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 4: VISIBILITY CHECK
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Note: IsWindowVisible() returns TRUE for some hidden UWP background frames,
    // but combined with owner/hierarchy checks above, we've already filtered those.
    // Now require visibility to show in our grid.
    if (!IsWindowVisible(h)) return false;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 5: SIZE & DIMENSION CHECKS
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    RECT r;
    if (!GetWindowRect(h, &r)) return false;
    
    int w = r.right - r.left;
    int hgt = r.bottom - r.top;

    // Reject windows larger than or equal to the entire screen
    // These are typically wallpaper/desktop/shell windows
    if (w >= GetSystemMetrics(SM_CXSCREEN) && hgt >= GetSystemMetrics(SM_CYSCREEN)) {
        return false;
    }

    // Reject windows that are too small to be meaningful applications
    // Minimum sensible size for an app window
    if (w < 100 || hgt < 50) return false;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 6: TITLE CHECK (Important for identifying real applications)
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    wchar_t title[256] = {0};
    int titleLen = GetWindowTextW(h, title, 255);
    
    // Reject windows with empty titles
    // Real applications almost always have a title (app name, filename, etc.)
    // Ghost/background windows often lack titles
    if (titleLen == 0) return false;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 7: CLASS NAME TARGETED REJECTS (Strict allowlist approach)
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 63);
    
    // IMPORTANT: Only reject KNOWN system classes.
    // Do NOT reject ApplicationFrameHost, Windows.UI.Core.CoreWindow indiscriminately
    // as these are used by legitimate visible UWP apps like Edge.
    // The ownership/hierarchy checks above already filter out ghost frames.
    
    // True system/shell classes that should never appear in the grid
    const wchar_t* systemOnlyClasses[] = {
        L"Shell_TrayWnd",           // Taskbar
        L"Progman",                 // Desktop window manager
        L"WorkerW",                 // Desktop worker window
        L"NotifyIconOverflowWindow",// System notification area overflow
        L"ImmersiveLauncher",       // Windows 10+ Start Menu
        L"Shell_SecondaryTrayHost", // Secondary taskbars
        L"SearchUI"                 // Windows Search popup (when summoned but backgrounded)
    };

    for (const auto& sysClass : systemOnlyClasses) {
        if (wcscmp(cls, sysClass) == 0) return false;
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 8: EXTENDED STYLE - ACTIVATION CHECKS
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // A window that cannot be activated (WS_EX_NOACTIVATE) AND is small and has no special title
    // is likely a system window. But we only reject if it also fails other heuristics.
    // Combined with previous checks, most ghosts are already gone.
    if (exStyle & WS_EX_NOACTIVATE) {
        // Window cannot be activated. It's informational or system-managed.
        // However, legitimate windows (like some file dialogs) can have this.
        // Additional heuristic: if it also has a very generic title or is a known UWP overlay,
        // Consider rejecting. For now, this is a weak signal combined with other checks.
        
        // Special case: ApplicationFrameHost with WS_EX_NOACTIVATE is likely a ghost
        // But non-ApplicationFrameHost windows with WS_EX_NOACTIVATE can be valid (e.g., search boxes)
        // We've already removed owned windows, so allow this for now.
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // PHASE 9: FINAL VALIDATION - WINDOWS IN ALT+TAB (Optional deep check)
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // GetWindowDisplayAffinity can tell us if the window should appear in taskbar/switcher,
    // but this is relatively expensive. Skip for performance unless debugging.
    // Comment out unless needed:
    // DWORD affinity = 0;
    // GetWindowDisplayAffinity(h, &affinity);
    // if (affinity & WDA_EXCLUDEFROMCAPTURE) return false; // Hidden from Alt+Tab-like enumeration

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // ALL CHECKS PASSED: This is a valid user application window
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    return true;
}

void ArrangeGrid();
void TakeSnapshot();
void FocusOnWindow(HWND target);


// ═══════════════════════════════════════════════════════════════════════════════
// ЛОГИКА
// ═══════════════════════════════════════════════════════════════════════════════
bool CheckDoubleTap(WPARAM vkCode, bool isKeyDown) {
    if (vkCode != g_activateKey) return false;
    DWORD now = GetTickCount();
    if (isKeyDown) {
        if (g_wasActivateKeyDown && !g_doubleTapHandled) return false;
        if (g_lastActivateKeyPress > 0 && g_lastActivateKeyRelease > 0) {
            DWORD pressDuration = g_lastActivateKeyRelease - g_lastActivateKeyPress;
            if (pressDuration < HOLD_THRESHOLD && (now - g_lastActivateKeyRelease) < DOUBLE_TAP_TIMEOUT) {
                g_doubleTapHandled = true;
                return true;
            }
        }
        g_lastActivateKeyPress = now;
        g_wasActivateKeyDown = true;
    } else {
        g_lastActivateKeyRelease = now;
        g_wasActivateKeyDown = false;
        if (g_doubleTapHandled) g_doubleTapHandled = false;
    }
    return false;
}

void HandleDoubleTapReset() {
    g_doubleTapHandled = false;
    ArrangeGrid();
}

// ═══════════════════════════════════════════════════════════════════════════════
// ОТЛАДОЧНОЕ ОКНО
// ═══════════════════════════════════════════════════════════════════════════════
void UpdateDebugWindow() {
    std::wstringstream ss;
    ss << L"=== WCWM DEBUG ===\n";
    ss << L"Camera: " << g_camOffset.x << L", " << g_camOffset.y << L"\n";
    ss << L"Grid Anim: " << (g_gridAnim.active ? L"RUNNING" : L"IDLE") << L"\n";
    ss << L"Windows Cached: " << g_snapshots.size() << L"\n";
    ss << L"Activate: " << GetKeyNameStr(g_activateKey) << L"\n";
    ss << L"Pan: " << GetKeyNameStr(g_panKey) << L"\n";
    
    if (g_bindingMode) {
        ss << L"\n>>> WAITING FOR INPUT... <<<\n";
    }
    std::wstring newWindowNotice;
    EnterCriticalSection(&g_debugLock);
    newWindowNotice = g_newWindowNotice;
    LeaveCriticalSection(&g_debugLock);
    if (!newWindowNotice.empty()) {
        ss << newWindowNotice << L"\n";
    }
    ss << L"---------------------\n";

    EnterCriticalSection(&g_lock);
    int count = 0;
    for (const auto& s : g_snapshots) {
        if (!IsWindow(s.hwnd)) continue;
        if (count >= 15) { ss << L"... (and more)\n"; break; }
        
        wchar_t title[256] = {0};
        GetWindowTextW(s.hwnd, title, 255);
        if (wcslen(title) == 0) wcscpy_s(title, 256, L"<No Title>");

        ss << L"[" << count << L"] " << title << L"\n";
        count++;
    }
    LeaveCriticalSection(&g_lock);

    EnterCriticalSection(&g_debugLock);
    g_debugText = ss.str();
    LeaveCriticalSection(&g_debugLock);

    if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
}

LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetWindowTextW(hwnd, L"WCWM");
            return 0;
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            g_btnBindRect = { rc.right - 170, 10, rc.right - 10, 40 };
            g_btnPanRect = { rc.right - 170, 45, rc.right - 10, 75 };
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam); int y = HIWORD(lParam);
            if (x >= g_btnBindRect.left && x <= g_btnBindRect.right && y >= g_btnBindRect.top && y <= g_btnBindRect.bottom) {
                g_bindingMode = true; g_bindingPanKey = false;
                if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
            } else if (x >= g_btnPanRect.left && x <= g_btnPanRect.right && y >= g_btnPanRect.top && y <= g_btnPanRect.bottom) {
                g_bindingMode = true; g_bindingPanKey = true;
                if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HBRUSH hBrush = CreateSolidBrush(RGB(240, 240, 240));
            FillRect(hdc, &rc, hBrush); DeleteObject(hBrush);

            COLORREF c1 = (g_bindingMode && !g_bindingPanKey) ? RGB(255, 200, 200) : RGB(200, 220, 255);
            HBRUSH b1 = CreateSolidBrush(c1); FillRect(hdc, &g_btnBindRect, b1); FrameRect(hdc, &g_btnBindRect, (HBRUSH)GetStockObject(BLACK_BRUSH)); DeleteObject(b1);
            
            COLORREF c2 = (g_bindingMode && g_bindingPanKey) ? RGB(255, 200, 200) : RGB(200, 255, 200);
            HBRUSH b2 = CreateSolidBrush(c2); FillRect(hdc, &g_btnPanRect, b2); FrameRect(hdc, &g_btnPanRect, (HBRUSH)GetStockObject(BLACK_BRUSH)); DeleteObject(b2);

            SetBkMode(hdc, TRANSPARENT);
            HFONT f = CreateFontW(12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
            HFONT old = (HFONT)SelectObject(hdc, f);
            DrawTextW(hdc, (g_bindingMode && !g_bindingPanKey) ? L"LISTENING..." : L"Set Activate", -1, &g_btnBindRect, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            DrawTextW(hdc, (g_bindingMode && g_bindingPanKey) ? L"LISTENING..." : L"Set Pan Key", -1, &g_btnPanRect, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(hdc, old); DeleteObject(f);

            HFONT fc = CreateFontW(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH|FF_MODERN, L"Consolas");
            SelectObject(hdc, fc);
            RECT tr = { 10, 85, rc.right - 10, rc.bottom - 10 };
            EnterCriticalSection(&g_debugLock); std::wstring t = g_debugText; LeaveCriticalSection(&g_debugLock);
            DrawTextW(hdc, t.c_str(), -1, &tr, DT_LEFT|DT_TOP|DT_WORDBREAK);
            DeleteObject(fc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            g_debugHwnd = NULL; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateDebugWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DebugWndProc; wc.hInstance = hInst; wc.lpszClassName = L"CanvasDebugClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    if (!RegisterClassExW(&wc)) return;
    g_debugHwnd = CreateWindowExW(0, L"CanvasDebugClass", L"WCWM", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 450, 650, NULL, NULL, hInst, NULL);
    if (g_debugHwnd) ShowWindow(g_debugHwnd, SW_SHOW);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ДВИЖОК
// ═══════════════════════════════════════════════════════════════════════════════
void ApplyMoves(const std::vector<WindowMoveOp>& ops) {
    if (ops.empty()) return;
    HDWP h = BeginDeferWindowPos((int)ops.size());
    if (!h) { for (auto& o : ops) SetWindowPos(o.hwnd, 0, o.x, o.y, o.w, o.h, o.flags); return; }
    for (auto& o : ops) { h = DeferWindowPos(h, o.hwnd, 0, o.x, o.y, o.w, o.h, o.flags); if (!h) { EndDeferWindowPos(h); return; } }
    EndDeferWindowPos(h);
}

void ArrangeGrid() {
    std::vector<WindowSnapshot> list;
    list.reserve(64);
    
    // 1. Сбор окон (берем текущие реальные координаты для старта анимации)
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!IsValidWnd(h)) return TRUE;
        RECT r; GetWindowRect(h, &r);
        auto* v = (std::vector<WindowSnapshot>*)l;
        v->push_back({h, r.left, r.top, r.right-r.left, r.bottom-r.top});
        return TRUE;
    }, (LPARAM)&list);

    if (list.empty()) return;

    // 2. Сортировка: самое большое окно должно быть первым (оно станет центром)
    std::sort(list.begin(), list.end(), [](const WindowSnapshot& a, const WindowSnapshot& b) {
        return (a.width * a.height) > (b.width * b.height);
    });

    struct PlacedRect { int x, y, w, h; };
    std::vector<PlacedRect> placed;
    std::vector<POINT> finalPositions;
    
    EnterCriticalSection(&g_lock);
    g_gridAnim.items.clear();
    g_gridAnim.items.reserve(list.size());

    // Вспомогательная лямбда для проверки пересечений
    auto intersects = [&](int nx, int ny, int nw, int nh, const std::vector<PlacedRect>& currentPlaced) {
        for (const auto& pr : currentPlaced) {
            // Проверка: если НЕ пересекается, то продолжаем. Если пересекается - возвращаем true.
            // Пересечение есть, если прямоугольники НАЛАГАЮТСЯ.
            bool noOverlap = (nx + nw <= pr.x || nx >= pr.x + pr.w || 
                              ny + nh <= pr.y || ny >= pr.y + pr.h);
            if (!noOverlap) return true;
        }
        return false;
    };

    // 3. Пошаговая упаковка каждого окна
    for (size_t i = 0; i < list.size(); ++i) {
        int w = list[i].width;
        int h = list[i].height;
        POINT bestPos = {0, 0};
        long long minDistSq = -1;

        if (i == 0) {
            // Первое окно — в центр координат (0,0)
            bestPos = { -w/2, -h/2 };
        } else {
            // Для остальных ищем лучшее место среди ВСЕХ уже размещенных окон
            for (const auto& pr : placed) {
                // Генерируем 4 кандидата вокруг текущего размещенного окна
                std::vector<std::pair<int, int>> slots = {
                    {pr.x + pr.w, pr.y},       // Справа
                    {pr.x - w,       pr.y},    // Слева
                    {pr.x,           pr.y + pr.h}, // Снизу
                    {pr.x,           pr.y - h}     // Сверху
                };

                for (auto& slot : slots) {
                    int cx = slot.first;
                    int cy = slot.second;

                    // Проверяем, не пересекается ли этот слот с УЖЕ размещенными окнами
                    if (!intersects(cx, cy, w, h, placed)) {
                        // Считаем расстояние от центра (0,0) до центра этого слота
                        int slotCenterX = cx + w/2;
                        int slotCenterY = cy + h/2;
                        long long distSq = 1LL * slotCenterX * slotCenterX + 1LL * slotCenterY * slotCenterY;

                        // Выбираем слот с минимальным расстоянием до центра
                        if (minDistSq == -1 || distSq < minDistSq) {
                            minDistSq = distSq;
                            bestPos = {cx, cy};
                        }
                    }
                }
            }
        }

        placed.push_back({bestPos.x, bestPos.y, w, h});
        finalPositions.push_back(bestPos);
    }

    // 4. Центрирование всей полученной фигуры на экране
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const auto& p : placed) {
        if (p.x < minX) minX = p.x;
        if (p.y < minY) minY = p.y;
        if (p.x + p.w > maxX) maxX = p.x + p.w;
        if (p.y + p.h > maxY) maxY = p.y + p.h;
    }

    int totalW = maxX - minX;
    int totalH = maxY - minY;
    int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
    int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

    // Смещение, чтобы центр фигуры совпал с центром экрана
    int offsetX = screenCx - (minX + totalW / 2);
    int offsetY = screenCy - (minY + totalH / 2);

    // 5. Формирование анимации
    for (size_t i = 0; i < list.size(); ++i) {
        GridAnimItem item;
        item.hwnd = list[i].hwnd;
        item.startX = list[i].baseX; 
        item.startY = list[i].baseY;
        
        // Применяем смещение
        item.endX = finalPositions[i].x + offsetX;
        item.endY = finalPositions[i].y + offsetY;
        
        item.width = list[i].width;
        item.height = list[i].height;
        
        g_gridAnim.items.push_back(item);
    }

    g_camOffset = {0, 0};
    g_gridAnim.startTime = std::chrono::steady_clock::now();
    g_gridAnim.active = true;
    
    LeaveCriticalSection(&g_lock);
}

void TakeSnapshot() {
    EnterCriticalSection(&g_lock);
    g_snapshots.clear();
    g_snapshots.reserve(64);
    SnapshotCtx ctx{&g_snapshots, g_camOffset};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!IsValidWnd(h)) return TRUE;
        RECT r; GetWindowRect(h, &r);
        auto* c = (SnapshotCtx*)l;
        c->list->push_back({h, r.left - c->offset.x, r.top - c->offset.y, r.right-r.left, r.bottom-r.top});
        return TRUE;
    }, (LPARAM)&ctx);
    LeaveCriticalSection(&g_lock);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ПОТОК
// ═══════════════════════════════════════════════════════════════════════════════
POINT FindBestSpot(HWND newHwnd, int newW, int newH, const std::vector<WindowSnapshot>& snapshots, POINT camOffset) {
    // Получаем текущую позицию нового окна (абсолютные экранные координаты)
    RECT newRect;
    if (!GetWindowRect(newHwnd, &newRect)) return {0, 0};
    int curCenterX = (newRect.left + newRect.right) / 2;
    int curCenterY = (newRect.top + newRect.bottom) / 2;

    // Структура для кандидатов
    struct Candidate {
        int x, y;
        long long distSq;
    };
    std::vector<Candidate> candidates;

    // Функция проверки пересечения с любым существующим окном
    auto intersectsAny = [&](int cx, int cy, int cw, int ch) -> bool {
        for (const auto& s : snapshots) {
            int sx = s.baseX + camOffset.x;
            int sy = s.baseY + camOffset.y;
            int sw = s.width;
            int sh = s.height;
            if (!(cx + cw <= sx || cx >= sx + sw || cy + ch <= sy || cy >= sy + sh)) {
                return true; // Пересекается
            }
        }
        return false;
    };

    // Генерируем кандидатов для каждого существующего окна
    for (const auto& s : snapshots) {
        int sx = s.baseX + camOffset.x;
        int sy = s.baseY + camOffset.y;
        int sw = s.width;
        int sh = s.height;

        // 4 кандидата: Right, Left, Bottom, Top
        std::vector<std::pair<int, int>> slots = {
            {sx + sw, sy},       // Right
            {sx - newW, sy},     // Left
            {sx, sy + sh},       // Bottom
            {sx, sy - newH}      // Top
        };

        for (auto& slot : slots) {
            int cx = slot.first;
            int cy = slot.second;
            if (!intersectsAny(cx, cy, newW, newH)) {
                // Свободно: рассчитываем расстояние от текущего центра нового окна
                int slotCenterX = cx + newW / 2;
                int slotCenterY = cy + newH / 2;
                long long dx = slotCenterX - curCenterX;
                long long dy = slotCenterY - curCenterY;
                long long distSq = dx * dx + dy * dy;
                candidates.push_back({cx, cy, distSq});
            }
        }
    }

    // Выбираем кандидата с минимальным расстоянием
    if (!candidates.empty()) {
        auto minIt = std::min_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.distSq < b.distSq;
        });
        return {minIt->x, minIt->y};
    }

    // Fallback: Если нет свободных слотов рядом, размещаем ниже самого нижнего окна
    int maxY = 0;
    for (const auto& s : snapshots) {
        int sy = s.baseY + camOffset.y + s.height;
        if (sy > maxY) maxY = sy;
    }
    int fallbackY = maxY + 10; // Небольшой отступ
    return {curCenterX - newW / 2, fallbackY}; // Центрируем по X, ниже по Y
}

// Вычисляет целевую позицию камеры для центрирования окна на экране
POINT CalculateCameraTarget(HWND hwnd, int width, int height) {
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return {0, 0};
    
    int scX = GetSystemMetrics(SM_CXSCREEN) / 2;
    int scY = GetSystemMetrics(SM_CYSCREEN) / 2;
    
    // Вычисляем смещение камеры, чтобы центр окна оказался в центре экрана
    int windowCenterX = r.left + width / 2;
    int windowCenterY = r.top + height / 2;
    
    int dx = scX - windowCenterX;
    int dy = scY - windowCenterY;
    
    return {g_camOffset.x + dx, g_camOffset.y + dy};
}

void WorkerFunc() {
    auto lastWindowScan = std::chrono::steady_clock::now();
    
    while (!g_stop.load()) {
        bool drag = g_isDragging.load();
        bool camAnim = g_isCamAnim.load();
        bool gridAnim = g_gridAnim.active;

        // ============================================================
        // 1. ПЕРИОДИЧЕСКОЕ СКАНИРОВАНИЕ НОВЫХ ОКОН
        // ============================================================
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWindowScan).count() >= 1000) {
            lastWindowScan = now;

            std::vector<HWND> knownHwnds;
            EnterCriticalSection(&g_lock);
            knownHwnds.reserve(g_snapshots.size() + g_newWindowsFound.size());
            for (const auto& s : g_snapshots) knownHwnds.push_back(s.hwnd);
            for (const auto& h : g_newWindowsFound) knownHwnds.push_back(h);
            LeaveCriticalSection(&g_lock);

            struct ScanCtx {
                const std::vector<HWND>* pKnown;
                std::wstring notice;
                HWND newHwnd;
            };

            ScanCtx ctx;
            ctx.pKnown = &knownHwnds;
            ctx.notice = L"";
            ctx.newHwnd = NULL;

            EnumWindows([](HWND h, LPARAM l) -> BOOL {
                if (!IsValidWnd(h)) return TRUE;
                ScanCtx* c = reinterpret_cast<ScanCtx*>(l);
                if (std::find(c->pKnown->begin(), c->pKnown->end(), h) != c->pKnown->end()) return TRUE;

                wchar_t title[256] = {0};
                GetWindowTextW(h, title, _countof(title));
                std::wstringstream ss;
                ss << L"[NEW] " << (title[0] ? title : L"<No Title>");
                c->notice = ss.str();
                c->newHwnd = h;
                return FALSE; 
            }, reinterpret_cast<LPARAM>(&ctx));

            if (ctx.newHwnd != NULL) {
                RECT r;
                if (GetWindowRect(ctx.newHwnd, &r)) {
                    int w = r.right - r.left;
                    int h = r.bottom - r.top;

                    // Проверяем, является ли новое окно активным
                    bool isActive = false;
                    HWND fgWnd = GetForegroundWindow();
                    if (fgWnd == ctx.newHwnd || GetAncestor(fgWnd, GA_ROOTOWNER) == ctx.newHwnd) {
                        isActive = true;
                    }

                    EnterCriticalSection(&g_lock);
                    
                    if (isActive) {
                        // АКТИВНОЕ ОКНО: Отложенная стыковка
                        // Находим лучшую позицию
                        POINT targetPos = FindBestSpot(ctx.newHwnd, w, h, g_snapshots, g_camOffset);
                        
                        // Добавляем в список отложенных окон
                        PendingWindow pw;
                        pw.hwnd = ctx.newHwnd;
                        pw.targetAbsX = targetPos.x;
                        pw.targetAbsY = targetPos.y;
                        pw.width = w;
                        pw.height = h;
                        g_pendingWindows.push_back(pw);
                        
                        // Запускаем анимацию камеры к этому окну
                        POINT camTarget = CalculateCameraTarget(ctx.newHwnd, w, h);
                        g_camAnimStart = g_camOffset;
                        g_camAnimTarget = camTarget;
                        g_camAnimTime = std::chrono::steady_clock::now();
                        g_isCamAnim.store(true);
                        g_autoCamAnim.store(true); // Блокируем ввод во время авто-анимации
                        
                        LeaveCriticalSection(&g_lock);
                        
                        // Логирование
                        std::wstring noticeMsg = ctx.notice + L" -> DEFERRED DOCKING (CAM ANIMATING)";
                        EnterCriticalSection(&g_debugLock);
                        g_newWindowNotice = noticeMsg;
                        LeaveCriticalSection(&g_debugLock);
                    } else {
                        // НЕАКТИВНОЕ ОКНО: Немедленная анимация в сетку
                        g_newWindowsFound.push_back(ctx.newHwnd);
                        
                        // Находим лучшую позицию
                        POINT targetPos = FindBestSpot(ctx.newHwnd, w, h, g_snapshots, g_camOffset);
                        
                        // Создаем анимацию для окна
                        GridAnimItem newItem;
                        newItem.hwnd = ctx.newHwnd;
                        newItem.startX = r.left;
                        newItem.startY = r.top;
                        newItem.endX = targetPos.x;
                        newItem.endY = targetPos.y;
                        newItem.width = w;
                        newItem.height = h;
                        
                        g_gridAnim.items.push_back(newItem);
                        g_gridAnim.startTime = std::chrono::steady_clock::now();
                        g_gridAnim.active = true;
                        
                        LeaveCriticalSection(&g_lock);
                        
                        // Логирование
                        std::wstring noticeMsg = ctx.notice + L" -> ANIMATING TO GRID";
                        EnterCriticalSection(&g_debugLock);
                        g_newWindowNotice = noticeMsg;
                        LeaveCriticalSection(&g_debugLock);
                    }
                }
            }
        }

        // ============================================================
        // 2. ОСНОВНОЙ ЦИКЛ ОТРИСОВКИ И АНИМАЦИИ
        // ============================================================
        std::vector<WindowMoveOp> ops;
        
        EnterCriticalSection(&g_lock);
        
        if (g_autoFocusTarget != NULL && IsWindow(g_autoFocusTarget)) {
            // Запускаем анимацию камеры к этому окну
            RECT r;
            if (GetWindowRect(g_autoFocusTarget, &r)) {
                int scX = GetSystemMetrics(SM_CXSCREEN)/2;
                int scY = GetSystemMetrics(SM_CYSCREEN)/2;
                
                int dx = scX - (r.left + (r.right - r.left)/2);
                int dy = scY - (r.top + (r.bottom - r.top)/2);

                // Важно: не прерывать, если уже идет камерная анимация, или перезаписать?
                // Обычно лучше перезаписать, если это новый приоритетный таргет.
                g_camAnimStart = g_camOffset; 
                g_camAnimTarget = {g_camOffset.x + dx, g_camOffset.y + dy};
                g_camAnimTime = std::chrono::steady_clock::now(); 
                g_isCamAnim.store(true);
                
                // Сбрасываем флаг после установки
                g_autoFocusTarget = NULL;
            } else {
                // Окно исчезло, сбрасываем
                g_autoFocusTarget = NULL;
            }
        }

        if (gridAnim) {
            auto animNow = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(animNow - g_gridAnim.startTime).count();
            float t = std::min(1.0f, (float)ms / g_gridAnim.durationMs);
            float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t); 
            
            ops.reserve(g_gridAnim.items.size());
            
            for (const auto& item : g_gridAnim.items) {
                if (!IsWindow(item.hwnd)) continue;
                
                int curX = (int)(item.startX + (item.endX - item.startX) * ease);
                int curY = (int)(item.startY + (item.endY - item.startY) * ease);
                
                ops.push_back({item.hwnd, curX, curY, item.width, item.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
            }
            
            if (t >= 1.0f) {
                g_gridAnim.active = false;
                
                // ВАЖНО: Мы не очищаем весь g_snapshots, а только добавляем новые окна.
                // Старые окна уже там есть и их трогать не надо.
                // Но так как у нас логика "полного пересчета" была в ArrangeGrid, а здесь инкрементальная,
                // нам нужно просто добавить новые окна в список.
                
                for (const auto& item : g_gridAnim.items) {
                     if (IsWindow(item.hwnd)) {
                         // ПРОВЕРКА: Есть ли уже это окно в списке? (На всякий случай)
                         bool exists = false;
                         for(const auto& s : g_snapshots) {
                             if(s.hwnd == item.hwnd) { exists = true; break; }
                         }

                         if(!exists) {
                             // КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ:
                             // item.endX/Y - это абсолютные экранные координаты.
                             // g_snapshots требует координаты ОТНОСИТЕЛЬНО камеры (base = real - offset).
                             int relativeX = item.endX - g_camOffset.x;
                             int relativeY = item.endY - g_camOffset.y;
                             
                             g_snapshots.push_back({item.hwnd, relativeX, relativeY, item.width, item.height});
                         }
                         
                         // Удаляем из списка новых окон
                         auto it = std::find(g_newWindowsFound.begin(), g_newWindowsFound.end(), item.hwnd);
                         if (it != g_newWindowsFound.end()) {
                             g_newWindowsFound.erase(it);
                         }
                     }
                }
                g_gridAnim.items.clear();
            }
        }
        else if (drag || camAnim) {
            ops.reserve(g_snapshots.size());
            POINT currentOffset = g_camOffset;

            if (camAnim) {
                auto animNow = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(animNow - g_camAnimTime).count();
                float t = std::min(1.0f, (float)ms / CAM_ANIM_DURATION);
                float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t);
                currentOffset.x = (int)(g_camAnimStart.x + (g_camAnimTarget.x - g_camAnimStart.x) * ease);
                currentOffset.y = (int)(g_camAnimStart.y + (g_camAnimTarget.y - g_camAnimStart.y) * ease);
                g_camOffset = currentOffset;
                
                // Рендерим отложенные окна (держим их на месте во время анимации камеры)
                for (const auto& pw : g_pendingWindows) {
                    if (!IsWindow(pw.hwnd)) continue;
                    RECT r;
                    if (GetWindowRect(pw.hwnd, &r)) {
                        // Держим окно в центре экрана во время анимации
                        int scX = GetSystemMetrics(SM_CXSCREEN)/2;
                        int scY = GetSystemMetrics(SM_CYSCREEN)/2;
                        int centerX = scX - pw.width/2;
                        int centerY = scY - pw.height/2;
                        ops.push_back({pw.hwnd, centerX, centerY, pw.width, pw.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
                    }
                }
                
                if (t >= 1.0f) {
                    g_isCamAnim.store(false);
                    
                    // Проверяем, есть ли отложенные окна для "посадки"
                    if (!g_pendingWindows.empty()) {
                        // Запускаем анимацию сетки для всех отложенных окон
                        g_gridAnim.items.clear();
                        g_gridAnim.startTime = std::chrono::steady_clock::now();
                        g_gridAnim.active = true;
                        
                        for (const auto& pw : g_pendingWindows) {
                            if (!IsWindow(pw.hwnd)) continue;
                            
                            RECT r;
                            if (GetWindowRect(pw.hwnd, &r)) {
                                GridAnimItem item;
                                item.hwnd = pw.hwnd;
                                item.startX = r.left;
                                item.startY = r.top;
                                item.endX = pw.targetAbsX;
                                item.endY = pw.targetAbsY;
                                item.width = pw.width;
                                item.height = pw.height;
                                g_gridAnim.items.push_back(item);
                            }
                        }
                        
                        g_pendingWindows.clear(); // Очищаем список после запуска анимации
                        g_autoCamAnim.store(false); // Разблокируем ввод
                    }
                }
            } 
            else if (drag) {
                POINT cur = g_currentMouse;
                POINT last = g_dragStartMouse;
                g_camOffset.x += (cur.x - last.x);
                g_camOffset.y += (cur.y - last.y);
                g_dragStartMouse = cur;
                currentOffset = g_camOffset;
            }

            for (auto& s : g_snapshots) {
                if (!IsWindow(s.hwnd)) continue;
                int nx = s.baseX + currentOffset.x;
                int ny = s.baseY + currentOffset.y;
                nx = std::max(-5000, std::min(nx, CANVAS_WIDTH+5000));
                ny = std::max(-5000, std::min(ny, CANVAS_HEIGHT+5000));
                ops.push_back({s.hwnd, nx, ny, s.width, s.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
            }
        }
        
        LeaveCriticalSection(&g_lock);
        
        if (!ops.empty()) ApplyMoves(ops);

        static int frameCount = 0;
        if (++frameCount % 8 == 0) UpdateDebugWindow();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_MS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// УПРАВЛЕНИЕ
// ═══════════════════════════════════════════════════════════════════════════════
void SnapToWindow(HWND target) {
    if (g_gridAnim.active) return;

    EnterCriticalSection(&g_lock);
    if (g_snapshots.empty()) { LeaveCriticalSection(&g_lock); return; }
    WindowSnapshot* found = nullptr;
    for (auto& s : g_snapshots) { if (s.hwnd == target) { found = &s; break; } }
    if (!found) {
        HWND root = GetAncestor(target, GA_ROOTOWNER);
        for (auto& s : g_snapshots) { if (s.hwnd == root) { found = &s; break; } }
    }
    if (found) {
        int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;
        g_camAnimStart = g_camOffset;
        g_camAnimTarget = { screenCx - (found->baseX + found->width/2), screenCy - (found->baseY + found->height/2) };
        g_camAnimTime = std::chrono::steady_clock::now();
        g_isCamAnim.store(true);
    }
    LeaveCriticalSection(&g_lock);
}

void FocusOnWindow(HWND target) {
    // Safety checks
    if (!target || !IsWindow(target)) return;
    if (g_gridAnim.active) return;

    EnterCriticalSection(&g_lock);

    RECT r;
    if (!GetWindowRect(target, &r)) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
    int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

    // Calculate window center in screen space
    int targetCenterX = r.left + (r.right - r.left) / 2;
    int targetCenterY = r.top + (r.bottom - r.top) / 2;

    // Calculate camera offset needed to center this window
    int dx = screenCx - targetCenterX;
    int dy = screenCy - targetCenterY;

    // Set up camera animation
    g_camAnimStart = g_camOffset;
    g_camAnimTarget = {g_camOffset.x + dx, g_camOffset.y + dy};
    g_camAnimTime = std::chrono::steady_clock::now();
    g_isCamAnim.store(true);

    LeaveCriticalSection(&g_lock);
}

void StartDrag(POINT p) {
    if (g_gridAnim.active) return;
    
    // 1. СИНХРОНИЗАЦИЯ: Обновляем координаты окон перед началом движения камеры
    EnterCriticalSection(&g_lock);
    for (auto& s : g_snapshots) {
        if (IsWindow(s.hwnd)) {
            RECT r;
            if (GetWindowRect(s.hwnd, &r)) {
                // Получаем текущие экранные координаты
                int realX = r.left;
                int realY = r.top;
                
                // Вычисляем новые базовые координаты относительно текущего смещения камеры
                // Формула: Base = Real - CameraOffset
                s.baseX = realX - g_camOffset.x;
                s.baseY = realY - g_camOffset.y;
                
                // Также на всякий случай обновляем размер, если окно изменилось
                s.width = r.right - r.left;
                s.height = r.bottom - r.top;
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    // 2. СТАРТ ДВИЖЕНИЯ
    g_isCamAnim.store(false);
    g_isDragging.store(true);
    g_dragStartMouse = p;
    g_currentMouse = p;
}

void Zoom(float scale) {
    if (g_gridAnim.active) return;

    EnterCriticalSection(&g_lock);
    POINT center = {GetSystemMetrics(SM_CXSCREEN)/2, GetSystemMetrics(SM_CYSCREEN)/2};
    if (g_snapshots.empty()) { LeaveCriticalSection(&g_lock); return; }
    std::vector<WindowMoveOp> ops;
    ops.reserve(g_snapshots.size());
    for (auto& s : g_snapshots) {
        if (!IsWindow(s.hwnd)) continue;
        int physX = s.baseX + g_camOffset.x;
        int physY = s.baseY + g_camOffset.y;
        int ncx = center.x + (int)((physX + s.width/2 - center.x) * scale);
        int ncy = center.y + (int)((physY + s.height/2 - center.y) * scale);
        int nw = std::max(100, (int)(s.width * scale));
        int nh = std::max(100, (int)(s.height * scale));
        ops.push_back({s.hwnd, ncx - nw/2, ncy - nh/2, nw, nh, SWP_NOZORDER|SWP_NOACTIVATE});
        s.baseX = (ncx - nw/2) - g_camOffset.x;
        s.baseY = (ncy - nh/2) - g_camOffset.y;
        s.width = nw; s.height = nh;
    }
    LeaveCriticalSection(&g_lock);
    ApplyMoves(ops);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ХУКИ
// ═══════════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK MouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    
    MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lParam;

    // ========================================================================
    // 1. РЕЖИМ ПРИВЯЗКИ КЛАВИШ
    // ========================================================================
    if (g_bindingMode) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN) {
            WPARAM newKey = 0;
            if (wParam == WM_LBUTTONDOWN) newKey = VK_LBUTTON;
            else if (wParam == WM_RBUTTONDOWN) newKey = VK_RBUTTON;
            else if (wParam == WM_MBUTTONDOWN) newKey = VK_MBUTTON;
            else if (wParam == WM_XBUTTONDOWN) newKey = (HIWORD(m->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            
            if (g_bindingPanKey) g_panKey = newKey; 
            else g_activateKey = newKey;
            
            g_bindingMode = false;
            if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
            return 1;
        }
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    // ========================================================================
    // 1.5. БЛОКИРОВКА ВВОДА ВО ВРЕМЯ АВТО-КАМЕРНОЙ АНИМАЦИИ
    // ========================================================================
    if (g_autoCamAnim.load()) {
        // Блокируем все события мыши во время авто-анимации камеры
        return 1;
    }

    // ========================================================================
    // 2. ГЛАВНАЯ ЛОГИКА: КОНЕЧНЫЙ АВТОМАТ
    // ========================================================================

    // --- СОСТОЯНИЕ 1: МЫ УЖЕ ТАЩИМ (DRAG ACTIVE) ---
    if (g_isDragging.load()) {
        // А. Обновляем координаты для рабочего потока
        if (!g_gridAnim.active) {
            g_currentMouse = m->pt;
        }

        // Б. Проверяем, отпустили ли мы ТУ САМУЮ кнопку, которой начали тянуть
        bool isStopEvent = false;
        WPARAM startBtn = g_panStartButton.load();

        if (wParam == WM_LBUTTONUP && startBtn == VK_LBUTTON) isStopEvent = true;
        else if (wParam == WM_RBUTTONUP && startBtn == VK_RBUTTON) isStopEvent = true;
        else if (wParam == WM_MBUTTONUP && startBtn == VK_MBUTTON) isStopEvent = true;
        else if (wParam == WM_XBUTTONUP) {
            WORD xBtn = HIWORD(m->mouseData);
            if ((startBtn == VK_XBUTTON1 && xBtn == XBUTTON1) || 
                (startBtn == VK_XBUTTON2 && xBtn == XBUTTON2)) {
                isStopEvent = true;
            }
        }

        // В. Если отпустили "ту самую" кнопку -> ЗАВЕРШАЕМ
        if (isStopEvent) {
            g_isDragging.store(false);
            g_panStartButton.store(0);
            return 1; // БЛОКИРУЕМ событие
        }

        // Г. Блокируем любые другие нажатия/отпускания во время драга
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN ||
            wParam == WM_LBUTTONUP || wParam == WM_RBUTTONUP || wParam == WM_MBUTTONUP || wParam == WM_XBUTTONUP) {
            return 1;
        }

        // Д. Пропускаем движение мыши и колесо
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    // --- СОСТОЯНИЕ 2: МЫ НЕ ТАЩИМ (IDLE) -> ПРОВЕРЯЕМ ЗАПУСК ---
    
    if (g_gridAnim.active) {
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    // Проверяем, является ли текущее событие НАЖАТИЕМ кнопки
    bool isPressEvent = false;
    WPARAM pressedKey = 0;

    if (wParam == WM_LBUTTONDOWN) { isPressEvent = true; pressedKey = VK_LBUTTON; }
    else if (wParam == WM_RBUTTONDOWN) { isPressEvent = true; pressedKey = VK_RBUTTON; }
    else if (wParam == WM_MBUTTONDOWN) { isPressEvent = true; pressedKey = VK_MBUTTON; }
    else if (wParam == WM_XBUTTONDOWN) { 
        isPressEvent = true; 
        pressedKey = (HIWORD(m->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; 
    }

    if (isPressEvent) {
        bool shouldStartDrag = false;

        // ЛОГИКА ЗАПУСКА (Два независимых условия):
        
        // 1. Нажата назначенная клавиша Pan Key (если она != 0)
        if (g_panKey != 0 && pressedKey == g_panKey) {
            shouldStartDrag = true;
        }
        
        // 2. Комбо: Нажата клавиша Активации (Ctrl) + СРЕДНЯЯ кнопка мыши
        // Работает ВСЕГДА, независимо от Pan Key
        if (pressedKey == VK_MBUTTON) {
            // Проверяем, зажата ли клавиша активации в момент нажатия средней кнопки
            bool isActivateHeld = false;
            if (g_activateKey != 0) {
                if (GetAsyncKeyState(g_activateKey) & 0x8000) isActivateHeld = true;
            }
            
            if (isActivateHeld) {
                shouldStartDrag = true;
            }
        }

        if (shouldStartDrag) {
            g_panStartButton.store(pressedKey); // Запоминаем, чем начали
            StartDrag(m->pt);                   // Инициализируем drag
            return 1;                           // Блокируем исходное нажатие
        }
    }

    // ========================================================================
    // 3. ОБРАБОТКА АКТИВАЦИИ (Зум и Фокус)
    // ========================================================================
    
    bool isActivateHeld = false;
    if (g_activateKey != 0) {
        if (GetAsyncKeyState(g_activateKey) & 0x8000) isActivateHeld = true;
    }

    if (isActivateHeld) {
        if (wParam == WM_LBUTTONDOWN) {
            HWND h = WindowFromPoint(m->pt);
            if (h) SnapToWindow(GetAncestor(h, GA_ROOTOWNER));
            return 1;
        }
        if (wParam == WM_MOUSEWHEEL) {
            Zoom(GET_WHEEL_DELTA_WPARAM(m->mouseData) > 0 ? 1.1f : 0.9f);
            return 1;
        }
    }

    // Пропускаем все остальные события
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK KbHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;

    if (g_bindingMode && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        if (k->vkCode != VK_LWIN && k->vkCode != VK_RWIN && k->vkCode != VK_APPS) {
            if (g_bindingPanKey) g_panKey = k->vkCode; else g_activateKey = k->vkCode;
            g_bindingMode = false;
            if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
            return 1;
        }
    }

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        if (CheckDoubleTap(k->vkCode, true)) { HandleDoubleTapReset(); return 1; }
        
        if (!g_gridAnim.active && k->vkCode == VK_NUMPAD5 && (GetAsyncKeyState(g_activateKey) & 0x8000)) {
            int scX = GetSystemMetrics(SM_CXSCREEN)/2, scY = GetSystemMetrics(SM_CYSCREEN)/2;
            long long minD = -1; HWND best = NULL;
            EnterCriticalSection(&g_lock);
            for (const auto& s : g_snapshots) {
                if (!IsWindow(s.hwnd)) continue;
                RECT r; if (!GetWindowRect(s.hwnd, &r)) continue;
                int cx = r.left + (r.right-r.left)/2, cy = r.top + (r.bottom-r.top)/2;
                long long d = 1LL*(cx-scX)*(cx-scX) + 1LL*(cy-scY)*(cy-scY);
                if (minD == -1 || d < minD) { minD = d; best = s.hwnd; }
            }
            LeaveCriticalSection(&g_lock);
            if (best) {
                FocusOnWindow(best);
                return 1;
            }
        }
    }
    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) CheckDoubleTap(k->vkCode, false);

    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CREATE) {
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_debugLock);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, NULL, 0);
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, NULL, 0);
        if (!g_mouseHook || !g_kbHook) { MessageBoxW(NULL, L"Hook Error", L"Error", MB_ICONERROR); return -1; }
        g_stop.store(false);
        g_worker = std::thread(WorkerFunc);
        return 0;
    }
    if (m == WM_DESTROY) {
        g_stop.store(true);
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
        g_mouseHook = NULL; g_kbHook = NULL;
        if (g_worker.joinable()) g_worker.join();
        DeleteCriticalSection(&g_lock);
        DeleteCriticalSection(&g_debugLock);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    if (!IsAdmin()) RunAsAdmin();
    WNDCLASSEXW wcMain = {sizeof(wcMain)};
    wcMain.lpfnWndProc = WndProc; wcMain.hInstance = h; wcMain.lpszClassName = L"CanvasDesk";
    RegisterClassExW(&wcMain);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW, L"CanvasDesk", L"", WS_POPUP, 0,0,1,1, NULL,NULL,h,NULL);
    if (!g_hwnd) return 1;
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOW);

    CreateDebugWindow(h);
    Sleep(100);
    ArrangeGrid();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}