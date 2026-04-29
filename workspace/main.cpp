#include <windows.h>
#include <vector>
#include <string>
#include <cmath>
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <SFML/System.hpp>

struct WindowInfo {
    HWND hWnd;
    std::string title;
    int x, y, width, height;
};

std::vector<WindowInfo> windows;

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam) {
    if (!IsWindowVisible(hWnd)) return TRUE;
    if (GetWindowTextLength(hWnd) == 0) return TRUE;
    
    RECT rect;
    if (GetWindowRect(hWnd, &rect)) {
        WindowInfo info;
        info.hWnd = hWnd;
        
        char buffer[512];
        GetWindowTextA(hWnd, buffer, 512);
        info.title = std::string(buffer);
        
        info.x = rect.left;
        info.y = rect.top;
        info.width = rect.right - rect.left;
        info.height = rect.bottom - rect.top;
        
        windows.push_back(info);
    }
    return TRUE;
}

int main() {
    // Сканируем окна
    EnumWindows(EnumWindowsProc, 0);
    
    // В SFML 2.x десктоп режим получается так:
    sf::VideoMode desktopMode = sf::VideoMode::getDesktopMode();
    
    // Создаем окно на весь экран
    // В SFML 2.x конструктор принимает (width, height, bitsPerPixel)
    sf::RenderWindow window(sf::VideoMode(desktopMode.width, desktopMode.height, 32), 
                            "WCWM - Canvas", sf::Style::None);
    
    // Делаем окно прозрачным для кликов
    HWND hWnd = window.getSystemHandle();
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, GetWindowLongPtr(hWnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
    
    sf::View view(window.getDefaultView());
    sf::Vector2f cameraPos(0, 0);
    float zoomLevel = 1.0f;
    
    sf::Vector2i lastMousePos;
    bool isDragging = false;
    
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                window.close();
            }
            // В SFML 2.x доступ к коду клавиши через event.key.code
            if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Escape) {
                window.close();
            }
        }
        
        // Управление камерой
        bool controlMode = sf::Keyboard::isKeyPressed(sf::Keyboard::Space);
        
        if (controlMode) {
            if (sf::Mouse::isButtonPressed(sf::Mouse::Middle)) {
                if (!isDragging) {
                    lastMousePos = sf::Mouse::getPosition(window);
                    isDragging = true;
                } else {
                    sf::Vector2i currentMousePos = sf::Mouse::getPosition(window);
                    sf::Vector2i delta = currentMousePos - lastMousePos;
                    
                    cameraPos.x -= delta.x * zoomLevel;
                    cameraPos.y -= delta.y * zoomLevel;
                    
                    lastMousePos = currentMousePos;
                }
            } else {
                isDragging = false;
            }
        }
        
        // Обновление камеры
        float centerX = static_cast<float>(desktopMode.width) / 2.0f;
        float centerY = static_cast<float>(desktopMode.height) / 2.0f;
        
        // В SFML 2.x setCenter принимает два аргумента (x, y) или вектор
        view.setCenter(centerX + cameraPos.x * zoomLevel, centerY + cameraPos.y * zoomLevel);
        view.setSize(static_cast<float>(desktopMode.width) / zoomLevel, static_cast<float>(desktopMode.height) / zoomLevel);
        
        window.setView(view);
        window.clear(sf::Color(0, 0, 0, 0)); 
        
        // Рисуем сетку
        // В SFML 2.x mapPixelToCoords принимает просто sf::Vector2i
        sf::Vector2f topLeft = window.mapPixelToCoords(sf::Vector2i(0, 0), view);
        sf::Vector2f bottomRight = window.mapPixelToCoords(sf::Vector2i(desktopMode.width, desktopMode.height), view);
        
        sf::VertexArray gridLines(sf::Lines);
        float gridSize = 100.0f * zoomLevel;
        
        for (float x = std::fmod(topLeft.x, gridSize); x < bottomRight.x; x += gridSize) {
            // В SFML 2.x конструктор Vertex принимает (position, color)
            gridLines.append(sf::Vertex(sf::Vector2f(x, topLeft.y), sf::Color(50, 50, 50)));
            gridLines.append(sf::Vertex(sf::Vector2f(x, bottomRight.y), sf::Color(50, 50, 50)));
        }
        for (float y = std::fmod(topLeft.y, gridSize); y < bottomRight.y; y += gridSize) {
            gridLines.append(sf::Vertex(sf::Vector2f(topLeft.x, y), sf::Color(50, 50, 50)));
            gridLines.append(sf::Vertex(sf::Vector2f(bottomRight.x, y), sf::Color(50, 50, 50)));
        }
        window.draw(gridLines);
        
        // Рисуем окна
        for (const auto& win : windows) {
            sf::RectangleShape shape(sf::Vector2f(static_cast<float>(win.width), static_cast<float>(win.height)));
            // В SFML 2.x setPosition принимает (x, y) или вектор
            shape.setPosition(static_cast<float>(win.x), static_cast<float>(win.y));
            shape.setFillColor(sf::Color(100, 150, 200, 50));
            shape.setOutlineColor(sf::Color(100, 150, 200, 150));
            shape.setOutlineThickness(2);
            window.draw(shape);
        }
        
        window.display();
    }
    return 0;
}