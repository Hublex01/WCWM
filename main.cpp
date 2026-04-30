#include <windows.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════════════════════
const int CANVAS_WIDTH  = 5000;
const int CANVAS_HEIGHT = 5000;
const WPARAM ACTIVATE_KEY = VK_RCONTROL;
const int THREAD_SLEEP_MS = 8; // ~120 FPS для плавности перемещения

// ═══════════════════════════════════════════════════════════════════════════════
// СТРУКТУРЫ ДАННЫХ
// ═══════════════════════════════════════════════════════════════════════════════
struct WindowSnapshot {
    HWND hwnd;
    int  snapshotX, snapshotY;  // Позиция на момент начала драга
    int  width, height;         // Размеры окна
};

struct WindowMoveOperation {
    HWND hwnd;
    int x, y, width, height;
    UINT flags;
};

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ СОСТОЯНИЯ
// ═══════════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_isDragging(false);
POINT g_dragStartMouse = {0, 0};        // Позиция мыши в момент нажатия
POINT g_currentMouse = {0, 0};          // Текущая позиция мыши (обновляется в хуке)
std::vector<WindowSnapshot> g_windowSnapshots;

// Логическое смещение для центрирования при первом drag
int g_virtualCanvasX = 0;
int g_virtualCanvasY = 0;

// Флаг для отслеживания первой инициализации
bool g_initialLayoutDone = false;

// Синхронизация для доступа к списку окон
CRITICAL_SECTION g_snapshotLock;

// Управление рабочим потоком
std::thread g_workerThread;
std::atomic<bool> g_stopWorker(false);

// Системные объекты
HHOOK g_mouseHook = NULL;
HWND  g_overlayWindow = NULL;

// ═══════════════════════════════════════════════════════════════════════════════
// УТИЛИТЫ БЕЗОПАСНОСТИ И ПРАВ
// ═══════════════════════════════════════════════════════════════════════════════

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup)) {
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
    
    if (ShellExecuteExW(&shellInfo)) {
        ExitProcess(0);
    } else {
        MessageBoxW(NULL, L"Не удалось получить права администратора!", 
                   L"Ошибка", MB_ICONERROR);
        ExitProcess(1);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ФИЛЬТРАЦИЯ ОКОН
// ═══════════════════════════════════════════════════════════════════════════════

bool IsFullscreenWindow(HWND hwnd) {
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    return (windowRect.right - windowRect.left >= screenWidth) && 
           (windowRect.bottom - windowRect.top >= screenHeight);
}

bool ShouldProcessWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_overlayWindow) return false;
    if (!IsWindowVisible(hwnd)) return false;
    if (IsFullscreenWindow(hwnd)) return false;
    
    wchar_t className[256] = {0};
    GetClassNameW(hwnd, className, 255);
    
    if (wcscmp(className, L"Shell_TrayWnd") == 0) return false;
    if (wcscmp(className, L"Progman") == 0) return false;
    if (wcscmp(className, L"WorkerW") == 0) return false;
    if (wcscmp(className, L"DV2ControlHost") == 0) return false;
    
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    
    bool hasFrame = (style & WS_THICKFRAME) || (style & WS_CAPTION);
    bool isPopup = (style & WS_POPUP) != 0;
    
    if (!hasFrame && !isPopup) return false;
    if (exStyle & WS_EX_NOACTIVATE) return false;
    
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SNAPSHOT СИСТЕМА
// ═══════════════════════════════════════════════════════════════════════════════

// Forward declaration
void ApplyWindowMoves(const std::vector<WindowMoveOperation>& operations);

// Расстановка окон в сетку 3 колонки
void ArrangeWindowsInGrid() {
    std::vector<WindowSnapshot> windows;
    
    // Собираем все окна
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!ShouldProcessWindow(hwnd)) return TRUE;
        
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        auto* windows = reinterpret_cast<std::vector<WindowSnapshot>*>(lParam);
        windows->push_back({
            hwnd,
            rect.left, rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top
        });
        
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    
    if (windows.empty()) return;
    
    const int COLUMNS = 3;
    const int PADDING = 5;
    
    // Вычисляем количество рядов
    int totalWindows = static_cast<int>(windows.size());
    int rows = (totalWindows + COLUMNS - 1) / COLUMNS;
    
    // Вычисляем максимальную ширину и высоту для каждой колонки/ряда
    std::vector<int> colMaxWidth(COLUMNS, 0);
    std::vector<int> rowMaxHeight(rows, 0);
    
    for (int i = 0; i < totalWindows; ++i) {
        int col = i % COLUMNS;
        int row = i / COLUMNS;
        
        colMaxWidth[col] = (windows[i].width > colMaxWidth[col]) ? windows[i].width : colMaxWidth[col];
        rowMaxHeight[row] = (windows[i].height > rowMaxHeight[row]) ? windows[i].height : rowMaxHeight[row];
    }
    
    // Вычисляем общую ширину и высоту сетки
    int gridWidth = 0;
    for (int w : colMaxWidth) gridWidth += w;
    gridWidth += (COLUMNS - 1) * PADDING;
    
    int gridHeight = 0;
    for (int h : rowMaxHeight) gridHeight += h;
    gridHeight += (rows - 1) * PADDING;
    
    // Вычисляем начальную позицию для центрирования
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int startX = (screenWidth - gridWidth) / 2;
    int startY = (screenHeight - gridHeight) / 2;
    
    // Ограничиваем, чтобы сетка не выходила за границы холста
    startX = (startX < 0) ? 0 : startX;
    startY = (startY < 0) ? 0 : startY;
    
    // Вычисляем новые позиции
    std::vector<WindowMoveOperation> moves;
    moves.reserve(totalWindows);
    
    int currentY = startY;
    for (int row = 0; row < rows; ++row) {
        int currentX = startX;
        for (int col = 0; col < COLUMNS; ++col) {
            int idx = row * COLUMNS + col;
            if (idx >= totalWindows) break;
            
            moves.push_back({
                windows[idx].hwnd,
                currentX, currentY,
                windows[idx].width, windows[idx].height,
                SWP_NOZORDER | SWP_NOACTIVATE
            });
            
            currentX += colMaxWidth[col] + PADDING;
        }
        currentY += rowMaxHeight[row] + PADDING;
    }
    
    // Применяем перемещения
    if (!moves.empty()) {
        ApplyWindowMoves(moves);
    }
}

void TakeWindowSnapshot() {
    EnterCriticalSection(&g_snapshotLock);
    g_windowSnapshots.clear();
    
    // Инициализируем виртуальный центр только один раз при первом snapshot
    if (g_virtualCanvasX == 0 && g_virtualCanvasY == 0) {
        g_virtualCanvasX = CANVAS_WIDTH / 2;  // 2500
        g_virtualCanvasY = CANVAS_HEIGHT / 2; // 2500
    }
    
    // Сохраняем исключительно физические координаты
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!ShouldProcessWindow(hwnd)) return TRUE;
        
        RECT rect;
        GetWindowRect(hwnd, &rect);
        
        auto* snapshots = reinterpret_cast<std::vector<WindowSnapshot>*>(lParam);
        snapshots->push_back({
            hwnd,
            rect.left, rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top
        });
        
        return TRUE;
    }, reinterpret_cast<LPARAM>(&g_windowSnapshots));
    
    LeaveCriticalSection(&g_snapshotLock);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ПАКЕТНОЕ ПЕРЕМЕЩЕНИЕ (Вызывается из ОТДЕЛЬНОГО ПОТОКА!)
// ═══════════════════════════════════════════════════════════════════════════════

void ApplyWindowMoves(const std::vector<WindowMoveOperation>& operations) {
    if (operations.empty()) return;
    
    HDWP deferHandle = BeginDeferWindowPos(static_cast<int>(operations.size()));
    if (!deferHandle) {
        for (const auto& op : operations) {
            SetWindowPos(op.hwnd, NULL, op.x, op.y, op.width, op.height, op.flags);
        }
        return;
    }
    
    for (const auto& op : operations) {
        HDWP result = DeferWindowPos(deferHandle, op.hwnd, NULL,
                                   op.x, op.y, op.width, op.height, op.flags);
        if (!result) {
            EndDeferWindowPos(deferHandle);
            return;
        }
        deferHandle = result;
    }
    
    EndDeferWindowPos(deferHandle);
}

// ═══════════════════════════════════════════════════════════════════════════════
// РАБОЧИЙ ПОТОК (Background Worker)
// ═══════════════════════════════════════════════════════════════════════════════

void WorkerThreadFunc() {
    while (!g_stopWorker.load()) {
        if (g_isDragging.load()) {
            POINT current = g_currentMouse;
            POINT start = g_dragStartMouse;
            
            int deltaX = current.x - start.x;
            int deltaY = current.y - start.y;
            
            std::vector<WindowMoveOperation> moves;
            
            EnterCriticalSection(&g_snapshotLock);
            moves.reserve(g_windowSnapshots.size());
            
            for (const auto& snapshot : g_windowSnapshots) {
                if (!IsWindow(snapshot.hwnd)) continue;
                
                // Простое перемещение: snapshot + delta
                int newX = snapshot.snapshotX + deltaX;
                int newY = snapshot.snapshotY + deltaY;
                
                // Границы холста (центр в CANVAS_WIDTH/2, CANVAS_HEIGHT/2)
                newX = std::max(-(snapshot.width / 2), 
                       std::min(newX, CANVAS_WIDTH - snapshot.width / 2));
                newY = std::max(-(snapshot.height / 2), 
                       std::min(newY, CANVAS_HEIGHT - snapshot.height / 2));
                
                moves.push_back({
                    snapshot.hwnd, newX, newY, snapshot.width, snapshot.height,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE
                });
            }
            LeaveCriticalSection(&g_snapshotLock);
            
            if (!moves.empty()) {
                ApplyWindowMoves(moves);
            }
        }
        
        // Небольшая пауза, чтобы не грузить процессор на 100%
        std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_MS));
    }
}

void StartWorkerThread() {
    g_stopWorker.store(false);
    g_workerThread = std::thread(WorkerThreadFunc);
}

void StopWorkerThread() {
    g_stopWorker.store(true);
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ОПЕРАЦИИ С ХОЛСТОМ
// ═══════════════════════════════════════════════════════════════════════════════

void StartCanvasDrag(POINT mousePosition) {
    g_isDragging.store(true);
    
    // Не добавляем offset - он применяется в worker thread
    g_dragStartMouse = mousePosition;
    g_currentMouse = mousePosition;
    
    TakeWindowSnapshot();
}

void StopCanvasDrag() {
    g_isDragging.store(false);
}

void ScaleCanvas(float scaleFactor, POINT scaleCenter) {
    EnterCriticalSection(&g_snapshotLock);
    // Если снапшот пуст (драг не начат), берем текущие позиции
    bool needUpdateSnapshot = g_windowSnapshots.empty();
    if (needUpdateSnapshot) {
        LeaveCriticalSection(&g_snapshotLock);
        TakeWindowSnapshot();
        EnterCriticalSection(&g_snapshotLock);
    }

    std::vector<WindowMoveOperation> moves;
    moves.reserve(g_windowSnapshots.size());

    for (auto& snapshot : g_windowSnapshots) {
        if (!IsWindow(snapshot.hwnd)) continue;

        int windowCenterX = snapshot.snapshotX + snapshot.width / 2;
        int windowCenterY = snapshot.snapshotY + snapshot.height / 2;

        int newCenterX = scaleCenter.x + static_cast<int>((windowCenterX - scaleCenter.x) * scaleFactor);
        int newCenterY = scaleCenter.y + static_cast<int>((windowCenterY - scaleCenter.y) * scaleFactor);

        int newWidth  = std::max(100, static_cast<int>(snapshot.width * scaleFactor));
        int newHeight = std::max(100, static_cast<int>(snapshot.height * scaleFactor));

        int newX = newCenterX - newWidth / 2;
        int newY = newCenterY - newHeight / 2;

        moves.push_back({
            snapshot.hwnd, newX, newY, newWidth, newHeight,
            SWP_NOZORDER | SWP_NOACTIVATE
        });

        // Обновляем снапшот к новым физическим координатам
        snapshot.snapshotX = newX;
        snapshot.snapshotY = newY;
        snapshot.width = newWidth;
        snapshot.height = newHeight;
    }
    
    LeaveCriticalSection(&g_snapshotLock);

    ApplyWindowMoves(moves);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЙ ХУК МЫШИ (Только чтение координат!)
// ═══════════════════════════════════════════════════════════════════════════════

LRESULT CALLBACK GlobalMouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) {
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    MSLLHOOKSTRUCT* mouseData = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    BOOL isActive = (GetAsyncKeyState(ACTIVATE_KEY) & 0x8000) != 0;

    if (!isActive) {
        if (g_isDragging.load()) StopCanvasDrag();
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    // Обновляем глобальные координаты (атомарно)
    g_currentMouse.x = mouseData->pt.x;
    g_currentMouse.y = mouseData->pt.y;

    if (wParam == WM_LBUTTONDOWN || wParam == WM_MBUTTONDOWN) {
        StartCanvasDrag(mouseData->pt);
        return 1; // Блокируем клик, чтобы окна не начали свой драг
    }

    if (wParam == WM_LBUTTONUP || wParam == WM_MBUTTONUP) {
        StopCanvasDrag();
        return 1;
    }

    if (wParam == WM_MOUSEMOVE) {
        // ВАЖНО: Не блокируем движение! Возвращаем CallNextHookEx.
        // Перемещение окон делает отдельный поток.
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    if (wParam == WM_MOUSEWHEEL) {
        short wheelDelta = GET_WHEEL_DELTA_WPARAM(mouseData->mouseData);
        float scaleFactor = (wheelDelta > 0) ? 1.1f : 0.9f;

        POINT scaleCenter = { GetSystemMetrics(SM_CXSCREEN) / 2,
                              GetSystemMetrics(SM_CYSCREEN) / 2 };
        ScaleCanvas(scaleFactor, scaleCenter);
        
        // Обновляем точку отсчета драга после зума (без offset - snapshot уже содержит истинные координаты)
        g_dragStartMouse = mouseData->pt;

        return 1; // Зум лучше блокировать, чтобы не скроллились окна под курсором
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ОКНО-ОВЕРЛЕЙ
// ═══════════════════════════════════════════════════════════════════════════════

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            InitializeCriticalSection(&g_snapshotLock);
            
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, GlobalMouseHook,
                                          GetModuleHandleW(NULL), 0);
            if (!g_mouseHook) {
                MessageBoxW(NULL, L"Не удалось установить хук мыши!", L"Ошибка", MB_ICONERROR);
                return -1;
            }
            
            StartWorkerThread(); // Запускаем поток перемещения
            return 0;
            
        case WM_DESTROY:
            StopWorkerThread(); // Останавливаем поток
            
            if (g_mouseHook) {
                UnhookWindowsHookEx(g_mouseHook);
                g_mouseHook = NULL;
            }
            
            DeleteCriticalSection(&g_snapshotLock);
            PostQuitMessage(0);
            return 0;
            
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void CreateOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW windowClass = { sizeof(WNDCLASSEXW) };
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance = hInstance;
    windowClass.lpszClassName = L"CanvasControllerOverlay";
    windowClass.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);

    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(NULL, L"Не удалось зарегистрировать класс окна!", L"Ошибка", MB_ICONERROR);
        return;
    }

    g_overlayWindow = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"CanvasControllerOverlay",
        L"Canvas Controller",
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, hInstance, NULL
    );

    if (!g_overlayWindow) {
        MessageBoxW(NULL, L"Не удалось создать окно-оверлей!", L"Ошибка", MB_ICONERROR);
        return;
    }

    SetLayeredWindowAttributes(g_overlayWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g_overlayWindow, SW_SHOW);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ТОЧКА ВХОДА
// ═══════════════════════════════════════════════════════════════════════════════

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    
    if (!IsRunningAsAdmin()) {
        RestartAsAdmin();
        return 0;
    }
    
    CreateOverlayWindow(hInstance);
    if (!g_overlayWindow) return 1;
    
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    
    return static_cast<int>(message.wParam);
}   