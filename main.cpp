#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <algorithm>
#pragma comment(lib, "dwmapi.lib")
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <iomanip>
#include <shlobj.h>
#include <cstdio>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// ═══════════════════════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════════════════════
const int CANVAS_WIDTH  = 10000;
const int CANVAS_HEIGHT = 10000;

// Структура конфигурации
struct Config {
    // Клавиши
    WPARAM activateKey;
    WPARAM panKey;
    
    // Анимация
    int threadSleepMs;
    int gridAnimDuration;
    int camAnimDuration;
    int zoomAnimDuration;
    
    // Физика
    int physicsDelayMs;
    int maxPhysicsDistance;
    int windowGap;
    
    // Double-tap
    int doubleTapTimeout;
    int holdThreshold;
    
    // Зум лимиты
    int minWindowSize;
    float maxWindowSizePercent;
    
    // Canvas
    int canvasWidth;
    int canvasHeight;

    Config() : 
        activateKey(VK_RCONTROL), 
        panKey(0),
        threadSleepMs(8),
        gridAnimDuration(600),
        camAnimDuration(400),
        zoomAnimDuration(150),
        physicsDelayMs(1000),
        maxPhysicsDistance(100),
        windowGap(2),
        doubleTapTimeout(300),
        holdThreshold(200),
        minWindowSize(300),
        maxWindowSizePercent(0.98f),
        canvasWidth(10000),
        canvasHeight(10000)
    {}
};

// Путь к config.ini (%APPDATA%\WCWM\config.ini)
std::wstring GetConfigPath() {
    wchar_t appData[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    std::wstring path = std::wstring(appData) + L"\\WCWM\\config.ini";
    return path;
}

// Создать директорию %APPDATA%\WCWM если её нет
void EnsureConfigDir() {
    std::wstring path = GetConfigPath();
    size_t pos = path.find_last_of(L'\\');
    if (pos != std::wstring::npos) {
        std::wstring dir = path.substr(0, pos);
        CreateDirectoryW(dir.c_str(), NULL);
    }
}

// Forward declaration
void SaveConfig(const Config& cfg);

// Загрузить конфиг из файла (если нет — использовать дефолты)
Config LoadConfig() {
    Config cfg;
    std::wstring path = GetConfigPath();

    bool fileExists = (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    
    if (!fileExists) {
        // Файла нет — создаём с дефолтами
        SaveConfig(cfg);
        return cfg;
    }

    // Читаем клавиши
    int activate = GetPrivateProfileIntW(L"General", L"activateKey", (int)cfg.activateKey, path.c_str());
    int pan = GetPrivateProfileIntW(L"General", L"panKey", (int)cfg.panKey, path.c_str());
    
    if (activate >= 1 && activate <= 255) cfg.activateKey = (WPARAM)activate;
    if (pan >= 0 && pan <= 255) cfg.panKey = (WPARAM)pan;

    // Читаем анимацию
    cfg.threadSleepMs = GetPrivateProfileIntW(L"Animation", L"threadSleepMs", cfg.threadSleepMs, path.c_str());
    cfg.gridAnimDuration = GetPrivateProfileIntW(L"Animation", L"gridAnimDuration", cfg.gridAnimDuration, path.c_str());
    cfg.camAnimDuration = GetPrivateProfileIntW(L"Animation", L"camAnimDuration", cfg.camAnimDuration, path.c_str());
    cfg.zoomAnimDuration = GetPrivateProfileIntW(L"Animation", L"zoomAnimDuration", cfg.zoomAnimDuration, path.c_str());

    // Читаем физику
    cfg.physicsDelayMs = GetPrivateProfileIntW(L"Physics", L"physicsDelayMs", cfg.physicsDelayMs, path.c_str());
    cfg.maxPhysicsDistance = GetPrivateProfileIntW(L"Physics", L"maxPhysicsDistance", cfg.maxPhysicsDistance, path.c_str());
    cfg.windowGap = GetPrivateProfileIntW(L"Physics", L"windowGap", cfg.windowGap, path.c_str());

    // Читаем double-tap
    cfg.doubleTapTimeout = GetPrivateProfileIntW(L"DoubleTap", L"doubleTapTimeout", cfg.doubleTapTimeout, path.c_str());
    cfg.holdThreshold = GetPrivateProfileIntW(L"DoubleTap", L"holdThreshold", cfg.holdThreshold, path.c_str());

    // Читаем зум лимиты
    cfg.minWindowSize = GetPrivateProfileIntW(L"Zoom", L"minWindowSize", cfg.minWindowSize, path.c_str());
    wchar_t percentBuf[32];
    GetPrivateProfileStringW(L"Zoom", L"maxWindowSizePercent", L"0.98", percentBuf, 32, path.c_str());
    cfg.maxWindowSizePercent = (float)_wtof(percentBuf);

    // Читаем canvas
    cfg.canvasWidth = GetPrivateProfileIntW(L"Canvas", L"canvasWidth", cfg.canvasWidth, path.c_str());
    cfg.canvasHeight = GetPrivateProfileIntW(L"Canvas", L"canvasHeight", cfg.canvasHeight, path.c_str());

    return cfg;
}

// Сохранить конфиг атомарно (через temp-файл + rename)
void SaveConfig(const Config& cfg) {
    EnsureConfigDir();
    std::wstring path = GetConfigPath();
    std::wstring tmpPath = path + L".tmp";

    wchar_t buf[32];
    
    // General
    swprintf_s(buf, L"%d", (int)cfg.activateKey);
    WritePrivateProfileStringW(L"General", L"activateKey", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", (int)cfg.panKey);
    WritePrivateProfileStringW(L"General", L"panKey", buf, tmpPath.c_str());
    
    // Animation
    swprintf_s(buf, L"%d", cfg.threadSleepMs);
    WritePrivateProfileStringW(L"Animation", L"threadSleepMs", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.gridAnimDuration);
    WritePrivateProfileStringW(L"Animation", L"gridAnimDuration", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.camAnimDuration);
    WritePrivateProfileStringW(L"Animation", L"camAnimDuration", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.zoomAnimDuration);
    WritePrivateProfileStringW(L"Animation", L"zoomAnimDuration", buf, tmpPath.c_str());
    
    // Physics
    swprintf_s(buf, L"%d", cfg.physicsDelayMs);
    WritePrivateProfileStringW(L"Physics", L"physicsDelayMs", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.maxPhysicsDistance);
    WritePrivateProfileStringW(L"Physics", L"maxPhysicsDistance", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.windowGap);
    WritePrivateProfileStringW(L"Physics", L"windowGap", buf, tmpPath.c_str());
    
    // DoubleTap
    swprintf_s(buf, L"%d", cfg.doubleTapTimeout);
    WritePrivateProfileStringW(L"DoubleTap", L"doubleTapTimeout", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.holdThreshold);
    WritePrivateProfileStringW(L"DoubleTap", L"holdThreshold", buf, tmpPath.c_str());
    
    // Zoom
    swprintf_s(buf, L"%d", cfg.minWindowSize);
    WritePrivateProfileStringW(L"Zoom", L"minWindowSize", buf, tmpPath.c_str());
    swprintf_s(buf, L"%.2f", cfg.maxWindowSizePercent);
    WritePrivateProfileStringW(L"Zoom", L"maxWindowSizePercent", buf, tmpPath.c_str());
    
    // Canvas
    swprintf_s(buf, L"%d", cfg.canvasWidth);
    WritePrivateProfileStringW(L"Canvas", L"canvasWidth", buf, tmpPath.c_str());
    swprintf_s(buf, L"%d", cfg.canvasHeight);
    WritePrivateProfileStringW(L"Canvas", L"canvasHeight", buf, tmpPath.c_str());

    // Атомарная замена
    MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// Глобальный экземпляр конфига
Config g_config;

WPARAM g_activateKey = VK_RCONTROL;
WPARAM g_panKey = 0;

// ═══════════════════════════════════════════════════════════════════════════════
// СТРУКТУРЫ
// ═══════════════════════════════════════════════════════════════════════════════
struct WindowSnapshot {
    HWND hwnd;
    int  baseX, baseY; 
    int  width, height;
    int  originalWidth, originalHeight; // Исходные размеры для анимации зума
    float zoomDebt = 0.0f; // Накопленный долг зума (положительный = хотели увеличить, отрицательный = хотели уменьшить)
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
    int durationMs = 600;
};

// Вспомогательная структура для сортировки окон
struct SortedWindow {
    HWND hwnd;
    int w, h;
    long long area;
    int curX, curY; // Текущие координаты (для старта анимации)
};

// ═══════════════════════════════════════════════════════════════════════════════
// ЗАЩИТА ОТ ВТОРОЙ КОПИИ (SINGLE INSTANCE)
// ═══════════════════════════════════════════════════════════════════════════════
HANDLE g_hSingleInstanceMutex = NULL; // Хэндл мьютекса — глобально, чтобы не закрылся досрочно

// Проверка единственного экземпляра
bool CheckSingleInstance() {
    // Имя мьютекса: Local\ для изоляции по пользователям, GUID для абсолютной уникальности
    const wchar_t* MUTEX_NAME = L"Local\\WCWM_SingleInstance_B7F2A9E4-1C3D-48B5-9A6E-3F8C2D1B0A9E";
    g_hSingleInstanceMutex = CreateMutexW(
        NULL,           // Атрибуты безопасности по умолчанию
        TRUE,           // Вызывающий поток сразу владеет мьютексом
        MUTEX_NAME      // Имя мьютекса
    );
    
    if (g_hSingleInstanceMutex == NULL) {
        // Не удалось создать мьютекс (ошибка системы) — из соображений безопасности запрещаем запуск
        return false;
    }
    
    DWORD lastError = GetLastError();
    if (lastError == ERROR_ALREADY_EXISTS) {
        // Мьютекс уже существовал — значит, программа уже запущена
        return false;
    }
    
    // Мьютекс создан впервые — продолжаем работу
    return true;
}

// Активация уже работающего экземпляра
void ActivateExistingInstance() {
    // Ищем главное окно по классу
    HWND hExisting = FindWindowW(L"CanvasDesk", NULL);
    if (hExisting != NULL) {
        // Восстанавливаем если свернуто
        if (IsIconic(hExisting)) {
            ShowWindow(hExisting, SW_RESTORE);
        }
        // Выводим на передний план
        SetForegroundWindow(hExisting);
    }
    
    // Показываем сообщение пользователю
    MessageBoxW(NULL, 
        L"Программа уже запущена.\n\nПриложение может управлять окнами и установлено глобальные хуки ввода.\nЗапуск второй копии приведет к конфликту.",
        L"WCWM — Программа уже запущена",
        MB_ICONINFORMATION | MB_OK);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ═════════════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_isDragging(false);
std::atomic<WPARAM> g_panStartButton = 0; // Запоминаем, какая кнопка начала тягу (VK_MBUTTON, VK_LBUTTON и т.д.)
std::vector<WindowSnapshot> g_snapshots;
std::vector<HWND> g_newWindowsFound; // Нееповторяющийся буфер новых окон
std::wstring g_newWindowNotice;
CRITICAL_SECTION g_lock;
std::thread g_worker;
std::atomic<bool> g_stop(false);

// Delta-accumulation для плавного перетаскивания
std::atomic<long> g_mouseDeltaX{0};
std::atomic<long> g_mouseDeltaY{0};
POINT g_lastHookMousePos = {0, 0};

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

// Анимация зума
struct ZoomAnimState {
    bool active = false;
    std::chrono::steady_clock::time_point startTime;
    int durationMs = 150; // Длительность анимации зума
    
    // Снимок начального состояния
    struct WindowState {
        HWND hwnd;
        int startX, startY, startW, startH;
        int targetX, targetY, targetW, targetH;
    };
    std::vector<WindowState> windows;
};
ZoomAnimState g_zoomAnim;
std::atomic<bool> g_lastZoomWasIn{false}; // Направление последнего зума для физики
std::chrono::steady_clock::time_point g_lastZoomTime;
const int PHYSICS_DELAY_MS = 1000; // Задержка перед запуском физики после зума (1 секунда)
std::atomic<bool> g_physicsScheduled{false}; // Флаг: физика запланирована

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
// UI STATE — Fonts & Hover
// ═══════════════════════════════════════════════════════════════════════════════
HFONT g_hFontBtn = NULL;      // Segoe UI Bold  — button labels
HFONT g_hFontDebug = NULL;    // Consolas       — debug log
bool  g_btnBindHover = false;
bool  g_btnPanHover = false;

// ═══════════════════════════════════════════════════════════════════════════════
// THEME — Catppuccin Mocha Palette
// ═══════════════════════════════════════════════════════════════════════════════
// All colors are straight RGB (0–255)
const COLORREF CLR_BG           = RGB(30,  30,  46);  // #1E1E2E — base dark
const COLORREF CLR_BTN_BIND_NORM= RGB(137, 180, 250); // #89B4FA — blue (activate)
const COLORREF CLR_BTN_BIND_HOT = RGB(110, 153, 242); // #6E98F2 — blue hover (darker)
const COLORREF CLR_BTN_BIND_ACT = RGB(250, 179, 135); // #FABFB7 — peach (listening)
const COLORREF CLR_BTN_PAN_NORM = RGB(166, 227, 161); // #A6E3A1 — green (pan)
const COLORREF CLR_BTN_PAN_HOT  = RGB(137, 220, 128); // #89E280 — green hover (darker)
const COLORREF CLR_BTN_PAN_ACT  = RGB(250, 179, 135); // #FABFB7 — peach (listening)
const COLORREF CLR_TEXT         = RGB(205, 214, 244); // #CDD6F4 — primary text
const COLORREF CLR_TEXT_DIM     = RGB(166, 173, 200); // #A6ADc8 — secondary text

// ═══════════════════════════════════════════════════════════════════════════════
// СТРУКТУРЫ ДЛЯ ОТЛОЖЕННОЙ СТЫКОВКИ
// ═══════════════════════════════════════════════════════════════════════════════
struct PendingWindow {
    HWND hwnd;
    int targetAbsX, targetAbsY; // Абсолютные целевые координаты в сетке
    int width, height;
};

std::vector<PendingWindow> g_pendingWindows;

// Центральное окно (определяется в ArrangeGrid)
HWND g_centerWindow = NULL;

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
void ResolveCollisionsAfterZoom(bool isZoomIn);


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
    
    // Сбрасываем долг зума у всех окон
    EnterCriticalSection(&g_lock);
    for (auto& s : g_snapshots) {
        s.zoomDebt = 0.0f;
    }
    LeaveCriticalSection(&g_lock);
    
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
    ss << L"Zoom Anim: " << (g_zoomAnim.active ? L"RUNNING" : L"IDLE") << L"\n";
    ss << L"Physics Scheduled: " << (g_physicsScheduled.load() ? L"YES" : L"NO") << L"\n";
    ss << L"Last Zoom: " << (g_lastZoomWasIn.load() ? L"IN" : L"OUT") << L"\n";
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

        ss << L"[" << count << L"] " << title;
        
        // Показываем долг зума, если он ненулевой
        if (std::abs(s.zoomDebt) > 0.001f) {
            ss << L" (debt: " << std::fixed << std::setprecision(1) << (s.zoomDebt * 100.0f) << L"%)";
        }
        
        ss << L"\n";
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
            SetWindowTextW(hwnd, L"wcwm");
            // ─── Create persistent fonts (Segoe UI for buttons, Consolas for debug log) ───
            g_hFontBtn   = CreateFontW(12, 0, 0, 0, FW_BOLD,   0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
            g_hFontDebug = CreateFontW(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH|FF_MODERN, L"Consolas");
            // Hover states default to false — harmless if window created off-screen
            g_btnBindHover = false;
            g_btnPanHover  = false;
            return 0;

        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            // Button layout: right-aligned, 160px wide × 30px tall, 5px v-gap
            g_btnBindRect = { rc.right - 170, 10, rc.right - 10, 40 };
            g_btnPanRect  = { rc.right - 170, 45, rc.right - 10, 75 };
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            bool inBind = (PtInRect(&g_btnBindRect, pt) != 0);
            bool inPan  = (PtInRect(&g_btnPanRect,  pt) != 0);
            bool changed = (inBind != g_btnBindHover) || (inPan != g_btnPanHover);
            if (changed) {
                g_btnBindHover = inBind;
                g_btnPanHover  = inPan;
                InvalidateRect(hwnd, NULL, FALSE);  // repaint only if state changed
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam); int y = HIWORD(lParam);
            if (x >= g_btnBindRect.left && x <= g_btnBindRect.right && y >= g_btnBindRect.top && y <= g_btnBindRect.bottom) {
                g_bindingMode = true; g_bindingPanKey = false;
                if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, FALSE);
            } else if (x >= g_btnPanRect.left && x <= g_btnPanRect.right && y >= g_btnPanRect.top && y <= g_btnPanRect.bottom) {
                g_bindingMode = true; g_bindingPanKey = true;
                if (g_debugHwnd) InvalidateRect(g_debugHwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return TRUE;  // Suppress default background erase — we handle it in WM_PAINT

        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);

            // ─── Double buffering: compatible memory DC + bitmap ───
            HDC     hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hBmp   = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP hOldBmp= (HBITMAP)SelectObject(hdcMem, hBmp);

            // ─── Background ───────────────────────────────────────
            HBRUSH hBgBrush = CreateSolidBrush(CLR_BG);
            FillRect(hdcMem, &rc, hBgBrush);
            DeleteObject(hBgBrush);

            // ─── Button: Set Activate ──────────────────────────────
            COLORREF cBind = CLR_BTN_BIND_NORM;
            if (g_bindingMode && !g_bindingPanKey)      cBind = CLR_BTN_BIND_ACT;
            else if (g_btnBindHover && !g_bindingMode)  cBind = CLR_BTN_BIND_HOT;

            HBRUSH hBrushBind = CreateSolidBrush(cBind);
            RoundRect(hdcMem, g_btnBindRect.left, g_btnBindRect.top,
                            g_btnBindRect.right, g_btnBindRect.bottom, 8, 8);

            // Dark border (1px black) to separate from BG
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
            HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hNullBrush);
            RoundRect(hdcMem, g_btnBindRect.left, g_btnBindRect.top,
                            g_btnBindRect.right, g_btnBindRect.bottom, 8, 8);
            SelectObject(hdcMem, hOldBrush);
            SelectObject(hdcMem, hOldPen);
            DeleteObject(hPen);
            DeleteObject(hBrushBind);

            // ─── Button: Set Pan Key ───────────────────────────────
            COLORREF cPan = CLR_BTN_PAN_NORM;
            if (g_bindingMode && g_bindingPanKey)       cPan = CLR_BTN_PAN_ACT;
            else if (g_btnPanHover && !g_bindingMode)   cPan = CLR_BTN_PAN_HOT;

            HBRUSH hBrushPan = CreateSolidBrush(cPan);
            RoundRect(hdcMem, g_btnPanRect.left, g_btnPanRect.top,
                            g_btnPanRect.right, g_btnPanRect.bottom, 8, 8);
            // Border
            hOldPen   = (HPEN)SelectObject(hdcMem, hPen);
            hOldBrush = (HBRUSH)SelectObject(hdcMem, hNullBrush);
            RoundRect(hdcMem, g_btnPanRect.left, g_btnPanRect.top,
                            g_btnPanRect.right, g_btnPanRect.bottom, 8, 8);
            SelectObject(hdcMem, hOldBrush);
            SelectObject(hdcMem, hOldPen);
            DeleteObject(hPen);        // hPen created above — safe delete after second use
            DeleteObject(hBrushPan);

            // ─── Button Text (Segoe UI Bold) ──────────────────────
            SetBkMode(hdcMem, TRANSPARENT);
            SetTextColor(hdcMem, CLR_TEXT);
            HFONT hOldFontBtn = (HFONT)SelectObject(hdcMem, g_hFontBtn);
            DrawTextW(hdcMem, (g_bindingMode && !g_bindingPanKey) ? L"LISTENING..." : L"Set Activate",
                      -1, &g_btnBindRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(hdcMem, (g_bindingMode && g_bindingPanKey) ? L"LISTENING..." : L"Set Pan Key",
                      -1, &g_btnPanRect,  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdcMem, hOldFontBtn);

            // ─── Debug Log Text (Consolas) ────────────────────────
            HFONT hOldFontDbg = (HFONT)SelectObject(hdcMem, g_hFontDebug);
            SetTextColor(hdcMem, CLR_TEXT_DIM);
            RECT tr = { 10, 85, rc.right - 10, rc.bottom - 10 };
            EnterCriticalSection(&g_debugLock);
            std::wstring t = g_debugText;
            LeaveCriticalSection(&g_debugLock);
            DrawTextW(hdcMem, t.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(hdcMem, hOldFontDbg);

            // ─── Blit memory → screen ─────────────────────────────
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);

            // ─── Cleanup ───────────────────────────────────────────
            SelectObject(hdcMem, hOldBmp);
            DeleteObject(hBmp);
            DeleteDC(hdcMem);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            // Clean up fonts to avoid GDI leaks
            if (g_hFontBtn)   { DeleteObject(g_hFontBtn);   g_hFontBtn   = NULL; }
            if (g_hFontDebug) { DeleteObject(g_hFontDebug); g_hFontDebug = NULL; }
            g_debugHwnd = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DWM TITLE BAR THEMING (Catppuccin Mocha)
// ═══════════════════════════════════════════════════════════════════════════════
// Attempts to set a dark title bar with custom colors.
// Strategy: Try DWMWA_CAPTION_COLOR first (Win11 22H2+).
// If unsupported, fall back to DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+).
// On older OS: no-op (light title bar accepted).
// Note: DWMWA_CAPTION_COLOR (35), DWMWA_TEXT_COLOR (36), DWMWA_BORDER_COLOR (34)
// are defined in newer SDKs; we use raw integers for maximum compatibility.
void ApplyDarkTitleBar(HWND hwnd) {
    if (!hwnd) return;

    // Catppuccin Mocha colors
    const COLORREF CLR_CAPTION = RGB(30, 30, 46);   // #1E1E2E
    const COLORREF CLR_TEXT    = RGB(205, 214, 244); // #CDD6F4
    const COLORREF CLR_BORDER  = RGB(30, 30, 46);   // match caption

    // Try custom caption color (Win11 22H2+). If it works, also set text and border.
    HRESULT hr = DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &CLR_CAPTION, sizeof(CLR_CAPTION));
    if (SUCCEEDED(hr)) {
        DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/,    &CLR_TEXT,    sizeof(CLR_TEXT));
        DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/,  &CLR_BORDER,  sizeof(CLR_BORDER));
    } else {
        // Fallback: immersive dark mode (Win10 20H1+)
        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hwnd, 19 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark, sizeof(useDark));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// DEBUG WINDOW CREATION
// ═══════════════════════════════════════════════════════════════════════════════
void CreateDebugWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DebugWndProc; wc.hInstance = hInst; wc.lpszClassName = L"CanvasDebugClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    if (!RegisterClassExW(&wc)) return;
    g_debugHwnd = CreateWindowExW(0, L"CanvasDebugClass", L"WCWM", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 450, 650, NULL, NULL, hInst, NULL);
    if (g_debugHwnd) {
        ApplyDarkTitleBar(g_debugHwnd);
        ShowWindow(g_debugHwnd, SW_SHOW);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ДВИЖОК
// ═══════════════════════════════════════════════════════════════════════════════
void ApplyMoves(const std::vector<WindowMoveOp>& ops) {
    if (ops.empty()) return;

    // Фильтруем только существующие окна
    std::vector<WindowMoveOp> validOps;
    validOps.reserve(ops.size());
    for (auto& o : ops) {
        if (IsWindow(o.hwnd)) {
            validOps.push_back(o);
        }
    }

    if (validOps.empty()) return;

    HDWP h = BeginDeferWindowPos((int)validOps.size());
    if (!h) {
        for (auto& o : validOps) SetWindowPos(o.hwnd, 0, o.x, o.y, o.w, o.h, o.flags);
        return;
    }
    for (auto& o : validOps) {
        h = DeferWindowPos(h, o.hwnd, 0, o.x, o.y, o.w, o.h, o.flags);
        if (!h) {
            EndDeferWindowPos(h);
            return;
        }
    }
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

    int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
    int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

    // 2. Найти ближайшее окно к центру экрана (оно станет центральным)
    size_t centerIdx = 0;
    long long minDistSq = LLONG_MAX;
    for (size_t i = 0; i < list.size(); ++i) {
        int cx = list[i].baseX + list[i].width / 2;
        int cy = list[i].baseY + list[i].height / 2;
        long long distSq = 1LL * (cx - screenCx) * (cx - screenCx) + 
                          1LL * (cy - screenCy) * (cy - screenCy);
        if (distSq < minDistSq) {
            minDistSq = distSq;
            centerIdx = i;
        }
    }

    // 3. Создаём отсортированный список: центральное первое, остальные по расстоянию от него
    WindowSnapshot centerWindow = list[centerIdx];
    int centerCx = centerWindow.baseX + centerWindow.width / 2;
    int centerCy = centerWindow.baseY + centerWindow.height / 2;
    
    // ВАЖНО: Запоминаем центральное окно для физики коллизий
    g_centerWindow = centerWindow.hwnd;

    // Создаём пары (окно, расстояние до центра)
    struct WindowWithDist {
        WindowSnapshot window;
        long long distSq;
    };
    std::vector<WindowWithDist> others;
    others.reserve(list.size() - 1);

    for (size_t i = 0; i < list.size(); ++i) {
        if (i == centerIdx) continue;
        int cx = list[i].baseX + list[i].width / 2;
        int cy = list[i].baseY + list[i].height / 2;
        long long distSq = 1LL * (cx - centerCx) * (cx - centerCx) + 
                          1LL * (cy - centerCy) * (cy - centerCy);
        others.push_back({list[i], distSq});
    }

    // Сортируем по расстоянию
    std::sort(others.begin(), others.end(), [](const WindowWithDist& a, const WindowWithDist& b) {
        return a.distSq < b.distSq;
    });

    // Собираем финальный список
    std::vector<WindowSnapshot> sortedList;
    sortedList.reserve(list.size());
    sortedList.push_back(centerWindow);
    for (const auto& wd : others) {
        sortedList.push_back(wd.window);
    }

    // 4. Радиальная упаковка (Circle Packing)
    const int GAP = 2;
    struct PlacedRect { int x, y, w, h; };
    std::vector<PlacedRect> placed;
    std::vector<POINT> finalPositions;

    auto intersects = [&](int nx, int ny, int nw, int nh) -> bool {
        for (const auto& pr : placed) {
            // Проверка пересечения с учётом минимального зазора GAP между окнами
            // Окна НЕ пересекаются, если между ними есть зазор >= GAP
            if (!(nx + nw + GAP <= pr.x || nx >= pr.x + pr.w + GAP || 
                  ny + nh + GAP <= pr.y || ny >= pr.y + pr.h + GAP)) {
                return true;
            }
        }
        return false;
    };

    EnterCriticalSection(&g_lock);
    g_gridAnim.items.clear();
    g_gridAnim.items.reserve(sortedList.size());

    // 5. Размещение окон
    for (size_t i = 0; i < sortedList.size(); ++i) {
        int w = sortedList[i].width;
        int h = sortedList[i].height;
        POINT bestPos = {0, 0};

        if (i == 0) {
            // Центральное окно размещается строго в центре (0, 0 в координатах сетки)
            bestPos = { -w / 2, -h / 2 };
        } else {
            // Определяем, где окно находилось относительно центрального
            int windowCx = sortedList[i].baseX + w / 2;
            int windowCy = sortedList[i].baseY + h / 2;
            
            // Вектор от центра к окну (направление)
            int dx = windowCx - centerCx;
            int dy = windowCy - centerCy;
            
            // Находим центральное размещённое окно (первое в placed)
            const auto& centerPlaced = placed[0];
            int centerPlacedCx = centerPlaced.x + centerPlaced.w / 2;
            int centerPlacedCy = centerPlaced.y + centerPlaced.h / 2;
            
            // Пробуем позиции вокруг центрального окна в порядке приоритета
            // на основе направления, где окно находилось
            std::vector<std::pair<int, int>> slots;
            
            // Определяем приоритетное направление
            bool isRight = dx > 0;
            bool isLeft = dx < 0;
            bool isBottom = dy > 0;
            bool isTop = dy < 0;
            
            // Генерируем слоты в порядке приоритета
            if (isRight && isBottom) {
                // Правый нижний квадрант
                slots = {
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},  // Справа
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP},  // Снизу
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y + centerPlaced.h + GAP}, // Правый нижний угол
                    {centerPlaced.x - w - GAP, centerPlaced.y},               // Слева (запасной)
                    {centerPlaced.x, centerPlaced.y - h - GAP}                // Сверху (запасной)
                };
            } else if (isLeft && isBottom) {
                // Левый нижний квадрант
                slots = {
                    {centerPlaced.x - w - GAP, centerPlaced.y},               // Слева
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP},  // Снизу
                    {centerPlaced.x - w - GAP, centerPlaced.y + centerPlaced.h + GAP}, // Левый нижний угол
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},  // Справа (запасной)
                    {centerPlaced.x, centerPlaced.y - h - GAP}                // Сверху (запасной)
                };
            } else if (isRight && isTop) {
                // Правый верхний квадрант
                slots = {
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},  // Справа
                    {centerPlaced.x, centerPlaced.y - h - GAP},               // Сверху
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y - h - GAP}, // Правый верхний угол
                    {centerPlaced.x - w - GAP, centerPlaced.y},               // Слева (запасной)
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP}   // Снизу (запасной)
                };
            } else if (isLeft && isTop) {
                // Левый верхний квадрант
                slots = {
                    {centerPlaced.x - w - GAP, centerPlaced.y},               // Слева
                    {centerPlaced.x, centerPlaced.y - h - GAP},               // Сверху
                    {centerPlaced.x - w - GAP, centerPlaced.y - h - GAP},     // Левый верхний угол
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},  // Справа (запасной)
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP}   // Снизу (запасной)
                };
            } else if (isRight) {
                // Строго справа
                slots = {
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y - h - GAP},
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x, centerPlaced.y - h - GAP}
                };
            } else if (isLeft) {
                // Строго слева
                slots = {
                    {centerPlaced.x - w - GAP, centerPlaced.y},
                    {centerPlaced.x - w - GAP, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x - w - GAP, centerPlaced.y - h - GAP},
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x, centerPlaced.y - h - GAP}
                };
            } else if (isBottom) {
                // Строго снизу
                slots = {
                    {centerPlaced.x, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x - w - GAP, centerPlaced.y + centerPlaced.h + GAP},
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},
                    {centerPlaced.x - w - GAP, centerPlaced.y}
                };
            } else {
                // Строго сверху (или dx=0, dy=0)
                slots = {
                    {centerPlaced.x, centerPlaced.y - h - GAP},
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y - h - GAP},
                    {centerPlaced.x - w - GAP, centerPlaced.y - h - GAP},
                    {centerPlaced.x + centerPlaced.w + GAP, centerPlaced.y},
                    {centerPlaced.x - w - GAP, centerPlaced.y}
                };
            }
            
            // Пробуем слоты в порядке приоритета
            bool found = false;
            for (auto& slot : slots) {
                if (!intersects(slot.first, slot.second, w, h)) {
                    bestPos = {slot.first, slot.second};
                    found = true;
                    break;
                }
            }
            
            // Если не нашли место вокруг центрального, пробуем вокруг других
            if (!found) {
                long long minDist = LLONG_MAX;
                for (size_t pi = 1; pi < placed.size(); ++pi) {
                    const auto& pr = placed[pi];
                    std::vector<std::pair<int, int>> fallbackSlots = {
                        {pr.x + pr.w + GAP, pr.y},
                        {pr.x - w - GAP, pr.y},
                        {pr.x, pr.y + pr.h + GAP},
                        {pr.x, pr.y - h - GAP}
                    };
                    
                    for (auto& slot : fallbackSlots) {
                        if (!intersects(slot.first, slot.second, w, h)) {
                            long long dist = 1LL * slot.first * slot.first + 1LL * slot.second * slot.second;
                            if (dist < minDist) {
                                minDist = dist;
                                bestPos = {slot.first, slot.second};
                                found = true;
                            }
                        }
                    }
                }
            }
            
            // Последний запасной вариант
            if (!found && !placed.empty()) {
                const auto& last = placed.back();
                bestPos = {last.x + last.w + GAP, last.y};
            }
        }

        placed.push_back({bestPos.x, bestPos.y, w, h});
        finalPositions.push_back(bestPos);
    }

    // 6. Центрирование на экране
    int offsetX = screenCx;
    int offsetY = screenCy;

    // 7. Формирование анимации
    for (size_t i = 0; i < sortedList.size(); ++i) {
        GridAnimItem item;
        item.hwnd = sortedList[i].hwnd;
        item.startX = sortedList[i].baseX;
        item.startY = sortedList[i].baseY;
        item.endX = finalPositions[i].x + offsetX;
        item.endY = finalPositions[i].y + offsetY;
        item.width = sortedList[i].width;
        item.height = sortedList[i].height;
        
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
    RECT newRect;
    if (!GetWindowRect(newHwnd, &newRect)) return {0, 0};

    // Позиция нового окна в координатах холста
    int curCenterX = ((newRect.left + newRect.right) / 2) - camOffset.x;
    int curCenterY = ((newRect.top + newRect.bottom) / 2) - camOffset.y;

    struct Candidate {
        int x, y;
        long long distSq;
    };
    std::vector<Candidate> candidates;

    auto intersectsAny = [&](int cx, int cy, int cw, int ch) -> bool {
        for (const auto& s : snapshots) {
            int sx = s.baseX;
            int sy = s.baseY;
            int sw = s.width;
            int sh = s.height;
            if (!(cx + cw <= sx || cx >= sx + sw || cy + ch <= sy || cy >= sy + sh)) {
                return true;
            }
        }
        return false;
    };

    for (const auto& s : snapshots) {
        int sx = s.baseX;
        int sy = s.baseY;
        int sw = s.width;
        int sh = s.height;

        std::vector<std::pair<int, int>> slots = {
            {sx + sw, sy},
            {sx - newW, sy},
            {sx, sy + sh},
            {sx, sy - newH}
        };

        for (auto& slot : slots) {
            int cx = slot.first;
            int cy = slot.second;
            if (!intersectsAny(cx, cy, newW, newH)) {
                int slotCenterX = cx + newW / 2;
                int slotCenterY = cy + newH / 2;
                long long dx = slotCenterX - curCenterX;
                long long dy = slotCenterY - curCenterY;
                long long distSq = dx * dx + dy * dy;
                candidates.push_back({cx, cy, distSq});
            }
        }
    }

    if (!candidates.empty()) {
        auto minIt = std::min_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.distSq < b.distSq;
        });
        return {minIt->x, minIt->y};
    }

    int maxY = 0;
    for (const auto& s : snapshots) {
        int sy = s.baseY + s.height;
        if (sy > maxY) maxY = sy;
    }
    int fallbackY = maxY + 10;
    return {curCenterX - newW / 2, fallbackY};
}

POINT CalculateCameraTarget(int targetAbsX, int targetAbsY, int width, int height) {
    int screenCenterX = GetSystemMetrics(SM_CXSCREEN) / 2;
    int screenCenterY = GetSystemMetrics(SM_CYSCREEN) / 2;

    int desiredScreenX = screenCenterX - width / 2;
    int desiredScreenY = screenCenterY - height / 2;

    return {desiredScreenX - targetAbsX, desiredScreenY - targetAbsY};
}

void WorkerFunc() {
    auto lastWindowScan = std::chrono::steady_clock::now();
    auto lastDebugUpdate = std::chrono::steady_clock::now();
    
      while (!g_stop.load()) {
          bool camAnim = g_isCamAnim.load();
          bool gridAnim = g_gridAnim.active;

        // ============================================================
        // 1. ПЕРИОДИЧЕСКОЕ СКАНИРОВАНИЕ НОВЫХ ОКОН (Раз в 1 сек)
        // ============================================================
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWindowScan).count() >= 1000) {
            lastWindowScan = now;

            // Быстро копируем известные HWND
            std::vector<HWND> knownHwnds;
            {
                EnterCriticalSection(&g_lock);
                knownHwnds.reserve(g_snapshots.size() + g_newWindowsFound.size() + g_pendingWindows.size());
                for (const auto& s : g_snapshots) knownHwnds.push_back(s.hwnd);
                for (const auto& h : g_newWindowsFound) knownHwnds.push_back(h);
                for (const auto& pw : g_pendingWindows) knownHwnds.push_back(pw.hwnd);
                LeaveCriticalSection(&g_lock);
            }

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

                    bool isActive = false;
                    HWND fgWnd = GetForegroundWindow();
                    if (fgWnd == ctx.newHwnd || GetAncestor(fgWnd, GA_ROOTOWNER) == ctx.newHwnd) {
                        isActive = true;
                    }

                    EnterCriticalSection(&g_lock);
                    
                    if (isActive) {
                        // АКТИВНОЕ: Отложенная стыковка
                        POINT targetPos = FindBestSpot(ctx.newHwnd, w, h, g_snapshots, g_camOffset);
                        PendingWindow pw;
                        pw.hwnd = ctx.newHwnd;
                        pw.targetAbsX = targetPos.x;
                        pw.targetAbsY = targetPos.y;
                        pw.width = w;
                        pw.height = h;
                        g_pendingWindows.push_back(pw);

                        POINT camTarget = CalculateCameraTarget(targetPos.x, targetPos.y, w, h);
                        g_camAnimStart = g_camOffset;
                        g_camAnimTarget = camTarget;
                        g_camAnimTime = std::chrono::steady_clock::now();
                        g_isCamAnim.store(true);
                        g_autoCamAnim.store(true);

                        LeaveCriticalSection(&g_lock);

                        std::wstring noticeMsg = ctx.notice + L" -> DEFERRED DOCKING";
                        EnterCriticalSection(&g_debugLock);
                        g_newWindowNotice = noticeMsg;
                        LeaveCriticalSection(&g_debugLock);
                    } else {
                        // НЕАКТИВНОЕ: Сразу в сетку
                        g_newWindowsFound.push_back(ctx.newHwnd);
                        POINT targetPos = FindBestSpot(ctx.newHwnd, w, h, g_snapshots, g_camOffset);

                        GridAnimItem newItem;
                        newItem.hwnd = ctx.newHwnd;
                        newItem.startX = r.left;
                        newItem.startY = r.top;
                        // Важно: targetPos относительные, а для анимации нужны абсолютные старт/энд
                        newItem.endX = targetPos.x + g_camOffset.x;
                        newItem.endY = targetPos.y + g_camOffset.y;
                        newItem.width = w;
                        newItem.height = h;

                        g_gridAnim.items.push_back(newItem);
                        g_gridAnim.startTime = std::chrono::steady_clock::now();
                        g_gridAnim.active = true;

                        LeaveCriticalSection(&g_lock);

                        std::wstring noticeMsg = ctx.notice + L" -> ANIMATING TO GRID";
                        EnterCriticalSection(&g_debugLock);
                        g_newWindowNotice = noticeMsg;
                        LeaveCriticalSection(&g_debugLock);
                    }
                }
            }
        }

         // ============================================================
         // 2. ПОДГОТОВКА КАДРА (Сбор данных под локом)
         // ============================================================
         std::vector<WindowMoveOp> ops;
         std::vector<PendingWindow> localPending;
         std::vector<GridAnimItem> localGridItems;
         bool localGridActive = false;
         POINT localCamOffset = {0,0};
         bool localCamAnim = camAnim;
         POINT localCamStart = g_camAnimStart;
         POINT localCamTarget = g_camAnimTarget;
         auto localCamTime = g_camAnimTime;
        std::vector<WindowSnapshot> localSnapshots;

        EnterCriticalSection(&g_lock);

        localCamOffset = g_camOffset;
        
        // Копируем снимки для отрисовки
        localSnapshots = g_snapshots;

        // Обработка камеры
        if (localCamAnim) {
            auto animNow = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(animNow - localCamTime).count();
            float t = std::min(1.0f, (float)ms / CAM_ANIM_DURATION);
            float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t);
            
            localCamOffset.x = (int)(localCamStart.x + (localCamTarget.x - localCamStart.x) * ease);
            localCamOffset.y = (int)(localCamStart.y + (localCamTarget.y - localCamStart.y) * ease);
            
            // Обновляем глобальную камеру сразу, чтобы другие части видели актуальную
            g_camOffset = localCamOffset; 

            if (t >= 1.0f) {
                g_isCamAnim.store(false);
                localCamAnim = false; // Локальный флаг тоже сбрасываем
                
                if (!g_pendingWindows.empty()) {
                    g_gridAnim.items.clear();
                    g_gridAnim.startTime = std::chrono::steady_clock::now();
                    g_gridAnim.active = true;

                    for (const auto& pw : g_pendingWindows) {
                        if (!IsWindow(pw.hwnd)) continue;
                        RECT r;
                        if (!GetWindowRect(pw.hwnd, &r)) continue;

                        GridAnimItem item;
                        item.hwnd = pw.hwnd;
                        item.startX = r.left;
                        item.startY = r.top;
                        item.endX = pw.targetAbsX + localCamOffset.x;
                        item.endY = pw.targetAbsY + localCamOffset.y;
                        item.width = pw.width;
                        item.height = pw.height;
                        g_gridAnim.items.push_back(item);
                    }
                    g_pendingWindows.clear();
                    g_autoCamAnim.store(false);
                }
            }
         }

         // Копируем pending окна
         localPending = g_pendingWindows;

         // Копируем состояние сетки
         localGridActive = g_gridAnim.active;
         if (localGridActive) {
             localGridItems = g_gridAnim.items;
         }

          LeaveCriticalSection(&g_lock); // ОСВОБОЖДАЕМ ЛОК ЗДЕСЬ! Дальше только вычисления и рендер

          // ============================================================
          // 3. ВЫЧИСЛЕНИЕ ПОЗИЦИЙ И ОТРИСОВКА (БЕЗ ЛОКА)
          // ============================================================

          int screenCx = GetSystemMetrics(SM_CXSCREEN) / 2;
          int screenCy = GetSystemMetrics(SM_CYSCREEN) / 2;

         // 3.A Отрисовка Pending окон (висят в центре)
         for (const auto& pw : localPending) {
             if (!IsWindow(pw.hwnd)) continue;
             ops.push_back({pw.hwnd, screenCx - pw.width / 2, screenCy - pw.height / 2, pw.width, pw.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
         }

         // 3.B Отрисовка анимации сетки
         if (localGridActive) {
             auto animNow = std::chrono::steady_clock::now();
             auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(animNow - g_gridAnim.startTime).count();
             float t = std::min(1.0f, (float)ms / g_gridAnim.durationMs);
             float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t);

             for (const auto& item : localGridItems) {
                 if (!IsWindow(item.hwnd)) continue;
                 int curX = (int)(item.startX + (item.endX - item.startX) * ease);
                 int curY = (int)(item.startY + (item.endY - item.startY) * ease);
                 ops.push_back({item.hwnd, curX, curY, item.width, item.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
             }

             if (t >= 1.0f) {
                 EnterCriticalSection(&g_lock);
                 g_gridAnim.active = false;
                 for (const auto& item : g_gridAnim.items) {
                     if (!IsWindow(item.hwnd)) continue;
                     bool exists = false;
                     for (const auto& s : g_snapshots) {
                         if (s.hwnd == item.hwnd) { exists = true; break; }
                     }
                     if (!exists) {
                         int relativeX = item.endX - g_camOffset.x;
                         int relativeY = item.endY - g_camOffset.y;
                         g_snapshots.push_back({item.hwnd, relativeX, relativeY, item.width, item.height});
                     }
                     auto it = std::find(g_newWindowsFound.begin(), g_newWindowsFound.end(), item.hwnd);
                     if (it != g_newWindowsFound.end()) {
                         g_newWindowsFound.erase(it);
                     }
                 }
                 g_gridAnim.items.clear();
                 LeaveCriticalSection(&g_lock);
             }
         }

         // 3.C Обработка перетаскивания (delta-accumulation)
         bool cameraMoved = false; // Флаг: камера реально сдвинулась в этом кадре

         // ВАЖНО: Проверяем g_isDragging напрямую, а не через localDrag
         // Это предотвращает применение дельты после отпускания кнопки
         if (g_isDragging.load() && !g_gridAnim.active) {
             // Атомарно извлекаем накопленную дельту (exchange обнуляет аккумулятор)
             long dx = g_mouseDeltaX.exchange(0, std::memory_order_relaxed);
             long dy = g_mouseDeltaY.exchange(0, std::memory_order_relaxed);

             // Применяем дельту ТОЛЬКО если она ненулевая
             if (dx != 0 || dy != 0) {
                 EnterCriticalSection(&g_lock);
                 // ВАЖНО: Инвертируем дельту! Мышь вправо → камера влево
                 // Это создает эффект "тяги" контента за курсором
                 int newX = g_camOffset.x + (int)dx;
                 int newY = g_camOffset.y + (int)dy;
                 g_camOffset.x = std::max(-5000, std::min(newX, CANVAS_WIDTH + 5000));
                 g_camOffset.y = std::max(-5000, std::min(newY, CANVAS_HEIGHT + 5000));
                 localCamOffset = g_camOffset;
                 LeaveCriticalSection(&g_lock);
                 cameraMoved = true; // Камера сдвинулась!
             }
         }

         // 3.D Отрисовка обычных окон (если не идет анимация сетки)
         if (!localGridActive) {
             // Двигаем окна ТОЛЬКО если камера реально сдвинулась или идет анимация
             if (cameraMoved || localCamAnim) {
                 // КРИТИЧНО: Валидируем каждое окно перед использованием
                 // localSnapshots — это копия, но окна могли закрыться между копированием и использованием
                 for (auto& s : localSnapshots) {
                     // ЗАЩИТА: Проверяем существование окна ПЕРЕД использованием
                     if (!s.hwnd || !IsWindow(s.hwnd)) continue;

                     int targetX = s.baseX + localCamOffset.x;
                     int targetY = s.baseY + localCamOffset.y;

                     // Ограничиваем координаты
                     targetX = std::max(-5000, std::min(targetX, CANVAS_WIDTH + 5000));
                     targetY = std::max(-5000, std::min(targetY, CANVAS_HEIGHT + 5000));

                     // Двигаем окно без дополнительных проверок (для минимальной задержки)
                     ops.push_back({s.hwnd, targetX, targetY, s.width, s.height, SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE});
                 }
             }
         }

        if (!ops.empty()) ApplyMoves(ops);

        // ============================================================
        // 4. АНИМАЦИЯ ЗУМА
        // ============================================================
        if (g_zoomAnim.active) {
            auto animNow = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(animNow - g_zoomAnim.startTime).count();
            float t = std::min(1.0f, (float)ms / g_zoomAnim.durationMs);
            float ease = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t); // cubic ease-out
            
            // КРИТИЧНО: Быстро копируем данные под локом
            std::vector<ZoomAnimState::WindowState> localZoomWindows;
            EnterCriticalSection(&g_lock);
            localZoomWindows = g_zoomAnim.windows; // Быстрое копирование
            LeaveCriticalSection(&g_lock);
            
            // Вычисления БЕЗ ЛОКА
            std::vector<WindowMoveOp> zoomOps;
            zoomOps.reserve(localZoomWindows.size());
            
            for (const auto& ws : localZoomWindows) {
                if (!IsWindow(ws.hwnd)) continue;
                
                // Интерполируем позицию и размер
                int curX = (int)(ws.startX + (ws.targetX - ws.startX) * ease);
                int curY = (int)(ws.startY + (ws.targetY - ws.startY) * ease);
                int curW = (int)(ws.startW + (ws.targetW - ws.startW) * ease);
                int curH = (int)(ws.startH + (ws.targetH - ws.startH) * ease);
                
                zoomOps.push_back({ws.hwnd, curX, curY, curW, curH, SWP_NOZORDER|SWP_NOACTIVATE});
            }
            
            if (!zoomOps.empty()) ApplyMoves(zoomOps);
            
            // Завершение анимации
            if (t >= 1.0f) {
                EnterCriticalSection(&g_lock);
                g_zoomAnim.active = false;
                
                // Обновляем снапшоты реальными размерами после зума
                for (auto& s : g_snapshots) {
                    if (!IsWindow(s.hwnd)) continue;
                    RECT r;
                    if (GetWindowRect(s.hwnd, &r)) {
                        s.baseX = r.left - g_camOffset.x;
                        s.baseY = r.top - g_camOffset.y;
                        s.width = r.right - r.left;
                        s.height = r.bottom - r.top;
                    }
                }
                
                g_zoomAnim.windows.clear();
                
                LeaveCriticalSection(&g_lock);
                
                // Запускаем физику коллизий после завершения зума
                // Используем сохранённое направление
                bool wasZoomIn = g_lastZoomWasIn.load(std::memory_order_relaxed);
                ResolveCollisionsAfterZoom(wasZoomIn);
            }
        }
        
        // Проверка запланированной физики
        if (g_physicsScheduled.load(std::memory_order_relaxed)) {
            auto timeSinceLastZoom = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_lastZoomTime
            ).count();
            
            if (timeSinceLastZoom >= PHYSICS_DELAY_MS) {
                g_physicsScheduled.store(false, std::memory_order_relaxed);
                bool wasZoomIn = g_lastZoomWasIn.load(std::memory_order_relaxed);
                ResolveCollisionsAfterZoom(wasZoomIn);
            }
        }

        // ============================================================
        // 5. ОБНОВЛЕНИЕ DEBUG (Раз в 200мс)
        // ============================================================
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDebugUpdate).count() >= 200) {
            UpdateDebugWindow();
            lastDebugUpdate = now;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(g_config.threadSleepMs));
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

    // 2. СТАРТ ДВИЖЕНИЯ - инициализация delta-accumulation
    g_isCamAnim.store(false);
    g_isDragging.store(true);
    // g_panStartButton уже установлена в MouseHook перед вызовом StartDrag

    // Инициализируем систему дельт
    g_lastHookMousePos = p;
    g_mouseDeltaX.store(0, std::memory_order_relaxed);
    g_mouseDeltaY.store(0, std::memory_order_relaxed);
}

// Проверка лимитов масштабирования
bool CheckScaleLimits(int width, int height) {
    // Минимум: любая сторона должна быть >= 300px
    if (width < 300 || height < 300) {
        return false;
    }

    // Максимум: любая сторона должна быть <= 98% от размера экрана
    int maxWidth = (int)(GetSystemMetrics(SM_CXSCREEN) * 0.98f);
    int maxHeight = (int)(GetSystemMetrics(SM_CYSCREEN) * 0.98f);

    if (width > maxWidth || height > maxHeight) {
        return false;
    }

    return true;
}

void Zoom(float scale) {
    if (g_gridAnim.active) return;
    
    EnterCriticalSection(&g_lock);
    if (g_snapshots.empty()) { 
        LeaveCriticalSection(&g_lock); 
        return; 
    }
    
    // Запоминаем направление зума
    bool isZoomIn = (scale > 1.0f);
    g_lastZoomWasIn.store(isZoomIn, std::memory_order_relaxed);
    
    // Вычисляем изменение масштаба (например, 1.10 → +0.10, 0.90 → -0.10)
    float scaleChange = scale - 1.0f;
    
    // Находим центральное окно и его центр
    HWND centerHwnd = g_centerWindow;
    POINT centerPoint = {0, 0};
    bool foundCenter = false;
    
    for (const auto& s : g_snapshots) {
        if (s.hwnd == centerHwnd && IsWindow(s.hwnd)) {
            RECT r;
            if (GetWindowRect(s.hwnd, &r)) {
                centerPoint.x = r.left + (r.right - r.left) / 2;
                centerPoint.y = r.top + (r.bottom - r.top) / 2;
                foundCenter = true;
                break;
            }
        }
    }
    
    // Если центральное окно не найдено, используем центр экрана
    if (!foundCenter) {
        centerPoint.x = GetSystemMetrics(SM_CXSCREEN) / 2;
        centerPoint.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    }
    
    // Применяем масштабирование с учётом долга
    std::vector<WindowMoveOp> zoomOps;
    zoomOps.reserve(g_snapshots.size());
    
    for (auto& s : g_snapshots) {
        if (!IsWindow(s.hwnd)) continue;
        
        RECT r;
        if (!GetWindowRect(s.hwnd, &r)) continue;
        
        // Текущий размер и центр окна
        int currentW = r.right - r.left;
        int currentH = r.bottom - r.top;
        int currentCenterX = r.left + currentW / 2;
        int currentCenterY = r.top + currentH / 2;
        
        // Вектор от центральной точки к центру окна
        int vecX = currentCenterX - centerPoint.x;
        int vecY = currentCenterY - centerPoint.y;
        
        // Пробуем применить зум с учётом долга
        float newDebt = s.zoomDebt + scaleChange;
        
        // Вычисляем желаемый размер
        int targetW = (int)(s.width * (1.0f + newDebt));
        int targetH = (int)(s.height * (1.0f + newDebt));
        
        // Проверяем лимиты
        bool canScale = CheckScaleLimits(targetW, targetH);
        
        if (canScale) {
            // Окно может масштабироваться — применяем зум и обнуляем долг
            s.zoomDebt = 0.0f;
            s.width = targetW;
            s.height = targetH;
            
            // КЛЮЧЕВОЕ ИЗМЕНЕНИЕ: Масштабируем вектор от центральной точки
            int newVecX = (int)(vecX * scale);
            int newVecY = (int)(vecY * scale);
            
            // Новый центр окна
            int newCenterX = centerPoint.x + newVecX;
            int newCenterY = centerPoint.y + newVecY;
            
            // Новая позиция окна (верхний левый угол)
            int targetX = newCenterX - targetW / 2;
            int targetY = newCenterY - targetH / 2;
            
            zoomOps.push_back({s.hwnd, targetX, targetY, targetW, targetH, SWP_NOZORDER|SWP_NOACTIVATE});
        } else {
            // Окно достигло лимита
            // Проверяем, можем ли мы погасить долг
            if ((newDebt > 0 && s.zoomDebt < 0) || (newDebt < 0 && s.zoomDebt > 0)) {
                // Зум в противоположную сторону — гасим долг
                if (std::abs(newDebt) < std::abs(s.zoomDebt)) {
                    // Долг больше текущего зума — частично гасим
                    s.zoomDebt = newDebt;
                } else {
                    // Долг погашен полностью, остаток идёт в масштабирование
                    float remainder = newDebt - s.zoomDebt;
                    s.zoomDebt = 0.0f;
                    
                    // Пробуем применить остаток
                    int finalW = (int)(s.width * (1.0f + remainder));
                    int finalH = (int)(s.height * (1.0f + remainder));
                    
                    if (CheckScaleLimits(finalW, finalH)) {
                        s.width = finalW;
                        s.height = finalH;
                        
                        // Масштабируем вектор с учётом остатка
                        float remainderScale = 1.0f + remainder;
                        int newVecX = (int)(vecX * remainderScale);
                        int newVecY = (int)(vecY * remainderScale);
                        
                        int newCenterX = centerPoint.x + newVecX;
                        int newCenterY = centerPoint.y + newVecY;
                        
                        int targetX = newCenterX - finalW / 2;
                        int targetY = newCenterY - finalH / 2;
                        
                        zoomOps.push_back({s.hwnd, targetX, targetY, finalW, finalH, SWP_NOZORDER|SWP_NOACTIVATE});
                    } else {
                        // Остаток тоже не влезает — накапливаем новый долг
                        s.zoomDebt = remainder;
                    }
                }
            } else {
                // Зум в ту же сторону — накапливаем долг
                s.zoomDebt = newDebt;
            }
        }
    }
    
    LeaveCriticalSection(&g_lock);
    
    // Применяем изменения мгновенно
    if (!zoomOps.empty()) {
        ApplyMoves(zoomOps);
    }
    
    // Обновляем снапшоты (позиции, без размеров — они уже обновлены)
    EnterCriticalSection(&g_lock);
    for (auto& s : g_snapshots) {
        if (!IsWindow(s.hwnd)) continue;
        RECT r;
        if (GetWindowRect(s.hwnd, &r)) {
            s.baseX = r.left - g_camOffset.x;
            s.baseY = r.top - g_camOffset.y;
            // Размеры уже обновлены выше, не перезаписываем
        }
    }
    LeaveCriticalSection(&g_lock);
    
    // Планируем запуск физики через 1 секунду
    g_lastZoomTime = std::chrono::steady_clock::now();
    g_physicsScheduled.store(true, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ФИЗИКА КОЛЛИЗИЙ
// ═══════════════════════════════════════════════════════════════════════════════

// Структура для работы с окнами в физике
struct PhysicsWindow {
    HWND hwnd;
    int x, y, w, h;
    int centerX, centerY;
    long long distFromCenter; // Расстояние от центрального окна
};

// Проверка пересечения двух прямоугольников с учётом зазора
bool CheckCollision(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2, int gap) {
    return !(x1 + w1 + gap <= x2 || x1 >= x2 + w2 + gap ||
             y1 + h1 + gap <= y2 || y1 >= y2 + h2 + gap);
}

// Вычисление минимального зазора между двумя окнами
int CalculateGap(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    // Горизонтальное расстояние
    int hGap = INT_MAX;
    if (x1 + w1 <= x2) hGap = x2 - (x1 + w1);
    else if (x2 + w2 <= x1) hGap = x1 - (x2 + w2);
    else hGap = 0; // Перекрываются по X
    
    // Вертикальное расстояние
    int vGap = INT_MAX;
    if (y1 + h1 <= y2) vGap = y2 - (y1 + h1);
    else if (y2 + h2 <= y1) vGap = y1 - (y2 + h2);
    else vGap = 0; // Перекрываются по Y
    
    // Если перекрываются по обеим осям — коллизия (отрицательный зазор)
    if (hGap == 0 && vGap == 0) {
        // Вычисляем глубину проникновения
        int overlapX = std::min(x1 + w1, x2 + w2) - std::max(x1, x2);
        int overlapY = std::min(y1 + h1, y2 + h2) - std::max(y1, y2);
        return -std::min(overlapX, overlapY);
    }
    
    // Возвращаем минимальный зазор
    return std::min(hGap, vGap);
}

// Вычисление вектора разрешения коллизии (кратчайший путь)
void CalculateSeparationVector(int x1, int y1, int w1, int h1, 
                                int x2, int y2, int w2, int h2,
                                int& outDx, int& outDy) {
    const int GAP = 2;
    
    // Вычисляем перекрытие по каждой оси
    int overlapX = std::min(x1 + w1, x2 + w2) - std::max(x1, x2);
    int overlapY = std::min(y1 + h1, y2 + h2) - std::max(y1, y2);
    
    // Выбираем ось с минимальным перекрытием (кратчайший путь)
    if (overlapX < overlapY) {
        // Разделяем по X
        if (x1 < x2) {
            outDx = -(overlapX + GAP); // Двигаем влево
        } else {
            outDx = overlapX + GAP; // Двигаем вправо
        }
        outDy = 0;
    } else {
        // Разделяем по Y
        outDx = 0;
        if (y1 < y2) {
            outDy = -(overlapY + GAP); // Двигаем вверх
        } else {
            outDy = overlapY + GAP; // Двигаем вниз
        }
    }
}

// Рекурсивное выталкивание окна (цепная реакция)
bool PushWindowRecursive(size_t windowIdx, std::vector<PhysicsWindow>& windows, 
                         std::vector<bool>& processed, int depth = 0) {
    const int MAX_DEPTH = 10; // Защита от бесконечной рекурсии
    if (depth > MAX_DEPTH) return false;
    if (processed[windowIdx]) return true;
    
    processed[windowIdx] = true;
    PhysicsWindow& win = windows[windowIdx];
    
    // Собираем все коллизии
    struct CollisionInfo {
        size_t otherIdx;
        int dx, dy;
    };
    std::vector<CollisionInfo> collisions;
    
    for (size_t i = 0; i < windows.size(); ++i) {
        if (i == windowIdx) continue;
        
        const PhysicsWindow& other = windows[i];
        if (CheckCollision(win.x, win.y, win.w, win.h, 
                          other.x, other.y, other.w, other.h, 2)) {
            int dx, dy;
            CalculateSeparationVector(win.x, win.y, win.w, win.h,
                                     other.x, other.y, other.w, other.h,
                                     dx, dy);
            collisions.push_back({i, dx, dy});
        }
    }
    
    if (collisions.empty()) return true;
    
    // Сначала рекурсивно выталкиваем все окна, с которыми есть коллизия
    for (const auto& col : collisions) {
        if (!PushWindowRecursive(col.otherIdx, windows, processed, depth + 1)) {
            return false;
        }
    }
    
    // Теперь вычисляем суммарный вектор отталкивания
    int totalDx = 0, totalDy = 0;
    for (const auto& col : collisions) {
        totalDx += col.dx;
        totalDy += col.dy;
    }
    
    // Применяем смещение
    win.x += totalDx;
    win.y += totalDy;
    win.centerX = win.x + win.w / 2;
    win.centerY = win.y + win.h / 2;
    
    return true;
}

// Притягивание окна к центру при уменьшении
void PullWindowToCenter(PhysicsWindow& win, const std::vector<PhysicsWindow>& windows, 
                        int centerX, int centerY) {
    const int GAP = 2;
    
    // Вычисляем направление к центру
    int dx = centerX - win.centerX;
    int dy = centerY - win.centerY;
    
    if (dx == 0 && dy == 0) return; // Уже в центре
    
    // Нормализуем направление
    float len = std::sqrt(1.0f * dx * dx + 1.0f * dy * dy);
    float ndx = dx / len;
    float ndy = dy / len;
    
    // Пробуем двигаться к центру пиксель за пикселем
    int maxSteps = (int)len;
    for (int step = 0; step < maxSteps; ++step) {
        int newX = win.x + (int)(ndx * (step + 1));
        int newY = win.y + (int)(ndy * (step + 1));
        
        // Проверяем минимальный зазор до всех окон
        int minGap = INT_MAX;
        for (const auto& other : windows) {
            if (other.hwnd == win.hwnd) continue;
            int gap = CalculateGap(newX, win.y, win.w, win.h,
                                  other.x, other.y, other.w, other.h);
            if (gap < minGap) minGap = gap;
        }
        
        // Если зазор стал меньше 2px — останавливаемся
        if (minGap < GAP) {
            if (step > 0) {
                win.x += (int)(ndx * step);
                win.y += (int)(ndy * step);
                win.centerX = win.x + win.w / 2;
                win.centerY = win.y + win.h / 2;
            }
            return;
        }
    }
    
    // Если дошли сюда — можем двигаться на всё расстояние
    win.x += dx;
    win.y += dy;
    win.centerX = centerX;
    win.centerY = centerY;
}

// Разрешение коллизий после зума
void ResolveCollisionsAfterZoom(bool isZoomIn) {
    EnterCriticalSection(&g_lock);
    
    if (g_snapshots.empty()) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    
    const int GAP = 2;
    HWND centerHwnd = g_centerWindow;
    
    // Создаём список окон
    std::vector<PhysicsWindow> physWindows;
    physWindows.reserve(g_snapshots.size());
    
    size_t centerPhysIdx = SIZE_MAX;
    
    for (size_t i = 0; i < g_snapshots.size(); ++i) {
        if (!IsWindow(g_snapshots[i].hwnd)) continue;
        
        PhysicsWindow pw;
        pw.hwnd = g_snapshots[i].hwnd;
        pw.x = g_snapshots[i].baseX;
        pw.y = g_snapshots[i].baseY;
        pw.w = g_snapshots[i].width;
        pw.h = g_snapshots[i].height;
        pw.centerX = pw.x + pw.w / 2;
        pw.centerY = pw.y + pw.h / 2;
        
        if (pw.hwnd == centerHwnd) {
            centerPhysIdx = physWindows.size();
            pw.distFromCenter = 0;
        } else if (centerPhysIdx != SIZE_MAX) {
            // Вычисляем расстояние от центрального окна
            const PhysicsWindow& centerWin = physWindows[centerPhysIdx];
            long long dx = pw.centerX - centerWin.centerX;
            long long dy = pw.centerY - centerWin.centerY;
            pw.distFromCenter = dx * dx + dy * dy;
        }
        
        physWindows.push_back(pw);
    }
    
    if (centerPhysIdx == SIZE_MAX) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    
    bool hadChanges = false;
    
    if (isZoomIn) {
        // ПРИ УВЕЛИЧЕНИИ: только разрешаем коллизии
        for (size_t i = 0; i < physWindows.size(); ++i) {
            for (size_t j = i + 1; j < physWindows.size(); ++j) {
                PhysicsWindow& win1 = physWindows[i];
                PhysicsWindow& win2 = physWindows[j];
                
                if (CheckCollision(win1.x, win1.y, win1.w, win1.h,
                                 win2.x, win2.y, win2.w, win2.h, GAP)) {
                    hadChanges = true;
                    
                    int dx, dy;
                    CalculateSeparationVector(win1.x, win1.y, win1.w, win1.h,
                                            win2.x, win2.y, win2.w, win2.h,
                                            dx, dy);
                    
                    bool win1IsCenter = (i == centerPhysIdx);
                    bool win2IsCenter = (j == centerPhysIdx);
                    
                    if (win1IsCenter && !win2IsCenter) {
                        win2.x -= dx;
                        win2.y -= dy;
                    } else if (win2IsCenter && !win1IsCenter) {
                        win1.x += dx;
                        win1.y += dy;
                    } else if (!win1IsCenter && !win2IsCenter) {
                        win1.x += dx / 2;
                        win1.y += dy / 2;
                        win2.x -= dx / 2;
                        win2.y -= dy / 2;
                    }
                    
                    win1.centerX = win1.x + win1.w / 2;
                    win1.centerY = win1.y + win1.h / 2;
                    win2.centerX = win2.x + win2.w / 2;
                    win2.centerY = win2.y + win2.h / 2;
                }
            }
        }
    } else {
        // ПРИ УМЕНЬШЕНИИ: итеративно притягиваем от ближайших к дальним
        // Вычисляем расстояния для всех окон
        for (size_t i = 0; i < physWindows.size(); ++i) {
            if (i == centerPhysIdx) continue;
            
            const PhysicsWindow& centerWin = physWindows[centerPhysIdx];
            long long dx = physWindows[i].centerX - centerWin.centerX;
            long long dy = physWindows[i].centerY - centerWin.centerY;
            physWindows[i].distFromCenter = dx * dx + dy * dy;
        }
        
        // Сортируем по расстоянию (ближайшие первыми)
        std::vector<size_t> indices;
        for (size_t i = 0; i < physWindows.size(); ++i) {
            if (i != centerPhysIdx) indices.push_back(i);
        }
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            return physWindows[a].distFromCenter < physWindows[b].distFromCenter;
        });
        
        // Притягиваем по очереди
        const PhysicsWindow& centerWin = physWindows[centerPhysIdx];
        for (size_t idx : indices) {
            PhysicsWindow& win = physWindows[idx];
            
            // Вычисляем направление к центру
            int dx = centerWin.centerX - win.centerX;
            int dy = centerWin.centerY - win.centerY;
            
            if (dx == 0 && dy == 0) continue;
            
            // Нормализуем
            float len = std::sqrt(1.0f * dx * dx + 1.0f * dy * dy);
            float ndx = dx / len;
            float ndy = dy / len;
            
            // Пробуем двигаться к центру пиксель за пикселем
            int maxSteps = (int)len;
            for (int step = 1; step <= maxSteps; ++step) {
                int newX = win.x + (int)(ndx * step);
                int newY = win.y + (int)(ndy * step);
                
                // Проверяем коллизии со всеми окнами
                bool hasCollision = false;
                for (const auto& other : physWindows) {
                    if (other.hwnd == win.hwnd) continue;
                    if (CheckCollision(newX, newY, win.w, win.h,
                                     other.x, other.y, other.w, other.h, GAP)) {
                        hasCollision = true;
                        break;
                    }
                }
                
                if (hasCollision) {
                    // Применяем предыдущий шаг (если был)
                    if (step > 1) {
                        win.x += (int)(ndx * (step - 1));
                        win.y += (int)(ndy * (step - 1));
                        win.centerX = win.x + win.w / 2;
                        win.centerY = win.y + win.h / 2;
                        hadChanges = true;
                    }
                    break;
                }
                
                // Если дошли до конца — применяем полное смещение
                if (step == maxSteps) {
                    win.x = newX;
                    win.y = newY;
                    win.centerX = win.x + win.w / 2;
                    win.centerY = win.y + win.h / 2;
                    hadChanges = true;
                }
            }
        }
    }
    
    if (!hadChanges) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    
    // Применяем результаты через анимацию
    g_gridAnim.items.clear();
    g_gridAnim.items.reserve(physWindows.size());
    
    for (const auto& pw : physWindows) {
        RECT r;
        if (!GetWindowRect(pw.hwnd, &r)) continue;
        
        GridAnimItem item;
        item.hwnd = pw.hwnd;
        item.startX = r.left;
        item.startY = r.top;
        item.endX = pw.x + g_camOffset.x;
        item.endY = pw.y + g_camOffset.y;
        item.width = pw.w;
        item.height = pw.h;
        
        g_gridAnim.items.push_back(item);
        
        // Обновляем снапшот
        for (auto& s : g_snapshots) {
            if (s.hwnd == pw.hwnd) {
                s.baseX = pw.x;
                s.baseY = pw.y;
                break;
            }
        }
    }
    
    if (!g_gridAnim.items.empty()) {
        g_gridAnim.startTime = std::chrono::steady_clock::now();
        g_gridAnim.durationMs = 400; // Плавная анимация физики
        g_gridAnim.active = true;
    }
    
    LeaveCriticalSection(&g_lock);
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
            
            if (g_bindingPanKey) {
                g_panKey = newKey;
                g_config.panKey = g_panKey;
            } else {
                g_activateKey = newKey;
                g_config.activateKey = g_activateKey;
            }
            SaveConfig(g_config);
            
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
         // А. Накапливаем дельту движения мыши (только на WM_MOUSEMOVE)
         if (wParam == WM_MOUSEMOVE) {
             // Вычисляем дельту относительно последней обработанной позиции
             long dx = m->pt.x - g_lastHookMousePos.x;
             long dy = m->pt.y - g_lastHookMousePos.y;

             // Атомарно добавляем в аккумулятор
             g_mouseDeltaX.fetch_add(dx, std::memory_order_relaxed);
             g_mouseDeltaY.fetch_add(dy, std::memory_order_relaxed);

             // Обновляем последнюю позицию
             g_lastHookMousePos = m->pt;
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
             // Обнуляем аккумулятор дельт (важно!)
             g_mouseDeltaX.store(0, std::memory_order_relaxed);
             g_mouseDeltaY.store(0, std::memory_order_relaxed);

             // Обновляем снапшоты окон с новыми позициями
             EnterCriticalSection(&g_lock);
             for (auto& s : g_snapshots) {
                 if (IsWindow(s.hwnd)) {
                     RECT r;
                     if (GetWindowRect(s.hwnd, &r)) {
                         s.baseX = r.left - g_camOffset.x;
                         s.baseY = r.top - g_camOffset.y;
                         s.width = r.right - r.left;
                         s.height = r.bottom - r.top;
                     }
                 }
             }
             LeaveCriticalSection(&g_lock);

             g_isDragging.store(false);
             g_panStartButton.store(0);

             // НЕ блокируем событие отпускания - пропускаем его дальше
             return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
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
            // Фиксированное масштабирование: шаг 10%
            Zoom(GET_WHEEL_DELTA_WPARAM(m->mouseData) > 0 ? 1.10f : 0.90f);
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
             if (g_bindingPanKey) {
                 g_panKey = k->vkCode;
                 g_config.panKey = g_panKey;
             } else {
                 g_activateKey = k->vkCode;
                 g_config.activateKey = g_activateKey;
             }
             SaveConfig(g_config);
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
        // Сохраняем конфигурацию перед выходом (страховка)
        g_config.activateKey = g_activateKey;
        g_config.panKey = g_panKey;
        SaveConfig(g_config);

        g_stop.store(true);
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
        g_mouseHook = NULL; g_kbHook = NULL;
        if (g_worker.joinable()) g_worker.join();
        DeleteCriticalSection(&g_lock);
        DeleteCriticalSection(&g_debugLock);

        // Освобождаем мьютекс единственного экземпляра
        if (g_hSingleInstanceMutex) {
            CloseHandle(g_hSingleInstanceMutex);
            g_hSingleInstanceMutex = NULL;
        }

        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    // ═══════════════════════════════════════════════════════════════════════════════
    // ПРОВЕРКА ЕДИНОГО ЭКЗЕМПЛЯРА — ПЕРВЫМ ДЕЛОМ
    // ═══════════════════════════════════════════════════════════════════════════════
    if (!CheckSingleInstance()) {
        ActivateExistingInstance();
        return 0;
    }

    if (!IsAdmin()) RunAsAdmin();

    // Загружаем конфигурацию ДО создания окон/хуков
    g_config = LoadConfig();
    g_activateKey = g_config.activateKey;
    g_panKey = g_config.panKey;

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