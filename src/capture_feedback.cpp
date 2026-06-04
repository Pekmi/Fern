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

}
