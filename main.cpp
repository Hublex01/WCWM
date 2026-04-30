#include <windows.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>

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
    int  baseX, baseY; // Позиция относительно начала холста (без учета камеры)
    int  width, height;
};

struct WindowMoveOp {
    HWND hwnd;
    int x, y, w, h;
    UINT flags;
};

// Контекст для безопасного снимка
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
HWND g_hwnd = NULL;

// Система камеры
POINT g_camOffset = {0, 0}; // Текущее смещение камеры

// Система анимации
std::atomic<bool> g_isAnim(false);
POINT g_animStart = {0, 0};
POINT g_animTarget = {0, 0};
std::chrono::steady_clock::time_point g_animTime;
const int ANIM_DURATION = 400; // мс

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
    if (!h || h == g_hwnd || !IsWindowVisible(h)) return false;
    RECT r; GetWindowRect(h, &r);
    if ((r.right - r.left >= GetSystemMetrics(SM_CXSCREEN)) && 
        (r.bottom - r.top >= GetSystemMetrics(SM_CYSCREEN))) return false;
    
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 63);
    if (wcscmp(cls, L"Shell_TrayWnd")==0 || wcscmp(cls, L"Progman")==0 || wcscmp(cls, L"WorkerW")==0) return false;
    
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (!(st & WS_THICKFRAME) && !(st & WS_CAPTION) && !(st & WS_POPUP)) return false;
    return true;
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
    g_camOffset = {0, 0}; // Сброс камеры в 0 после расстановки
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
        // Сохраняем БАЗОВУЮ позицию: Физическая - СмещениеКамеры
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
                // EaseOutCubic
                float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t);

                int curX = (int)(g_animStart.x + (g_animTarget.x - g_animStart.x) * ease);
                int curY = (int)(g_animStart.y + (g_animTarget.y - g_animStart.y) * ease);

                g_camOffset.x = curX;
                g_camOffset.y = curY;

                for (auto& s : g_snapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    int nx = s.baseX + curX;
                    int ny = s.baseY + curY;
                    // Мягкие границы
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
                g_dragStartMouse = cur; // Сброс дельты

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
        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_MS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ЛОГИКА УПРАВЛЕНИЯ
// ═══════════════════════════════════════════════════════════════════════════════

// Функция запуска анимации к конкретному окну
void SnapToWindow(HWND target) {
    EnterCriticalSection(&g_lock);
    
    // Обновляем снапшот прямо перед расчетом, чтобы базы были актуальны
    // Но делаем это аккуратно, чтобы не потерять текущее состояние, если список пуст
    if (g_snapshots.empty()) {
        LeaveCriticalSection(&g_lock);
        TakeSnapshot();
        EnterCriticalSection(&g_lock);
    }

    WindowSnapshot* found = nullptr;
    for (auto& s : g_snapshots) {
        if (s.hwnd == target) { found = &s; break; }
    }
    // Поиск родителя если не нашли
    if (!found) {
        HWND root = GetAncestor(target, GA_ROOTOWNER);
        for (auto& s : g_snapshots) {
            if (s.hwnd == root) { found = &s; break; }
        }
    }

    if (found) {
        // Физический центр окна = База + ТекущаяКамера + ПоловинаРазмера
        int winCx = found->baseX + g_camOffset.x + found->width / 2;
        int winCy = found->baseY + g_camOffset.y + found->height / 2;

        int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

        // Целевая камера должна быть такой, чтобы центр окна совпал с центром экрана
        // TargetOffset = ScreenCenter - (Base + Size/2)
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
    g_isAnim.store(false); // Стоп анимация
    g_isDragging.store(true);
    g_dragStartMouse = p;
    g_currentMouse = p;
    TakeSnapshot(); // Фиксируем базы относительно текущей камеры
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
        
        // Обновляем базу: НоваяФизическая - ТекущаяКамера
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
        // ЛКМ: Полет к окну
        HWND h = WindowFromPoint(m->pt);
        if (h) SnapToWindow(GetAncestor(h, GA_ROOTOWNER));
        return 1;
    }
    
    if (wParam == WM_MBUTTONDOWN) {
        // СКМ: Драг
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
        if (k->vkCode == VK_NUMPAD5) {
            if ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0) {
                EnterCriticalSection(&g_lock);
                if (g_snapshots.empty()) {
                    LeaveCriticalSection(&g_lock);
                    TakeSnapshot();
                    EnterCriticalSection(&g_lock);
                }

                int bestDist = INT_MAX;
                WindowSnapshot* best = nullptr;
                int scX = GetSystemMetrics(SM_CXSCREEN)/2;
                int scY = GetSystemMetrics(SM_CYSCREEN)/2;

                for (auto& s : g_snapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    int cx = s.baseX + g_camOffset.x + s.width/2;
                    int cy = s.baseY + g_camOffset.y + s.height/2;
                    int d = (cx-scX)*(cx-scX) + (cy-scY)*(cy-scY);
                    if (d < bestDist) { bestDist = d; best = &s; }
                }

                if (best) {
                    int targetOffX = scX - (best->baseX + best->width/2);
                    int targetOffY = scY - (best->baseY + best->height/2);
                    
                    g_animStart = g_camOffset;
                    g_animTarget.x = targetOffX;
                    g_animTarget.y = targetOffY;
                    g_animTime = std::chrono::steady_clock::now();
                    g_isAnim.store(true);
                }
                LeaveCriticalSection(&g_lock);
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
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, NULL, 0);
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, NULL, 0);
        
        if (!g_mouseHook || !g_kbHook) {
            MessageBoxW(NULL, L"Hook Error", L"Error", MB_ICONERROR);
            return -1;
        }
        
        g_stop.store(false);
        g_worker = std::thread(WorkerFunc);
        
        Sleep(300);
        ArrangeGrid();
        return 0;
    }
    if (m == WM_DESTROY) {
        g_stop.store(true);
        if (g_worker.joinable()) g_worker.join();
        UnhookWindowsHookEx(g_mouseHook);
        UnhookWindowsHookEx(g_kbHook);
        DeleteCriticalSection(&g_lock);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    if (!IsAdmin()) RunAsAdmin();
    
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = h;
    wc.lpszClassName = L"CanvasDesk";
    RegisterClassExW(&wc);
    
    g_hwnd = CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
                             L"CanvasDesk", L"", WS_POPUP, 0,0,1,1, NULL,NULL,h,NULL);
    if (!g_hwnd) return 1;
    
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOW);
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}