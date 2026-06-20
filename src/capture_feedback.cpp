#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/fern/capture_feedback.h"

#include <windows.h>

#include <chrono>
#include <thread>

namespace fern {
    namespace {

        constexpr wchar_t kFeedbackClassName[] = L"FernCaptureFeedbackWindow";
        constexpr UINT_PTR kHideTimerId = 1;
        constexpr int kFeedbackWidth = 220;
        constexpr int kFeedbackHeight = 72;

        LRESULT CALLBACK FeedbackWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
            switch (message) {
            case WM_CREATE:
                SetTimer(hwnd, kHideTimerId, 1100, NULL);
                return 0;
            case WM_TIMER:
                if (wParam == kHideTimerId) {
                    KillTimer(hwnd, kHideTimerId);
                    DestroyWindow(hwnd);
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC dc = BeginPaint(hwnd, &ps);

                RECT client{};
                GetClientRect(hwnd, &client);

                HBRUSH background = CreateSolidBrush(RGB(18, 24, 32));
                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(82, 220, 150));
                HGDIOBJ oldBrush = SelectObject(dc, background);
                HGDIOBJ oldPen = SelectObject(dc, borderPen);
                RoundRect(dc, client.left, client.top, client.right, client.bottom, 14, 14);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(background);
                DeleteObject(borderPen);

                RECT accent{ 14, 18, 22, client.bottom - 18 };
                HBRUSH accentBrush = CreateSolidBrush(RGB(82, 220, 150));
                FillRect(dc, &accent, accentBrush);
                DeleteObject(accentBrush);

                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(245, 248, 250));

                HFONT titleFont = CreateFontW(
                    20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HGDIOBJ oldFont = SelectObject(dc, titleFont);
                RECT titleRect{ 36, 12, client.right - 16, 38 };
                DrawTextW(dc, L"Capture Fern", -1, &titleRect, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

                SetTextColor(dc, RGB(188, 198, 208));
                HFONT bodyFont = CreateFontW(
                    14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                SelectObject(dc, bodyFont);
                RECT bodyRect{ 36, 38, client.right - 16, 62 };
                DrawTextW(dc, L"Sauvegarde en cours", -1, &bodyRect, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

                SelectObject(dc, oldFont);
                DeleteObject(titleFont);
                DeleteObject(bodyFont);
                EndPaint(hwnd, &ps);
                return 0;
            }
            default:
                return DefWindowProc(hwnd, message, wParam, lParam);
            }
        }

        void RegisterFeedbackClass() {
            static bool registered = false;
            if (registered) return;

            WNDCLASSW wc{};
            wc.lpfnWndProc = FeedbackWndProc;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.lpszClassName = kFeedbackClassName;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }

        void FeedbackThread() {
            RegisterFeedbackClass();

            const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            const int x = screenWidth - kFeedbackWidth - 24;
            const int y = (screenHeight - kFeedbackHeight) / 2;

            HWND hwnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                kFeedbackClassName,
                L"Fern Capture",
                WS_POPUP,
                x,
                y,
                kFeedbackWidth,
                kFeedbackHeight,
                NULL,
                NULL,
                GetModuleHandleW(NULL),
                NULL);

            if (!hwnd) return;

            SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hwnd);

            MSG msg{};
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

    }

    void ShowCaptureFeedback() {
        std::thread(FeedbackThread).detach();
    }

    constexpr wchar_t kFrameClassName[] = L"FernCaptureFrameWindow";
    constexpr UINT_PTR kFrameTimerId = 2;

    LRESULT CALLBACK FrameWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            SetTimer(hwnd, kFrameTimerId, 16, NULL);
            SetPropW(hwnd, L"Alpha", (HANDLE)0);
            SetPropW(hwnd, L"State", (HANDLE)0);
            SetPropW(hwnd, L"Frames", (HANDLE)0);
            return 0;
        case WM_TIMER:
            if (wParam == kFrameTimerId) {
                int state = (int)(intptr_t)GetPropW(hwnd, L"State");
                int alpha = (int)(intptr_t)GetPropW(hwnd, L"Alpha");
                int frames = (int)(intptr_t)GetPropW(hwnd, L"Frames");
                
                if (state == 0) {
                    alpha += 15;
                    if (alpha >= 255) {
                        alpha = 255;
                        state = 1;
                    }
                } else if (state == 1) {
                    frames++;
                    if (frames > 120) {
                        state = 2;
                    }
                } else if (state == 2) {
                    alpha -= 10;
                    if (alpha <= 0) {
                        alpha = 0;
                        KillTimer(hwnd, kFrameTimerId);
                        DestroyWindow(hwnd);
                    }
                }
                
                SetPropW(hwnd, L"Alpha", (HANDLE)(intptr_t)alpha);
                SetPropW(hwnd, L"State", (HANDLE)(intptr_t)state);
                SetPropW(hwnd, L"Frames", (HANDLE)(intptr_t)frames);
                
                SetLayeredWindowAttributes(hwnd, RGB(0,0,0), (BYTE)alpha, LWA_COLORKEY | LWA_ALPHA);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            
            HBRUSH blackBrush = CreateSolidBrush(RGB(0,0,0));
            FillRect(dc, &client, blackBrush);
            DeleteObject(blackBrush);
            
            HBRUSH borderBrush = CreateSolidBrush(RGB(82, 220, 150));
            RECT top = { 0, 0, client.right, 8 };
            RECT bottom = { 0, client.bottom - 8, client.right, client.bottom };
            RECT left = { 0, 0, 8, client.bottom };
            RECT right = { client.right - 8, 0, client.right, client.bottom };
            FillRect(dc, &top, borderBrush);
            FillRect(dc, &bottom, borderBrush);
            FillRect(dc, &left, borderBrush);
            FillRect(dc, &right, borderBrush);
            DeleteObject(borderBrush);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            RemovePropW(hwnd, L"Alpha");
            RemovePropW(hwnd, L"State");
            RemovePropW(hwnd, L"Frames");
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
        }
    }

    void RegisterFrameClass() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSW wc{};
        wc.lpfnWndProc = FrameWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = kFrameClassName;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    void FrameThread(RECT desktopRect) {
        RegisterFrameClass();
        
        int x = desktopRect.left;
        int y = desktopRect.top;
        int width = desktopRect.right - desktopRect.left;
        int height = desktopRect.bottom - desktopRect.top;
        
        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            kFrameClassName,
            L"Fern Capture Frame",
            WS_POPUP,
            x, y, width, height,
            NULL, NULL, GetModuleHandleW(NULL), NULL);
            
        if (!hwnd) return;
        
        SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 0, LWA_COLORKEY | LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        
        MSG msg{};
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void ShowRecordingStartFrame(RECT desktopRect) {
        std::thread([desktopRect]() { FrameThread(desktopRect); }).detach();
    }

}
