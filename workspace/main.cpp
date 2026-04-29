#include <windows.h>
#include <windowsx.h> // Для GET_X_LPARAM и GET_Y_LPARAM
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>

// Подключаем SFML
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>

// Структура для хранения информации об окне
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    int x, y, w, h;
};

// Глобальные переменные
std::vector<WindowInfo> foundWindows;
sf::Vector2f cameraPos(0.0f, 0.0f);
float zoomLevel = 1.0f;
bool isDragging = false;
sf::Vector2i lastMousePos;

// Функция обратного вызова для перечисления окон
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    // Фильтруем окна
    if (!IsWindowVisible(hwnd)) return TRUE;
    
    // Получаем заголовок
    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);
    
    // Пропускаем окна без заголовка (часто это системные элементы)
    if (wcslen(title) == 0) return TRUE;
    
    // Пропускаем само консольное окно или окно нашего приложения (если бы оно было видно на этом этапе)
    // Но так как мы создадим графическое окно позже, пока просто собираем всё.

    RECT rect;
    if (GetWindowRect(hwnd, &rect)) {
        // Пропускаем окна за пределами разумного (например, панель задач обычно тоже имеет заголовок, но нам она не нужна для перемещения)
        // Панель задач обычно имеет класс "Shell_TrayWnd", можно отфильтровать по классу, если нужно.
        
        WindowInfo info;
        info.hwnd = hwnd;
        info.title = title;
        info.x = rect.left;
        info.y = rect.top;
        info.w = rect.right - rect.left;
        info.h = rect.bottom - rect.top;
        
        // Добавляем в список
        foundWindows.push_back(info);
    }
    return TRUE;
}

void scanWindows() {
    foundWindows.clear();
    EnumWindows(EnumWindowsProc, 0);
    
    std::wcout << L"=== Сканирование окон ===" << std::endl;
    std::wcout << L"Найдено окон: " << foundWindows.size() << std::endl;
    for (const auto& win : foundWindows) {
        std::wcout << L"Окно: " << win.title << L" | Pos: (" 
                   << win.x << L", " << win.y << L") | Size: " 
                   << win.w << L"x" << win.h << std::endl;
    }
    std::wcout << L"=========================" << std::endl;
}

int main() {
    // Проверка прав администратора (упрощенная)
    BOOL isAdmin = FALSE;
    PSID administratorsGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administratorsGroup)) {
        CheckTokenMembership(NULL, administratorsGroup, &isAdmin);
        FreeSid(administratorsGroup);
    }

    if (!isAdmin) {
        std::wcout << L"[WARN] Программа запущена НЕ от имени администратора." << std::endl;
        std::wcout << L"[INFO] Некоторые окна могут быть не видны или недоступны для управления." << std::endl;
    } else {
        std::wcout << L"[OK] Запуск с правами администратора." << std::endl;
    }

    // Сканируем окна перед созданием графического интерфейса
    scanWindows();

    // Создаем окно SFML
    sf::VideoMode desktopMode = sf::VideoMode::getDesktopMode();
    sf::RenderWindow window(sf::VideoMode(desktopMode.width, desktopMode.height, 32), 
                            "WCWM - Canvas", sf::Style::None); // Полноэкранное без рамок
    
    // Делаем окно прозрачным для кликов (Click-through), когда мы не двигаем камеру
    // В Windows это делается через SetWindowLong и WS_EX_TRANSPARENT
    HWND hWnd = window.getSystemHandle();
    SetWindowLong(hWnd, GWL_EXSTYLE, GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA); // Полностью видимое, но прозрачное для кликов

    // Основной цикл
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed)
                window.close();
            
            // Обработка клавиши ESC для выхода
            if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Escape) {
                window.close();
            }
        }

        // Логика управления камерой
        // Проверяем состояние мыши и клавиш напрямую, так как WS_EX_TRANSPARENT может мешать событиям мыши в обычном режиме
        // НО: С WS_EX_TRANSPARENT события мыши проходят СКВОЗЬ окно. 
        // Чтобы ловить мышь ДЛЯ себя, нам нужно временно убирать флаг TRANSPARENT или использовать глобальные хуки.
        // ПРОСТОЙ ВАРИАНТ ДЛЯ ТЕСТА: Зажимаем ПРОБЕЛ, чтобы активировать управление.
        // В этот момент мы убираем прозрачность для ввода.
        
        bool controlMode = (sf::Keyboard::isKeyPressed(sf::Keyboard::Space));

        if (controlMode) {
            // Включаем прием ввода (убираем TRANSPARENT)
            LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
            if (exStyle & WS_EX_TRANSPARENT) {
                SetWindowLong(hWnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
            }

            // Движение камеры средней кнопкой мыши
            if (sf::Mouse::isButtonPressed(sf::Mouse::Middle)) {
                sf::Vector2i currentMouse = sf::Mouse::getPosition(window);
                if (isDragging) {
                    float dx = (currentMouse.x - lastMousePos.x) / zoomLevel;
                    float dy = (currentMouse.y - lastMousePos.y) / zoomLevel;
                    cameraPos.x -= dx;
                    cameraPos.y -= dy;
                }
                lastMousePos = currentMouse;
                isDragging = true;
            } else {
                isDragging = false;
            }

            // Зум колесиком
            // Примечание: события колеса в pollEvent могут не приходить корректно при изменении стилей на лету,
            // но попробуем через event или просто проверку (SFML лучше работает с событиями для колеса)
            // Здесь упростим: зум сделаем через событие, если получится, или оставим пока без зума для стабильности теста.
        } else {
            // Возвращаем прозрачность для кликов
            LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
            if (!(exStyle & WS_EX_TRANSPARENT)) {
                SetWindowLong(hWnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
            }
            isDragging = false;
        }

        // Отрисовка
        window.clear(sf::Color(20, 20, 20, 200)); // Темный полупрозрачный фон

        sf::View view = window.getView();
        // Центрируем камеру на виртуальном холсте
        // Реальные координаты экрана: (0,0) - верхний лево.
        // Мы хотим сдвинуть мир так, чтобы cameraPos была в центре экрана.
        
        float centerX = desktopMode.width / 2.0f;
        float centerY = desktopMode.height / 2.0f;
        
        view.setCenter(centerX + cameraPos.x * zoomLevel, centerY + cameraPos.y * zoomLevel);
        view.setSize(desktopMode.width / zoomLevel, desktopMode.height / zoomLevel);
        window.setView(view);

        // Рисуем сетку (бесконечную визуально)
        // Оптимизация: рисуем только то, что в поле зрения
        sf::FloatRect visibleArea = view.getViewport(); // Это нормализованные координаты, нам нужны мировые
        // Получаем границы видимости в мировых координатах
        sf::Vector2f topLeft = window.mapPixelToCoords(sf::IntRect(0, 0, desktopMode.width, desktopMode.height).getPosition(), view);
        sf::Vector2f bottomRight = window.mapPixelToCoords(sf::IntRect(0, 0, desktopMode.width, desktopMode.height).getSize(), view);

        float gridSize = 100.0f;
        // Округляем начало сетки до ближайшей ячейки
        float startX = floor(topLeft.x / gridSize) * gridSize;
        float startY = floor(topLeft.y / gridSize) * gridSize;

        sf::VertexArray gridLines(sf::Lines);
        // Вертикальные линии
        for (float x = startX; x < bottomRight.x; x += gridSize) {
            gridLines.append(sf::Vertex(sf::Vector2f(x, topLeft.y), sf::Color(50, 50, 50)));
            gridLines.append(sf::Vertex(sf::Vector2f(x, bottomRight.y), sf::Color(50, 50, 50)));
        }
        // Горизонтальные линии
        for (float y = startY; y < bottomRight.y; y += gridSize) {
            gridLines.append(sf::Vertex(sf::Vector2f(topLeft.x, y), sf::Color(50, 50, 50)));
            gridLines.append(sf::Vertex(sf::Vector2f(bottomRight.x, y), sf::Color(50, 50, 50)));
        }
        window.draw(gridLines);

        // Рисуем прямоугольники вместо реальных окон (симуляция)
        for (const auto& win : foundWindows) {
            // Создаем форму
            sf::RectangleShape shape(sf::Vector2f(win.w, win.h));
            shape.setPosition(win.x, win.y);
            shape.setFillColor(sf::Color(0, 100, 200, 100)); // Полупрозрачный синий
            shape.setOutlineColor(sf::Color::White);
            shape.setOutlineThickness(2.0f);
            
            window.draw(shape);
            
            // Можно добавить текст (заголовок), но для этого нужен шрифт. Пока пропустим для простоты.
        }

        window.display();
    }

    return 0;
}