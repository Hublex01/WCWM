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

// ═══════════════════════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════════════════════
const int CANVAS_WIDTH  = 10000;
const int CANVAS_HEIGHT = 10000;

// Глобальные переменные для клавиш
WPARAM g_activateKey = VK_RCONTROL;  // Ключ активации (Зум + Клик по окну)
WPARAM g_panKey = 0;                 // Ключ панорамирования (Перетаскивание холста)

const int THREAD_SLEEP_MS = 8; 

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

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ═══════════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_isDragging(false);
POINT g_dragStartMouse = {0, 0};
POINT g_currentMouse = {0, 0};
std::vector<WindowSnapshot> g_snapshots;

CRITICAL_SECTION g_lock;
std::thread g_worker;
std::atomic<bool> g_stop(false);

HHOOK g_mouseHook = NULL;
HHOOK g_kbHook = NULL;
HWND g_hwnd = NULL;       // Основное прозрачное окно
HWND g_debugHwnd = NULL;  // Окно отладки

POINT g_camOffset = {0, 0};

std::atomic<bool> g_isAnim(false);
POINT g_animStart = {0, 0};
POINT g_animTarget = {0, 0};
std::chrono::steady_clock::time_point g_animTime;
const int ANIM_DURATION = 400;

std::wstring g_debugText = L"";
CRITICAL_SECTION g_debugLock;

// Double-Tap Globals (для сброса сетки)
DWORD g_lastActivateKeyPress = 0;
DWORD g_lastActivateKeyRelease = 0;
bool g_wasActivateKeyDown = false;
bool g_doubleTapHandled = false;
const DWORD DOUBLE_TAP_TIMEOUT = 300;
const DWORD HOLD_THRESHOLD = 200;

// Key Binding Globals
bool g_bindingMode = false;
bool g_bindingPanKey = false;  // true = binding pan key, false = binding activate key
RECT g_btnBindRect = {0};      // Координаты кнопки активации
RECT g_btnPanRect = {0};       // Координаты кнопки панорамирования

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

// Получение читаемого имени клавиши
std::wstring GetKeyNameStr(WPARAM vkCode) {
    if (vkCode == 0) return L"None (Not Set)";
    
    switch (vkCode) {
        case VK_LBUTTON: return L"Left Mouse Button";
        case VK_RBUTTON: return L"Right Mouse Button";
        case VK_MBUTTON: return L"Middle Mouse Button";
        case VK_XBUTTON1: return L"Mouse X1 (Back)";
        case VK_XBUTTON2: return L"Mouse X2 (Forward)";
    }

    LONG scanCode = MapVirtualKeyExW((UINT)vkCode, 0, GetKeyboardLayout(0));
    LONG lParam = (scanCode << 16);
    wchar_t name[64] = {0};
    
    if (GetKeyNameTextW(lParam, name, 63) && wcslen(name) > 0) {
        return std::wstring(name);
    }

    switch (vkCode) {
        case VK_SPACE: return L"Space";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Escape";
        case VK_CONTROL: return L"Ctrl";
        case VK_MENU: return L"Alt";
        case VK_SHIFT: return L"Shift";
        case VK_LCONTROL: return L"Left Ctrl";
        case VK_RCONTROL: return L"Right Ctrl";
        case VK_LSHIFT: return L"Left Shift";
        case VK_RSHIFT: return L"Right Shift";
        case VK_LMENU: return L"Left Alt";
        case VK_RMENU: return L"Right Alt";
        case VK_NUMPAD0: return L"Numpad 0";
        case VK_NUMPAD1: return L"Numpad 1";
        case VK_NUMPAD2: return L"Numpad 2";
        case VK_NUMPAD3: return L"Numpad 3";
        case VK_NUMPAD4: return L"Numpad 4";
        case VK_NUMPAD5: return L"Numpad 5";
        case VK_NUMPAD6: return L"Numpad 6";
        case VK_NUMPAD7: return L"Numpad 7";
        case VK_NUMPAD8: return L"Numpad 8";
        case VK_NUMPAD9: return L"Numpad 9";
        default:
            if (vkCode >= '0' && vkCode <= '9') return std::wstring(1, (wchar_t)vkCode);
            if (vkCode >= 'A' && vkCode <= 'Z') return std::wstring(1, (wchar_t)vkCode);
            if (vkCode >= VK_F1 && vkCode <= VK_F12) return L"F" + std::to_wstring(vkCode - VK_F1 + 1);
            return L"Key 0x" + std::to_wstring(vkCode);
    }
}

bool IsValidWnd(HWND h) {
    if (!h || h == g_hwnd) return false;
    if (!IsWindowVisible(h)) return false;

    RECT r; GetWindowRect(h, &r);
    int w = r.right - r.left;
    int hgt = r.bottom - r.top;
    
    if (w >= GetSystemMetrics(SM_CXSCREEN) && hgt >= GetSystemMetrics(SM_CYSCREEN)) return false;
    if (w < 100 || hgt < 50) return false;

    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 63);
    if (wcscmp(cls, L"Shell_TrayWnd")==0 || wcscmp(cls, L"Progman")==0 || 
        wcscmp(cls, L"WorkerW")==0 || wcscmp(cls, L"NotifyIconOverflowWindow")==0 ||
        wcscmp(cls, L"Microsoft::Windows::CUI::CICandidateWindow")==0) return false;

    LONG exStyle = GetWindowLongW(h, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    LONG st = GetWindowLongW(h, GWL_STYLE);
    bool hasFrame = (st & WS_THICKFRAME) || (st & WS_CAPTION);
    bool isPopup = (st & WS_POPUP) != 0;
    if (!hasFrame && !isPopup) return false;

    wchar_t title[256] = {0};
    GetWindowTextW(h, title, 255);
    if (wcslen(title) == 0) return false;

    return true;
}

void ArrangeGrid();
void TakeSnapshot();

// ═══════════════════════════════════════════════════════════════════════════════
// ЛОГИКА ДВОЙНОГО НАЖАТИЯ
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
    ss << L"Camera: X=" << g_camOffset.x << L" Y=" << g_camOffset.y << L"\n";
    ss << L"Windows Managed: " << g_snapshots.size() << L"\n";
    ss << L"---------------------\n";
    ss << L"Activate Key: " << GetKeyNameStr(g_activateKey) << L"\n";
    ss << L"Pan Key:      " << GetKeyNameStr(g_panKey) << L"\n";
    
    if (g_bindingMode) {
        ss << L"\n>>> WAITING FOR INPUT... <<<\n";
        ss << (g_bindingPanKey ? L"(Setting PAN key)" : L"(Setting ACTIVATE key)") << L"\n";
    }
    
    ss << L"---------------------\n";
    ss << L"WINDOW LIST:\n";

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
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_btnBindRect = { rc.right - 170, 10, rc.right - 10, 40 };
            g_btnPanRect = { rc.right - 170, 45, rc.right - 10, 75 };
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            
            if (x >= g_btnBindRect.left && x <= g_btnBindRect.right &&
                y >= g_btnBindRect.top && y <= g_btnBindRect.bottom) {
                g_bindingMode = true;
                g_bindingPanKey = false;
                if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
                return 0;
            }
            if (x >= g_btnPanRect.left && x <= g_btnPanRect.right &&
                y >= g_btnPanRect.top && y <= g_btnPanRect.bottom) {
                g_bindingMode = true;
                g_bindingPanKey = true;
                if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
                return 0;
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            HBRUSH hBrush = CreateSolidBrush(RGB(240, 240, 240));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);

            COLORREF btnColor1 = (g_bindingMode && !g_bindingPanKey) ? RGB(255, 200, 200) : RGB(200, 220, 255);
            HBRUSH hBtnBrush = CreateSolidBrush(btnColor1);
            FillRect(hdc, &g_btnBindRect, hBtnBrush);
            FrameRect(hdc, &g_btnBindRect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            DeleteObject(hBtnBrush);

            COLORREF btnColor2 = (g_bindingMode && g_bindingPanKey) ? RGB(255, 200, 200) : RGB(200, 255, 200);
            hBtnBrush = CreateSolidBrush(btnColor2);
            FillRect(hdc, &g_btnPanRect, hBtnBrush);
            FrameRect(hdc, &g_btnPanRect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            DeleteObject(hBtnBrush);

            SetBkMode(hdc, TRANSPARENT);
            HFONT hFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
            SetTextColor(hdc, RGB(0, 0, 0));
            
            const wchar_t* btnText1 = (g_bindingMode && !g_bindingPanKey) ? L"LISTENING..." : L"Set Activate Key";
            DrawTextW(hdc, btnText1, -1, &g_btnBindRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            const wchar_t* btnText2 = (g_bindingMode && g_bindingPanKey) ? L"LISTENING..." : L"Set Pan Key";
            DrawTextW(hdc, btnText2, -1, &g_btnPanRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);

            HFONT hConsolas = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            SelectObject(hdc, hConsolas);
            
            RECT textRect = { 10, 85, rc.right - 10, rc.bottom - 10 };
            EnterCriticalSection(&g_debugLock);
            std::wstring text = g_debugText;
            LeaveCriticalSection(&g_debugLock);
            
            DrawTextW(hdc, text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
            
            DeleteObject(hConsolas);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            g_debugHwnd = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateDebugWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DebugWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CanvasDebugClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    
    if (!RegisterClassExW(&wc)) return;

    g_debugHwnd = CreateWindowExW(0, L"CanvasDebugClass", L"WCWM", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 450, 650, NULL, NULL, hInst, NULL);
    if (g_debugHwnd) ShowWindow(g_debugHwnd, SW_SHOW);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ДВИЖОК ОКОН
// ═══════════════════════════════════════════════════════════════════════════════
void ApplyMoves(const std::vector<WindowMoveOp>& ops) {
    if (ops.empty()) return;
    HDWP h = BeginDeferWindowPos((int)ops.size());
    if (!h) {
        for (auto& o : ops) SetWindowPos(o.hwnd, 0, o.x, o.y, o.w, o.h, o.flags);
        return;
    }
    for (auto& o : ops) {
        h = DeferWindowPos(h, o.hwnd, 0, o.x, o.y, o.w, o.h, o.flags);
        if (!h) { EndDeferWindowPos(h); return; }
    }
    EndDeferWindowPos(h);
}

void ArrangeGrid() {
    std::vector<WindowSnapshot> list;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!IsValidWnd(h)) return TRUE;
        RECT r; GetWindowRect(h, &r);
        auto* v = (std::vector<WindowSnapshot>*)l;
        v->push_back({h, r.left, r.top, r.right-r.left, r.bottom-r.top});
        return TRUE;
    }, (LPARAM)&list);

    if (list.empty()) return;

    const int COLS = 3, PAD = 10;
    int rows = (list.size() + COLS - 1) / COLS;
    std::vector<int> cW(COLS, 0), rH(rows, 0);

    for (size_t i=0; i<list.size(); ++i) {
        cW[i%COLS] = std::max(cW[i%COLS], list[i].width);
        rH[i/COLS] = std::max(rH[i/COLS], list[i].height);
    }

    int gW = 0, gH = 0;
    for(int x:cW) gW+=x; for(int y:rH) gH+=y;
    gW += (COLS-1)*PAD; gH += (rows-1)*PAD;

    int startX = 0; 
    int startY = 0;

    std::vector<WindowMoveOp> ops;
    int cy = startY;
    for (int r=0; r<rows; ++r) {
        int cx = startX;
        for (int c=0; c<COLS; ++c) {
            int idx = r*COLS+c;
            if (idx >= (int)list.size()) break;
            ops.push_back({list[idx].hwnd, cx, cy, list[idx].width, list[idx].height, SWP_NOZORDER|SWP_NOACTIVATE});
            cx += cW[c] + PAD;
        }
        cy += rH[r] + PAD;
    }
    ApplyMoves(ops);
    g_camOffset = {0, 0};
}

void TakeSnapshot() {
    EnterCriticalSection(&g_lock);
    g_snapshots.clear();
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
// РАБОЧИЙ ПОТОК
// ═══════════════════════════════════════════════════════════════════════════════
void WorkerFunc() {
    while (!g_stop.load()) {
        bool drag = g_isDragging.load();
        bool anim = g_isAnim.load();
        
        if (drag || anim) {
            std::vector<WindowMoveOp> ops;
            EnterCriticalSection(&g_lock);
            ops.reserve(g_snapshots.size());

            POINT currentOffset = g_camOffset;

            if (anim) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_animTime).count();
                float t = std::min(1.0f, (float)ms / ANIM_DURATION);
                float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t);
                currentOffset.x = (int)(g_animStart.x + (g_animTarget.x - g_animStart.x) * ease);
                currentOffset.y = (int)(g_animStart.y + (g_animTarget.y - g_animStart.y) * ease);
                g_camOffset = currentOffset;
                if (t >= 1.0f) g_isAnim.store(false);
            } 
            else if (drag) {
                POINT cur = g_currentMouse;
                POINT last = g_dragStartMouse;
                int dx = cur.x - last.x;
                int dy = cur.y - last.y;
                g_camOffset.x += dx;
                g_camOffset.y += dy;
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
            LeaveCriticalSection(&g_lock);
            if (!ops.empty()) ApplyMoves(ops);
        }

        static int frameCount = 0;
        if (++frameCount % 10 == 0) UpdateDebugWindow();
        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_MS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ФУНКЦИИ УПРАВЛЕНИЯ
// ═══════════════════════════════════════════════════════════════════════════════
void SnapToWindow(HWND target) {
    EnterCriticalSection(&g_lock);
    if (g_snapshots.empty()) { LeaveCriticalSection(&g_lock); TakeSnapshot(); EnterCriticalSection(&g_lock); }
    
    WindowSnapshot* found = nullptr;
    for (auto& s : g_snapshots) { if (s.hwnd == target) { found = &s; break; } }
    if (!found) {
        HWND root = GetAncestor(target, GA_ROOTOWNER);
        for (auto& s : g_snapshots) { if (s.hwnd == root) { found = &s; break; } }
    }

    if (found) {
        int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;
        int targetOffX = screenCx - (found->baseX + found->width / 2);
        int targetOffY = screenCy - (found->baseY + found->height / 2);

        g_animStart = g_camOffset;
        g_animTarget = {targetOffX, targetOffY};
        g_animTime = std::chrono::steady_clock::now();
        g_isAnim.store(true);
    }
    LeaveCriticalSection(&g_lock);
}

void StartDrag(POINT p) {
    g_isAnim.store(false);
    g_isDragging.store(true);
    g_dragStartMouse = p;
    g_currentMouse = p;
    TakeSnapshot();
}

void Zoom(float scale) {
    EnterCriticalSection(&g_lock);
    POINT center = {GetSystemMetrics(SM_CXSCREEN)/2, GetSystemMetrics(SM_CYSCREEN)/2};
    if (g_snapshots.empty()) { LeaveCriticalSection(&g_lock); TakeSnapshot(); EnterCriticalSection(&g_lock); }

    std::vector<WindowMoveOp> ops;
    for (auto& s : g_snapshots) {
        if (!IsWindow(s.hwnd)) continue;
        int physX = s.baseX + g_camOffset.x;
        int physY = s.baseY + g_camOffset.y;
        int cx = physX + s.width/2;
        int cy = physY + s.height/2;
        int ncx = center.x + (int)((cx - center.x) * scale);
        int ncy = center.y + (int)((cy - center.y) * scale);
        int nw = std::max(100, (int)(s.width * scale));
        int nh = std::max(100, (int)(s.height * scale));
        int nx = ncx - nw/2;
        int ny = ncy - nh/2;
        ops.push_back({s.hwnd, nx, ny, nw, nh, SWP_NOZORDER|SWP_NOACTIVATE});
        s.baseX = nx - g_camOffset.x;
        s.baseY = ny - g_camOffset.y;
        s.width = nw;
        s.height = nh;
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

    // 1. ОБРАБОТКА РЕЖИМА ПРИВЯЗКИ КЛАВИШ
    if (g_bindingMode) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN) {
            WPARAM newKey = 0;
            if (wParam == WM_LBUTTONDOWN) newKey = VK_LBUTTON;
            else if (wParam == WM_RBUTTONDOWN) newKey = VK_RBUTTON;
            else if (wParam == WM_MBUTTONDOWN) newKey = VK_MBUTTON;
            else if (wParam == WM_XBUTTONDOWN) {
                newKey = (HIWORD(m->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            }

            if (g_bindingPanKey) {
                g_panKey = newKey;
            } else {
                g_activateKey = newKey;
            }
            
            g_bindingMode = false;
            if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
            return 1; 
        }
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    // 2. ПРОВЕРКА СОСТОЯНИЯ КЛАВИШ
    bool isPanActive = false;
    bool isActivateActive = false;
    bool isMiddleMouseDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

    // Проверка Pan Key
    if (g_panKey >= VK_LBUTTON && g_panKey <= VK_XBUTTON2) {
        if (g_panKey == VK_LBUTTON && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) isPanActive = true;
        if (g_panKey == VK_RBUTTON && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) isPanActive = true;
        if (g_panKey == VK_MBUTTON && (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) isPanActive = true;
        if (g_panKey == VK_XBUTTON1 && (GetAsyncKeyState(VK_XBUTTON1) & 0x8000)) isPanActive = true;
        if (g_panKey == VK_XBUTTON2 && (GetAsyncKeyState(VK_XBUTTON2) & 0x8000)) isPanActive = true;
    } else if (g_panKey != 0) {
        if (GetAsyncKeyState(g_panKey) & 0x8000) isPanActive = true;
    }

    // Проверка Activate Key
    if (g_activateKey >= VK_LBUTTON && g_activateKey <= VK_XBUTTON2) {
        if (g_activateKey == VK_LBUTTON && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) isActivateActive = true;
        if (g_activateKey == VK_RBUTTON && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) isActivateActive = true;
        if (g_activateKey == VK_MBUTTON && (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) isActivateActive = true;
        if (g_activateKey == VK_XBUTTON1 && (GetAsyncKeyState(VK_XBUTTON1) & 0x8000)) isActivateActive = true;
        if (g_activateKey == VK_XBUTTON2 && (GetAsyncKeyState(VK_XBUTTON2) & 0x8000)) isActivateActive = true;
    } else if (g_activateKey != 0) {
        if (GetAsyncKeyState(g_activateKey) & 0x8000) isActivateActive = true;
    }

    // 3. ЛОГИКА ПАНОРАМИРОВАНИЯ (PAN)
    // Работает если: нажат PanKey ИЛИ (нажат ActivateKey + Средняя кнопка)
    bool shouldPan = isPanActive || (isActivateActive && isMiddleMouseDown);

    if (shouldPan) {
        if (!g_isDragging.load()) {
            StartDrag(m->pt);
        }
        g_currentMouse = m->pt;

        // Блокируем клики, чтобы не сработал SnapToWindow во время перетаскивания
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN) {
             return 1; 
        }
        
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    } else {
        if (g_isDragging.load()) {
            g_isDragging.store(false);
        }
    }

    // 4. ЛОГИКА АКТИВАЦИИ (ACTIVATE)
    // Работает ТОЛЬКО если активен ActivateKey и НЕ активен режим панорамирования
    if (isActivateActive && !shouldPan) {
        g_currentMouse = m->pt;

        // Клик по окну (SnapToWindow)
        if (wParam == WM_LBUTTONDOWN) {
            HWND h = WindowFromPoint(m->pt);
            if (h) SnapToWindow(GetAncestor(h, GA_ROOTOWNER));
            return 1;
        }
        
        // Зум (ТОЛЬКО от ActivateKey)
        if (wParam == WM_MOUSEWHEEL) {
            short delta = GET_WHEEL_DELTA_WPARAM(m->mouseData);
            Zoom(delta > 0 ? 1.1f : 0.9f);
            g_dragStartMouse = m->pt;
            return 1;
        }
    }

    // Запасной вариант: средняя кнопка для драга, если PanKey не назначен
    if (g_panKey == 0 && wParam == WM_MBUTTONDOWN && !isActivateActive) {
        StartDrag(m->pt);
        return 1;
    }
    if (g_panKey == 0 && wParam == WM_MBUTTONUP) {
        g_isDragging.store(false);
        return 1;
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK KbHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;

    if (g_bindingMode && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        if (k->vkCode != VK_LWIN && k->vkCode != VK_RWIN && k->vkCode != VK_APPS) {
            if (g_bindingPanKey) {
                g_panKey = k->vkCode;
            } else {
                g_activateKey = k->vkCode;
            }
            g_bindingMode = false;
            if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, TRUE);
            return 1;
        }
    }

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        if (CheckDoubleTap(k->vkCode, true)) {
            HandleDoubleTapReset();
            return 1;
        }
    }
    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        CheckDoubleTap(k->vkCode, false);
    }

    if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && k->vkCode == VK_NUMPAD5) {
        if (GetAsyncKeyState(g_activateKey) & 0x8000) {
            int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
            int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;
            long long minDistSq = -1;
            HWND bestHwnd = NULL;
            RECT bestRect = {0};

            EnterCriticalSection(&g_lock);
            for (const auto& s : g_snapshots) {
                if (!IsWindow(s.hwnd)) continue;
                RECT realRect;
                if (!GetWindowRect(s.hwnd, &realRect)) continue;
                int winCx = realRect.left + (realRect.right - realRect.left) / 2;
                int winCy = realRect.top + (realRect.bottom - realRect.top) / 2;
                long long dx = winCx - screenCx;
                long long dy = winCy - screenCy;
                long long distSq = dx*dx + dy*dy;
                if (minDistSq == -1 || distSq < minDistSq) {
                    minDistSq = distSq; bestHwnd = s.hwnd; bestRect = realRect;
                }
            }
            LeaveCriticalSection(&g_lock);

            if (bestHwnd) {
                int deltaX = screenCx - (bestRect.left + (bestRect.right - bestRect.left)/2);
                int deltaY = screenCy - (bestRect.top + (bestRect.bottom - bestRect.top)/2);
                g_animStart = g_camOffset;
                g_animTarget = {g_camOffset.x + deltaX, g_camOffset.y + deltaY};
                g_animTime = std::chrono::steady_clock::now();
                g_isAnim.store(true);
                return 1;
            }
        }
    }

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
        Sleep(300);
        return 0;
    }
    if (m == WM_DESTROY) {
        g_stop.store(true);
        if (g_worker.joinable()) g_worker.join();
        UnhookWindowsHookEx(g_mouseHook);
        UnhookWindowsHookEx(g_kbHook);
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
    wcMain.lpfnWndProc = WndProc;
    wcMain.hInstance = h;
    wcMain.lpszClassName = L"CanvasDesk";
    RegisterClassExW(&wcMain);
    
    g_hwnd = CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
                             L"CanvasDesk", L"", WS_POPUP, 0,0,1,1, NULL,NULL,h,NULL);
    if (!g_hwnd) return 1;
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOW);

    CreateDebugWindow(h);
    Sleep(100);
    ArrangeGrid();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}