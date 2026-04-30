#include <windows.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════════════════════
const int CANVAS_WIDTH  = 10000;
const int CANVAS_HEIGHT = 10000;
const WPARAM ACTIVATE_KEY = VK_RCONTROL;
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
HWND g_hwnd = NULL;       // Основное прозрачное окно (невидимое)
HWND g_debugHwnd = NULL;  // Окно отладки (видимое, часть матрицы)

// Система камеры
POINT g_camOffset = {0, 0};

// Система анимации
std::atomic<bool> g_isAnim(false);
POINT g_animStart = {0, 0};
POINT g_animTarget = {0, 0};
std::chrono::steady_clock::time_point g_animTime;
const int ANIM_DURATION = 400;

// Текст для отладки (буфер)
std::wstring g_debugText = L"";
CRITICAL_SECTION g_debugLock;

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

bool IsValidWnd(HWND h) {
    // 1. Игнорируем только основное невидимое окно-контейнер
    if (!h || h == g_hwnd) return false;
    
    // 2. Проверка видимости (для отладки можно показывать даже скрытые, но пока строго)
    if (!IsWindowVisible(h)) return false;

    // 3. Проверка на весь экран
    RECT r; 
    GetWindowRect(h, &r);
    int w = r.right - r.left;
    int hgt = r.bottom - r.top;
    
    if (w >= GetSystemMetrics(SM_CXSCREEN) && hgt >= GetSystemMetrics(SM_CYSCREEN)) 
        return false;

    // 4. Минимальный размер (отсекаем мелочь, но дебаг окно обычно большое)
    if (w < 100 || hgt < 50) return false;

    // 5. Проверка класса (исключаем системный мусор)
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 63);
    if (wcscmp(cls, L"Shell_TrayWnd")==0 || 
        wcscmp(cls, L"Progman")==0 || 
        wcscmp(cls, L"WorkerW")==0 ||
        wcscmp(cls, L"NotifyIconOverflowWindow")==0 ||
        wcscmp(cls, L"Microsoft::Windows::CUI::CICandidateWindow")==0)
        return false;

    // 6. Проверка расширенных стилей (исключаем тултипы)
    LONG exStyle = GetWindowLongW(h, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    // 7. Проверка обычного стиля
    LONG st = GetWindowLongW(h, GWL_STYLE);
    bool hasFrame = (st & WS_THICKFRAME) || (st & WS_CAPTION);
    bool isPopup = (st & WS_POPUP) != 0;
    
    if (!hasFrame && !isPopup) return false;

    // 8. ПРОВЕРКА ЗАГОЛОВКА
    // Окно отладки имеет заголовок "Debug Info", поэтому оно пройдет эту проверку
    wchar_t title[256] = {0};
    GetWindowTextW(h, title, 255);
    
    if (wcslen(title) == 0) {
        return false; 
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ОТЛАДОЧНОЕ ОКНО
// ═══════════════════════════════════════════════════════════════════════════════

void UpdateDebugWindow() {
    std::wstringstream ss;
    ss << L"=== DEBUG CONSOLE ===\n";
    ss << L"Cam Offset: X=" << g_camOffset.x << L" Y=" << g_camOffset.y << L"\n";
    ss << L"Animating: " << (g_isAnim.load() ? L"YES" : L"NO") << L"\n";
    ss << L"Windows Count: " << g_snapshots.size() << L"\n";
    ss << L"---------------------\n";

    EnterCriticalSection(&g_lock);
    int count = 0;
    for (const auto& s : g_snapshots) {
        if (!IsWindow(s.hwnd)) continue;
        if (count >= 10) { ss << L"... (too many)\n"; break; }

        RECT realRect;
        GetWindowRect(s.hwnd, &realRect);
        
        int calcX = s.baseX + g_camOffset.x;
        int calcY = s.baseY + g_camOffset.y;
        int diffX = realRect.left - calcX;
        int diffY = realRect.top - calcY;

        ss << L"[" << count << L"] Diff: (" << diffX << L", " << diffY << L")\n";
        ss << L"    Base: (" << s.baseX << L", " << s.baseY << L")\n";
        ss << L"    Real: (" << realRect.left << L", " << realRect.top << L")\n";
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
            SetWindowTextW(hwnd, L"Debug Info");
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);

            SetTextColor(hdc, RGB(0, 0, 0));
            SetBkMode(hdc, TRANSPARENT);
            
            HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

            EnterCriticalSection(&g_debugLock);
            std::wstring text = g_debugText;
            LeaveCriticalSection(&g_debugLock);

            DrawTextW(hdc, text.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK);

            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            g_debugHwnd = NULL; // Сбрасываем хендл
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
    wc.hbrBackground = (HBRUSH)(WHITE_BRUSH);
    
    if (!RegisterClassExW(&wc)) return;

    // Создаем обычное окно с заголовком и рамкой, чтобы оно попало в матрицу
    g_debugHwnd = CreateWindowExW(
        0, 
        L"CanvasDebugClass",
        L"Debug Info",
        WS_OVERLAPPEDWINDOW, // Рамка, заголовок, кнопки
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 600,
        NULL, NULL, hInst, NULL
    );
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

    int startX = (CANVAS_WIDTH/2) - (gW/2);
    int startY = (CANVAS_HEIGHT/2) - (gH/2);

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
    
    SnapshotCtx ctx;
    ctx.list = &g_snapshots;
    ctx.offset = g_camOffset;

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
        bool needMove = drag || anim;

        if (needMove) {
            std::vector<WindowMoveOp> ops;
            EnterCriticalSection(&g_lock);
            ops.reserve(g_snapshots.size());

            if (anim) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_animTime).count();
                float t = std::min(1.0f, (float)ms / ANIM_DURATION);
                float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t);

                int curX = (int)(g_animStart.x + (g_animTarget.x - g_animStart.x) * ease);
                int curY = (int)(g_animStart.y + (g_animTarget.y - g_animStart.y) * ease);

                g_camOffset.x = curX;
                g_camOffset.y = curY;

                for (auto& s : g_snapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    int nx = s.baseX + curX;
                    int ny = s.baseY + curY;
                    nx = std::max(-5000, std::min(nx, CANVAS_WIDTH+5000));
                    ny = std::max(-5000, std::min(ny, CANVAS_HEIGHT+5000));
                    ops.push_back({s.hwnd, nx, ny, s.width, s.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
                }

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

                for (auto& s : g_snapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    int nx = s.baseX + g_camOffset.x;
                    int ny = s.baseY + g_camOffset.y;
                    nx = std::max(-5000, std::min(nx, CANVAS_WIDTH+5000));
                    ny = std::max(-5000, std::min(ny, CANVAS_HEIGHT+5000));
                    ops.push_back({s.hwnd, nx, ny, s.width, s.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
                }
            }
            LeaveCriticalSection(&g_lock);
            if (!ops.empty()) ApplyMoves(ops);
        }

        static int frameCount = 0;
        if (++frameCount % 10 == 0) {
            UpdateDebugWindow();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_MS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ЛОГИКА УПРАВЛЕНИЯ
// ═══════════════════════════════════════════════════════════════════════════════

void SnapToWindow(HWND target) {
    EnterCriticalSection(&g_lock);
    if (g_snapshots.empty()) {
        LeaveCriticalSection(&g_lock);
        TakeSnapshot();
        EnterCriticalSection(&g_lock);
    }

    WindowSnapshot* found = nullptr;
    for (auto& s : g_snapshots) {
        if (s.hwnd == target) { found = &s; break; }
    }
    if (!found) {
        HWND root = GetAncestor(target, GA_ROOTOWNER);
        for (auto& s : g_snapshots) {
            if (s.hwnd == root) { found = &s; break; }
        }
    }

    if (found) {
        int winCx = found->baseX + g_camOffset.x + found->width / 2;
        int winCy = found->baseY + g_camOffset.y + found->height / 2;
        int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

        int targetOffX = screenCx - (found->baseX + found->width / 2);
        int targetOffY = screenCy - (found->baseY + found->height / 2);

        g_animStart = g_camOffset;
        g_animTarget.x = targetOffX;
        g_animTarget.y = targetOffY;
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
    
    if (g_snapshots.empty()) {
        LeaveCriticalSection(&g_lock);
        TakeSnapshot();
        EnterCriticalSection(&g_lock);
    }

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
    bool active = (GetAsyncKeyState(ACTIVATE_KEY) & 0x8000) != 0;
    
    if (!active) {
        if (g_isDragging.load()) g_isDragging.store(false);
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }
    
    g_currentMouse.x = m->pt.x;
    g_currentMouse.y = m->pt.y;
    
    if (wParam == WM_LBUTTONDOWN) {
        HWND h = WindowFromPoint(m->pt);
        if (h) SnapToWindow(GetAncestor(h, GA_ROOTOWNER));
        return 1;
    }
    
    if (wParam == WM_MBUTTONDOWN) {
        StartDrag(m->pt);
        return 1;
    }
    
    if (wParam == WM_MBUTTONUP) {
        g_isDragging.store(false);
        return 1;
    }
    
    if (wParam == WM_MOUSEWHEEL) {
        short d = GET_WHEEL_DELTA_WPARAM(m->mouseData);
        Zoom(d > 0 ? 1.1f : 0.9f);
        g_dragStartMouse = m->pt;
        return 1;
    }
    
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK KbHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
    
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
        
        if (k->vkCode == VK_NUMPAD5 && (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0) {
            int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
            int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

            long long minDistSq = -1;
            HWND bestHwnd = NULL;
            RECT bestRect = {0};

            EnterCriticalSection(&g_lock);
            if (!g_snapshots.empty()) {
                for (const auto& s : g_snapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    RECT realRect;
                    if (!GetWindowRect(s.hwnd, &realRect)) continue;

                    int winCx = realRect.left + (realRect.right - realRect.left) / 2;
                    int winCy = realRect.top + (realRect.bottom - realRect.top) / 2;

                    long long dx = winCx - screenCx;
                    long long dy = winCy - screenCy;
                    long long distSq = dx * dx + dy * dy;

                    if (minDistSq == -1 || distSq < minDistSq) {
                        minDistSq = distSq;
                        bestHwnd = s.hwnd;
                        bestRect = realRect;
                    }
                }
            }
            LeaveCriticalSection(&g_lock);

            if (bestHwnd) {
                int winW = bestRect.right - bestRect.left;
                int winH = bestRect.bottom - bestRect.top;
                int realCenterX = bestRect.left + winW / 2;
                int realCenterY = bestRect.top + winH / 2;

                int deltaX = screenCx - realCenterX;
                int deltaY = screenCy - realCenterY;

                g_animStart = g_camOffset;
                g_animTarget.x = g_camOffset.x + deltaX;
                g_animTarget.y = g_camOffset.y + deltaY;
                
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
        
        if (!g_mouseHook || !g_kbHook) {
            MessageBoxW(NULL, L"Hook Error", L"Error", MB_ICONERROR);
            return -1;
        }
        
        g_stop.store(false);
        g_worker = std::thread(WorkerFunc);
        
        Sleep(300);
        // ArrangeGrid вызовется после создания дебаг окна в WinMain
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

    // 1. Сначала создаем окно отладки
    CreateDebugWindow(h);
    
    // Небольшая пауза, чтобы окно точно создалось и стало видимым
    Sleep(100);

    // 2. Теперь запускаем цикл сообщений основного окна, внутри которого при WM_CREATE
    // вызывается ArrangeGrid(). Так как дебаг окно уже создано и видимо, 
    // IsValidWnd пропустит его, и оно попадет в матрицу.
    
    // Примечание: ArrangeGrid вызывается в WndProc при WM_CREATE. 
    // Чтобы гарантировать порядок, можно вызвать ArrangeGrid явно здесь после создания дебага,
    // но так как g_worker еще не запущен (ждет WM_CREATE), лучше оставить логику там.
    // Однако, чтобы быть уверенным, что ArrangeGrid увидит новое окно, 
    // мы можем просто подождать немного перед стартом цикла сообщений, 
    // но WM_CREATE уже отработал при создании g_hwnd.
    
    // Исправление порядка:
    // g_hwnd создан -> WM_CREATE отработал (хуки установлены, поток запущен).
    // Но ArrangeGrid вызван ДО CreateDebugWindow в предыдущей версии.
    // Теперь нам нужно вызвать ArrangeGrid ЕЩЕ РАЗ вручную после создания дебаг окна.
    
    ArrangeGrid(); // Принудительная перерасстановка с учетом нового окна

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}