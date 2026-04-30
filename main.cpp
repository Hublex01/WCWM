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
const int THREAD_SLEEP_MS = 8; // ~120 FPS

// ═══════════════════════════════════════════════════════════════════════════════
// СТРУКТУРЫ ДАННЫХ
// ═══════════════════════════════════════════════════════════════════════════════
struct WindowSnapshot {
    HWND hwnd;
    int  snapshotX, snapshotY; // Нормализованная позиция (BasePos)
    int  width, height;
};

struct WindowMoveOperation {
    HWND hwnd;
    int x, y, width, height;
    UINT flags;
};

// Контекст для безопасной передачи данных в EnumWindows
struct SnapshotContext {
    std::vector<WindowSnapshot>* snapshots;
    POINT offset;
};

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ СОСТОЯНИЯ
// ═══════════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_isDragging(false);
POINT g_dragStartMouse = {0, 0};
POINT g_currentMouse = {0, 0};
std::vector<WindowSnapshot> g_windowSnapshots;

CRITICAL_SECTION g_snapshotLock;

std::thread g_workerThread;
std::atomic<bool> g_stopWorker(false);

HHOOK g_mouseHook = NULL;
HHOOK g_keyboardHook = NULL;
HWND  g_overlayWindow = NULL;

// Анимация
std::atomic<bool> g_isAnimating(false);
POINT g_animStartOffset = {0, 0};
POINT g_animTargetOffset = {0, 0};
std::chrono::steady_clock::time_point g_animStartTime;
const int g_animDurationMs = 400;

// Глобальное смещение холста (основное состояние камеры)
POINT g_currentCanvasOffset = {0, 0};

// ═══════════════════════════════════════════════════════════════════════════════
// УТИЛИТЫ
// ═══════════════════════════════════════════════════════════════════════════════

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

void RestartAsAdmin() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    SHELLEXECUTEINFOW shellInfo = { sizeof(shellInfo) };
    shellInfo.lpVerb = L"runas";
    shellInfo.lpFile = exePath;
    shellInfo.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&shellInfo)) {
        MessageBoxW(NULL, L"Требуемые права администратора!", L"Ошибка", MB_ICONERROR);
        ExitProcess(1);
    }
    ExitProcess(0);
}

bool IsFullscreenWindow(HWND hwnd) {
    RECT r; GetWindowRect(hwnd, &r);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    return ((r.right - r.left >= sw) && (r.bottom - r.top >= sh));
}

bool ShouldProcessWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_overlayWindow) return false;
    if (!IsWindowVisible(hwnd)) return false;
    if (IsFullscreenWindow(hwnd)) return false;
    
    wchar_t cls[256] = {0};
    GetClassNameW(hwnd, cls, 255);
    if (wcscmp(cls, L"Shell_TrayWnd") == 0) return false;
    if (wcscmp(cls, L"Progman") == 0) return false;
    if (wcscmp(cls, L"WorkerW") == 0) return false;
    
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    bool hasFrame = (style & WS_THICKFRAME) || (style & WS_CAPTION);
    bool isPopup = (style & WS_POPUP) != 0;
    if (!hasFrame && !isPopup) return false;
    if (exStyle & WS_EX_NOACTIVATE) return false;
    
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// СИСТЕМА ОКОН
// ═══════════════════════════════════════════════════════════════════════════════

void ApplyWindowMoves(const std::vector<WindowMoveOperation>& ops) {
    if (ops.empty()) return;
    
    HDWP h = BeginDeferWindowPos((int)ops.size());
    if (!h) {
        // Fallback если не удалось создать список отложенных операций
        for (const auto& op : ops) {
            SetWindowPos(op.hwnd, NULL, op.x, op.y, op.width, op.height, op.flags);
        }
        return;
    }
    
    for (const auto& op : ops) {
        h = DeferWindowPos(h, op.hwnd, NULL, op.x, op.y, op.width, op.height, op.flags);
        if (!h) {
            EndDeferWindowPos(h);
            return;
        }
    }
    
    EndDeferWindowPos(h);
}

void ArrangeWindowsInGrid() {
    std::vector<WindowSnapshot> wins;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!ShouldProcessWindow(h)) return TRUE;
        RECT r; GetWindowRect(h, &r);
        auto* v = (std::vector<WindowSnapshot>*)l;
        v->push_back({h, r.left, r.top, r.right - r.left, r.bottom - r.top});
        return TRUE;
    }, (LPARAM)&wins);

    if (wins.empty()) return;

    const int COLS = 3; const int PAD = 10;
    int rows = (wins.size() + COLS - 1) / COLS;
    std::vector<int> colW(COLS, 0), rowH(rows, 0);

    for (size_t i = 0; i < wins.size(); ++i) {
        colW[i % COLS] = std::max(colW[i % COLS], wins[i].width);
        rowH[i / COLS] = std::max(rowH[i / COLS], wins[i].height);
    }

    int gridW = 0, gridH = 0;
    for (int w : colW) gridW += w;
    for (int h : rowH) gridH += h;
    gridW += (COLS - 1) * PAD;
    gridH += (rows - 1) * PAD;

    // Центрируем сетку в центре виртуального холста (5000, 5000)
    int startX = (CANVAS_WIDTH / 2) - (gridW / 2);
    int startY = (CANVAS_HEIGHT / 2) - (gridH / 2);

    std::vector<WindowMoveOperation> moves;
    int curY = startY;
    for (int r = 0; r < rows; ++r) {
        int curX = startX;
        for (int c = 0; c < COLS; ++c) {
            int idx = r * COLS + c;
            if (idx >= (int)wins.size()) break;
            moves.push_back({wins[idx].hwnd, curX, curY, wins[idx].width, wins[idx].height, SWP_NOZORDER | SWP_NOACTIVATE});
            curX += colW[c] + PAD;
        }
        curY += rowH[r] + PAD;
    }
    ApplyWindowMoves(moves);
    
    // Сброс смещения камеры в 0 после расстановки, чтобы видеть результат сразу
    g_currentCanvasOffset = {0, 0};
}

void TakeWindowSnapshot() {
    EnterCriticalSection(&g_snapshotLock);
    g_windowSnapshots.clear();

    SnapshotContext ctx;
    ctx.snapshots = &g_windowSnapshots;
    ctx.offset = g_currentCanvasOffset; // Копируем текущее смещение

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!ShouldProcessWindow(hwnd)) return TRUE;
        RECT r; GetWindowRect(hwnd, &r);
        
        SnapshotContext* pCtx = (SnapshotContext*)lParam;
        
        // НОРМАЛИЗАЦИЯ: Сохраняем позицию относительно текущего смещения
        // BasePos = PhysicalPos - CurrentOffset
        pCtx->snapshots->push_back({
            hwnd,
            r.left - pCtx->offset.x,
            r.top - pCtx->offset.y,
            r.right - r.left,
            r.bottom - r.top
        });
        return TRUE;
    }, (LPARAM)&ctx);

    LeaveCriticalSection(&g_snapshotLock);
}

// ═══════════════════════════════════════════════════════════════════════════════
// РАБОЧИЙ ПОТОК
// ═══════════════════════════════════════════════════════════════════════════════

void WorkerThreadFunc() {
    while (!g_stopWorker.load()) {
        bool dragging = g_isDragging.load();
        bool animating = g_isAnimating.load();

        if (dragging || animating) {
            std::vector<WindowMoveOperation> moves;
            
            EnterCriticalSection(&g_snapshotLock);
            moves.reserve(g_windowSnapshots.size());

            if (animating) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_animStartTime).count();
                float t = std::min(1.0f, (float)elapsed / g_animDurationMs);
                // Ease Out Cubic
                float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

                int curX = (int)(g_animStartOffset.x + (g_animTargetOffset.x - g_animStartOffset.x) * ease);
                int curY = (int)(g_animStartOffset.y + (g_animTargetOffset.y - g_animStartOffset.y) * ease);

                g_currentCanvasOffset.x = curX;
                g_currentCanvasOffset.y = curY;

                for (auto& s : g_windowSnapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    // ВОССТАНОВЛЕНИЕ: Physical = Base + Offset
                    int nx = s.snapshotX + curX;
                    int ny = s.snapshotY + curY;
                    // Мягкие границы
                    nx = std::max(-5000, std::min(nx, CANVAS_WIDTH + 5000));
                    ny = std::max(-5000, std::min(ny, CANVAS_HEIGHT + 5000));
                    
                    moves.push_back({s.hwnd, nx, ny, s.width, s.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE});
                }

                if (t >= 1.0f) g_isAnimating.store(false);
            } 
            else if (dragging) {
                POINT cur = g_currentMouse;
                POINT last = g_dragStartMouse;
                int dx = cur.x - last.x;
                int dy = cur.y - last.y;

                g_currentCanvasOffset.x += dx;
                g_currentCanvasOffset.y += dy;
                
                // Сбрасываем точку отсчета для следующего кадра
                g_dragStartMouse = cur;

                for (auto& s : g_windowSnapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    int nx = s.snapshotX + g_currentCanvasOffset.x;
                    int ny = s.snapshotY + g_currentCanvasOffset.y;
                    nx = std::max(-5000, std::min(nx, CANVAS_WIDTH + 5000));
                    ny = std::max(-5000, std::min(ny, CANVAS_HEIGHT + 5000));
                    
                    moves.push_back({s.hwnd, nx, ny, s.width, s.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE});
                }
            }
            LeaveCriticalSection(&g_snapshotLock);

            if (!moves.empty()) ApplyWindowMoves(moves);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_MS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// УПРАВЛЕНИЕ
// ═══════════════════════════════════════════════════════════════════════════════

void StartCanvasDrag(POINT p) {
    g_isAnimating.store(false); // Прервать анимацию
    g_isDragging.store(true);
    g_dragStartMouse = p;
    g_currentMouse = p;
    TakeWindowSnapshot();
}

void ScaleCanvas(float scale, POINT center) {
    EnterCriticalSection(&g_snapshotLock);
    POINT off = g_currentCanvasOffset;
    
    if (g_windowSnapshots.empty()) {
        LeaveCriticalSection(&g_snapshotLock);
        TakeWindowSnapshot();
        EnterCriticalSection(&g_snapshotLock);
        off = g_currentCanvasOffset;
    }

    std::vector<WindowMoveOperation> moves;
    for (auto& s : g_windowSnapshots) {
        if (!IsWindow(s.hwnd)) continue;
        
        int physX = s.snapshotX + off.x;
        int physY = s.snapshotY + off.y;
        
        int cx = physX + s.width/2;
        int cy = physY + s.height/2;
        
        int ncx = center.x + (int)((cx - center.x) * scale);
        int ncy = center.y + (int)((cy - center.y) * scale);
        
        int nw = std::max(100, (int)(s.width * scale));
        int nh = std::max(100, (int)(s.height * scale));
        
        int nx = ncx - nw/2;
        int ny = ncy - nh/2;
        
        moves.push_back({s.hwnd, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE});
        
        // Обновляем нормализованный снапшот
        s.snapshotX = nx - off.x;
        s.snapshotY = ny - off.y;
        s.width = nw;
        s.height = nh;
    }
    LeaveCriticalSection(&g_snapshotLock);
    ApplyWindowMoves(moves);
}

// Логика центрирования на окне
void SnapToWindow(HWND target) {
    TakeWindowSnapshot(); // Обновляем снапшоты перед поиском
    EnterCriticalSection(&g_snapshotLock);
    
    // Поиск в актуальном списке
    WindowSnapshot* found = nullptr;
    for (auto& s : g_windowSnapshots) {
        if (s.hwnd == target) { found = &s; break; }
    }

    // Если не нашли (редкий кейс), пробуем найти родителя или обновить список
    if (!found) {
        HWND root = GetAncestor(target, GA_ROOTOWNER);
        for (auto& s : g_windowSnapshots) {
            if (s.hwnd == root) { found = &s; break; }
        }
    }

    if (found) {
        // Расчет физического центра: Base + Offset + Size/2
        int winCx = found->snapshotX + g_currentCanvasOffset.x + found->width / 2;
        int winCy = found->snapshotY + g_currentCanvasOffset.y + found->height / 2;
        
        int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;
        
        // Целевое смещение: чтобы центр окна совпал с центром экрана
        int targetOffX = screenCx - winCx;
        int targetOffY = screenCy - winCy;
        
        g_animStartOffset = g_currentCanvasOffset;
        g_animTargetOffset.x = targetOffX;
        g_animTargetOffset.y = targetOffY;
        g_animStartTime = std::chrono::steady_clock::now();
        g_isAnimating.store(true);
    }
    LeaveCriticalSection(&g_snapshotLock);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ХУКИ
// ═══════════════════════════════════════════════════════════════════════════════

LRESULT CALLBACK MouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    
    MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lParam;
    bool active = (GetAsyncKeyState(ACTIVATE_KEY) & 0x8000) != 0;
    
    if (!active) {
        if (g_isDragging.load()) { g_isDragging.store(false); }
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }
    
    g_currentMouse.x = m->pt.x;
    g_currentMouse.y = m->pt.y;
    
    if (wParam == WM_LBUTTONDOWN) {
        HWND h = WindowFromPoint(m->pt);
        if (h) SnapToWindow(GetAncestor(h, GA_ROOTOWNER));
        return 1; // Блок клика
    }
    
    if (wParam == WM_MBUTTONDOWN) {
        StartCanvasDrag(m->pt);
        return 1;
    }
    
    if (wParam == WM_MBUTTONUP) {
        g_isDragging.store(false);
        return 1;
    }
    
    if (wParam == WM_MOUSEWHEEL) {
        short d = GET_WHEEL_DELTA_WPARAM(m->mouseData);
        ScaleCanvas(d > 0 ? 1.1f : 0.9f, {GetSystemMetrics(SM_CXSCREEN)/2, GetSystemMetrics(SM_CYSCREEN)/2});
        g_dragStartMouse = m->pt;
        return 1;
    }
    
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK KeyboardHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
    
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
        if (k->vkCode == VK_NUMPAD5) {
            if ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0) {
                // 1. СНАЧАЛА фиксируем текущее состояние
                TakeWindowSnapshot();

                EnterCriticalSection(&g_snapshotLock);
                
                int bestDist = INT_MAX;
                WindowSnapshot* best = nullptr;
                int scX = GetSystemMetrics(SM_CXSCREEN)/2;
                int scY = GetSystemMetrics(SM_CYSCREEN)/2;
                
                for (auto& s : g_windowSnapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    // Используем актуальные координаты
                    int cx = s.snapshotX + g_currentCanvasOffset.x + s.width/2;
                    int cy = s.snapshotY + g_currentCanvasOffset.y + s.height/2;
                    int dist = (cx-scX)*(cx-scX) + (cy-scY)*(cy-scY);
                    if (dist < bestDist) { bestDist = dist; best = &s; }
                }
                
                if (best) {
                    int cx = best->snapshotX + g_currentCanvasOffset.x + best->width/2;
                    int cy = best->snapshotY + g_currentCanvasOffset.y + best->height/2;
                    
                    // Start offset теперь точно совпадает с базой в снапшоте
                    g_animStartOffset = g_currentCanvasOffset;
                    g_animTargetOffset.x = (scX - cx);
                    g_animTargetOffset.y = (scY - cy);
                    g_animStartTime = std::chrono::steady_clock::now();
                    g_isAnimating.store(true);
                }
                LeaveCriticalSection(&g_snapshotLock);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════════════
// WINDOW PROC & MAIN
// ═══════════════════════════════════════════════════════════════════════════════

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CREATE) {
        InitializeCriticalSection(&g_snapshotLock);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, NULL, 0);
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, NULL, 0);
        
        if (!g_mouseHook || !g_keyboardHook) {
            MessageBoxW(NULL, L"Ошибка установки хуков", L"Error", MB_ICONERROR);
            return -1;
        }
        
        g_stopWorker.store(false);
        g_workerThread = std::thread(WorkerThreadFunc);
        
        Sleep(300);
        ArrangeWindowsInGrid();
        return 0;
    }
    if (m == WM_DESTROY) {
        g_stopWorker.store(true);
        if (g_workerThread.joinable()) g_workerThread.join();
        
        UnhookWindowsHookEx(g_mouseHook);
        UnhookWindowsHookEx(g_keyboardHook);
        DeleteCriticalSection(&g_snapshotLock);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    if (!IsRunningAsAdmin()) RestartAsAdmin();
    
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CanvasDesk";
    RegisterClassExW(&wc);
    
    HWND hw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                              L"CanvasDesk", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, hInst, NULL);
    if (!hw) return 1;
    
    SetLayeredWindowAttributes(hw, 0, 0, LWA_ALPHA);
    ShowWindow(hw, SW_SHOW);
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}