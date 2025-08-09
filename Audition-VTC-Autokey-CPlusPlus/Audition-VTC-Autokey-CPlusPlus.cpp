#include <windows.h>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <opencv2/opencv.hpp>
#include <ctime>
#include <algorithm>
#include <fstream>
#include <commctrl.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "resource.h"

#define IDC_CHECK_AUTOKEY     1001
#define IDC_CHECK_AUTOSPACE   1002
#define WM_UPDATE_THRESHOLD_EDIT (WM_USER + 1)
#pragma comment(lib, "comctl32.lib")

HINSTANCE hInst;
HWND hCheckAutokey, hCheckAutospace, hStaticStatus;
HWND hWndMain = nullptr;
HWND gameHwnd = nullptr;
RECT gameRect{};
COLORREF statusTextColor = RGB(0, 0, 0);
HBRUSH hBackgroundBrush = nullptr;
HHOOK hKeyboardHook = nullptr;

std::atomic<int> spaceThresholdX(113);
const int defaultSpaceThresholdX = 113;
HWND hEditThreshold = nullptr;
HWND hUpDownThreshold = nullptr;

std::atomic<bool> enableAutoKey(false);
std::atomic<bool> enableAutoSpace(false);
std::atomic<bool> keyDetectedInFrame(false);
std::atomic<bool> stopThreads(false);

cv::Mat spaceTemplate;
std::vector<std::pair<std::string, cv::Mat>> buttonTemplates;

struct DetectedKey {
    int x;
    char key;
};

HWND FindGameWindow() {
    HWND hwnd = nullptr;
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        wchar_t title[256];
        GetWindowTextW(hWnd, title, 256);

        if (!IsWindowVisible(hWnd) || !IsWindowEnabled(hWnd)) return TRUE;

        if (wcscmp(title, L"Audition") == 0) {
            *(HWND*)lParam = hWnd;
            return FALSE;
        }
        return TRUE;
        }, (LPARAM)&hwnd);

    return hwnd;
}

void UpdateThresholdEdit() {
    if (!hEditThreshold || !IsWindow(hEditThreshold)) return;

    int delta = spaceThresholdX.load() - defaultSpaceThresholdX;
    wchar_t buf[32];
    swprintf_s(buf, L"%d", delta);
    SetWindowText(hEditThreshold, buf);

    if (hUpDownThreshold && IsWindow(hUpDownThreshold)) {
        SendMessage(hUpDownThreshold, UDM_SETPOS, 0, MAKELPARAM(delta, 0));
    }
}

void FocusGameWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return;

    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD curThread = GetCurrentThreadId();

    AttachThreadInput(fgThread, curThread, TRUE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    AttachThreadInput(fgThread, curThread, FALSE);
}


cv::Mat CaptureWindow(HWND hwnd) {
    if (!hwnd) return cv::Mat();

    static HDC hdcScreen = nullptr;
    static HDC hdcMem = nullptr;
    static HBITMAP hBitmap = nullptr;
    static cv::Mat mat;
    static int lastWidth = 0, lastHeight = 0;

    GetClientRect(hwnd, &gameRect);
    int width = gameRect.right;
    int height = gameRect.bottom;

    HDC hdc = GetDC(hwnd);

    // Init once or if size changed
    if (!hdcScreen || width != lastWidth || height != lastHeight) {
        if (hdcMem) DeleteDC(hdcMem);
        if (hBitmap) DeleteObject(hBitmap);
        hdcScreen = hdc;
        hdcMem = CreateCompatibleDC(hdcScreen);
        hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
        SelectObject(hdcMem, hBitmap);

        BITMAPINFOHEADER bi = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
        mat = cv::Mat(height, width, CV_8UC4);
        lastWidth = width;
        lastHeight = height;
    }

    BitBlt(hdcMem, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
    GetDIBits(hdcMem, hBitmap, 0, height, mat.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    ReleaseDC(hwnd, hdc);

    cv::Mat matBGR;
    cv::cvtColor(mat, matBGR, cv::COLOR_BGRA2BGR);
    return matBGR;
}

void SendKey(WORD vk) {
    WORD scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;

    SendInput(1, &input, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}


LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        HWND foreground = GetForegroundWindow();

        if (foreground == gameHwnd || foreground == hWndMain) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                switch (p->vkCode) {
                case VK_F5:
                    enableAutoKey = !enableAutoKey.load();
                    if (hCheckAutokey && IsWindow(hCheckAutokey)) {
                        SendMessage(hCheckAutokey, BM_SETCHECK, enableAutoKey ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                    break;
                case VK_F6:
                    enableAutoSpace = !enableAutoSpace.load();
                    if (hCheckAutospace && IsWindow(hCheckAutospace)) {
                        SendMessage(hCheckAutospace, BM_SETCHECK, enableAutoSpace ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                    break;
                case VK_F7: {
                    int newVal = std::max(0, spaceThresholdX.load() - 1);
                    spaceThresholdX.store(newVal);

                    PostMessage(hWndMain, WM_UPDATE_THRESHOLD_EDIT, 0, 0);
                    break;
                }
                case VK_F8: {
                    int newVal = std::min(170, spaceThresholdX.load() + 1);
                    spaceThresholdX.store(newVal);

                    PostMessage(hWndMain, WM_UPDATE_THRESHOLD_EDIT, 0, 0);
                    break;
                }
                case VK_F9: {
                    spaceThresholdX.store(defaultSpaceThresholdX);

                    PostMessage(hWndMain, WM_UPDATE_THRESHOLD_EDIT, 0, 0);
                    break;
                }


                }
            }
        }
    }

    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}


void AutoKeyThread() {
    while (!stopThreads.load()) {
        if (!IsWindowVisible(gameHwnd) || IsIconic(gameHwnd)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto frame = CaptureWindow(gameHwnd);
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        cv::Rect roi(294, 514, 445, 40);
        if (roi.x + roi.width > frame.cols || roi.y + roi.height > frame.rows) continue;

        cv::Mat region = frame(roi);
        if (region.empty()) continue;

        std::vector<DetectedKey> foundKeys;
        keyDetectedInFrame = false;

        for (auto& [keyStr, tmpl] : buttonTemplates) {
            if (tmpl.empty()) continue;

            if (region.rows >= tmpl.rows && region.cols >= tmpl.cols) {
                cv::Mat result;
                cv::matchTemplate(region, tmpl, result, cv::TM_CCOEFF_NORMED);
                cv::threshold(result, result, 0.75, 1.0, cv::THRESH_TOZERO);

                std::vector<cv::Point> pts;
                cv::findNonZero(result, pts);

                for (const auto& pt : pts) {
                    keyDetectedInFrame = true;
                    foundKeys.push_back({ pt.x, keyStr[0] });
                }
            }
        }

        std::sort(foundKeys.begin(), foundKeys.end(), [](const DetectedKey& a, const DetectedKey& b) {
            return a.x < b.x;
            });

        int lastX = -999;
        std::vector<char> keysToSend;

        for (const auto& dk : foundKeys) {
            if (abs(dk.x - lastX) > 10) {
                keysToSend.push_back(dk.key);
                lastX = dk.x;
            }
        }

        if (!keysToSend.empty() && enableAutoKey && GetForegroundWindow() == gameHwnd) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (char key : keysToSend) {
                 WORD vk = 0;
                switch (key) {
                case '1': vk = VK_END;   break;
                case '2': vk = VK_DOWN;  break;
                case '3': vk = VK_NEXT;  break;
                case '4': vk = VK_LEFT;  break;
                case '6': vk = VK_RIGHT; break;
                case '7': vk = VK_HOME;  break;
                case '8': vk = VK_UP;    break;
                case '9': vk = VK_PRIOR; break;
                }

                if (vk) {
                    SendKey(vk);
                    // Optional: randomized delay
                    // int delayMs = 10 + rand() % 30;  // 10–39 ms
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AutoSpaceThread() {
    constexpr float MATCH_THRESHOLD = 0.6f;
    constexpr int ROI_X = 514, ROI_Y = 488, ROI_W = 170, ROI_H = 16;

    bool spaceTriggered = false;
    cv::Mat result;

    while (!stopThreads.load()) {
        if (!enableAutoSpace || !gameHwnd || !IsWindowVisible(gameHwnd) || GetForegroundWindow() != gameHwnd) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }

        cv::Mat frame = CaptureWindow(gameHwnd);
        if (frame.empty()) continue;

        if (frame.cols < ROI_X + ROI_W || frame.rows < ROI_Y + ROI_H) continue;

        cv::Mat roi = frame(cv::Rect(ROI_X, ROI_Y, ROI_W, ROI_H));

        cv::matchTemplate(roi, spaceTemplate, result, cv::TM_CCOEFF_NORMED);
        cv::threshold(result, result, MATCH_THRESHOLD, 1.0, cv::THRESH_TOZERO);

        std::vector<cv::Point> matches;
        cv::findNonZero(result, matches);

        int thresholdX = spaceThresholdX.load();
        bool found = false;
        for (const auto& pt : matches) {
            if (pt.x >= thresholdX) {
                found = true;
                break;
            }
        }

        if (found) {
            if (!spaceTriggered) {
                SendKey(VK_RCONTROL); //VK_LCONTROL - VK_RCONTROL - VK_SPACE
                spaceTriggered = true;
            }
        }
        else {
            spaceTriggered = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void StatusUpdateThread() {
    HWND lastGameHwnd = nullptr;

    while (!stopThreads.load()) {
        HWND currentGameHwnd = FindGameWindow();
        if (currentGameHwnd && IsWindow(currentGameHwnd)) {
            gameHwnd = currentGameHwnd;

            if (currentGameHwnd != lastGameHwnd) {
                lastGameHwnd = currentGameHwnd;
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(gameHwnd);
            }

            if (IsWindowVisible(gameHwnd) && !IsIconic(gameHwnd)) {
                SetWindowText(hStaticStatus, L"Status: Game visible");
                statusTextColor = RGB(0, 160, 0); // Green
            }
            else {
                SetWindowText(hStaticStatus, L"Status: Game hidden");
                statusTextColor = RGB(255, 140, 0); // Orange
            }
        }
        else {
            gameHwnd = nullptr;
            lastGameHwnd = nullptr;
            SetWindowText(hStaticStatus, L"Status: Game not found");
            statusTextColor = RGB(128, 128, 128); // Gray
        }

        InvalidateRect(hStaticStatus, NULL, TRUE);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// === GUI ===
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hCheckAutokey = CreateWindow(L"BUTTON", L"Auto Key (F5)",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            20, 20, 200, 20, hWnd, (HMENU)IDC_CHECK_AUTOKEY, hInst, NULL);

        hCheckAutospace = CreateWindow(L"BUTTON", L"Auto Space (F6)",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            20, 50, 200, 20, hWnd, (HMENU)IDC_CHECK_AUTOSPACE, hInst, NULL);

        // Offset label (F7/F8)
        CreateWindow(L"STATIC", L"Offset (F7/F8):",
            WS_VISIBLE | WS_CHILD,
            20, 85, 120, 20, hWnd, NULL, hInst, NULL);

        // Edit box
        hEditThreshold = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"0.00",
            WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_NUMBER,
            20, 105, 60, 22, hWnd, (HMENU)3003, hInst, NULL);

        // UpDown control
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_UPDOWN_CLASS;
        InitCommonControlsEx(&icex);

        hUpDownThreshold = CreateWindow(UPDOWN_CLASS, NULL,
            WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS,
            0, 0, 0, 0, hWnd, (HMENU)3004, hInst, NULL);

        SendMessage(hUpDownThreshold, UDM_SETBUDDY, (WPARAM)hEditThreshold, 0);
        SendMessage(hUpDownThreshold, UDM_SETRANGE, 0, MAKELPARAM(100, -100)); // ±1.00 (100 steps of 0.01)
        SendMessage(hUpDownThreshold, UDM_SETPOS, 0, MAKELPARAM(0, 0));

        // Status label
        hStaticStatus = CreateWindow(L"STATIC", L"Status: Waiting...",
            WS_VISIBLE | WS_CHILD | SS_CENTER,
            0, 150, 260, 20, hWnd, NULL, hInst, NULL);
        // Reset button
        CreateWindow(L"BUTTON", L"Reset (F9)",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            82, 105, 70, 20, hWnd, (HMENU)4001, hInst, NULL);

        UpdateThresholdEdit();

        hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
        if (!hKeyboardHook) {
            MessageBox(NULL, L"Failed to set keyboard hook", L"Error", MB_ICONERROR);
            return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CHECK_AUTOKEY:
            enableAutoKey.store(IsDlgButtonChecked(hWnd, IDC_CHECK_AUTOKEY) == BST_CHECKED);
            break;
        case IDC_CHECK_AUTOSPACE:
            enableAutoSpace.store(IsDlgButtonChecked(hWnd, IDC_CHECK_AUTOSPACE) == BST_CHECKED);
            break;
        case 3003: // EDIT threshold
            if (HIWORD(wParam) == EN_CHANGE) {
                wchar_t buf[32];
                GetWindowText(hEditThreshold, buf, 32);
                float delta = static_cast<float>(_wtof(buf));
                float newVal = defaultSpaceThresholdX + delta;
                newVal = std::clamp(newVal, 0.0f, 170.0f);
                spaceThresholdX.store(static_cast<int>(newVal));
            }
            break;
        case 4001: // Reset button
            SendMessage(hUpDownThreshold, UDM_SETPOS32, 0, 0); // 0 = delta 0.00
            SetWindowText(hEditThreshold, L"0.00");
            spaceThresholdX.store(defaultSpaceThresholdX);
            break;
        }
        break;

    case WM_CTLCOLORSTATIC:
    {
        HDC hdcStatic = (HDC)wParam;
        HWND hwndStatic = (HWND)lParam;

        if (hwndStatic == hStaticStatus) {
            SetTextColor(hdcStatic, statusTextColor);
            SetBkMode(hdcStatic, TRANSPARENT);
            if (!hBackgroundBrush) {
                hBackgroundBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
            }
            return (INT_PTR)hBackgroundBrush;
        }
        break;
    }
    
    case WM_USER + 1:
        UpdateThresholdEdit();
        break;

    case WM_DESTROY:
        stopThreads.store(true);

        if (hBackgroundBrush) {
            DeleteObject(hBackgroundBrush);
            hBackgroundBrush = nullptr;
        }
        if (hKeyboardHook) UnhookWindowsHookEx(hKeyboardHook);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

cv::Mat LoadImageFromResource(HINSTANCE hInst, int resourceID) {
    HRSRC hRes = FindResource(hInst, MAKEINTRESOURCE(resourceID), RT_RCDATA);
    if (!hRes) return cv::Mat();

    DWORD size = SizeofResource(hInst, hRes);
    HGLOBAL hGlobal = LoadResource(hInst, hRes);
    if (!hGlobal) return cv::Mat();

    void* pData = LockResource(hGlobal);
    if (!pData) return cv::Mat();

    std::vector<uchar> buffer((uchar*)pData, (uchar*)pData + size);
    return cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    timeBeginPeriod(1);
    // === Load space template ===
    spaceTemplate = LoadImageFromResource(hInstance, IDB_SPACEBTN);
    if (spaceTemplate.empty()) {
        MessageBox(NULL, L"Failed to load spacen.png", L"Error", MB_ICONERROR);
        return 0;
    }

    //CreateDirectoryA("debug", NULL);

    // Force to 3 channels (BGR)
    if (spaceTemplate.channels() != 3) {
        if (spaceTemplate.channels() == 4) {
            cv::cvtColor(spaceTemplate, spaceTemplate, cv::COLOR_BGRA2BGR);
        }
        else if (spaceTemplate.channels() == 1) {
            cv::cvtColor(spaceTemplate, spaceTemplate, cv::COLOR_GRAY2BGR);
        }
        else {
            MessageBox(NULL, L"Unsupported channel format in spacen.png", L"Error", MB_ICONERROR);
            return 0;
        }
    }

    // === Load button templates ===
    std::map<std::string, int> keyToResourceID = {
        { "1",  IDB_BTN_1 },
        { "1d", IDB_BTN_1D },
        { "2",  IDB_BTN_2 },
        { "2d", IDB_BTN_2D },
        { "3",  IDB_BTN_3 },
        { "3d", IDB_BTN_3D },
        { "4",  IDB_BTN_4 },
        { "4d", IDB_BTN_4D },
        { "6",  IDB_BTN_6 },
        { "6d", IDB_BTN_6D },
        { "7",  IDB_BTN_7 },
        { "7d", IDB_BTN_7D },
        { "8",  IDB_BTN_8 },
        { "8d", IDB_BTN_8D },
        { "9",  IDB_BTN_9 },
        { "9d", IDB_BTN_9D }
    };

    for (const auto& [keyStr, resID] : keyToResourceID) {
        cv::Mat img = LoadImageFromResource(hInstance, resID);
        if (img.empty()) {
            std::string msg = "Failed to load resource image for key: " + keyStr;
            MessageBoxA(NULL, msg.c_str(), "Resource Error", MB_ICONERROR);
            return 0;
        }

        if (img.channels() != 3) {
            if (img.channels() == 4) {
                cv::cvtColor(img, img, cv::COLOR_BGRA2BGR);
            }
            else if (img.channels() == 1) {
                cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
            }
            else {
                std::string msg = "Unsupported channel format in resource for key: " + keyStr;
                MessageBoxA(NULL, msg.c_str(), "Image Format Error", MB_ICONERROR);
                return 0;
            }
        }

        if (img.cols == 0 || img.rows == 0) {
            std::string msg = "Template too small in resource for key: " + keyStr;
            MessageBoxA(NULL, msg.c_str(), "Image Size Error", MB_ICONERROR);
            return 0;
        }

        buttonTemplates.push_back({ std::string(1, keyStr[0]), img });
    }

    // Continue with normal window initialization...
    WNDCLASSEX wc{ sizeof(WNDCLASSEX) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AutoWinClass";

    // 🟢 Gán icon lớn và nhỏ
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

    RegisterClassEx(&wc);

    hWndMain = CreateWindow(L"AutoWinClass", L"Audition Auto Tool",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 260, 230, NULL, NULL, hInstance, NULL);

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    std::thread(StatusUpdateThread).detach();
    std::thread(AutoKeyThread).detach();
    std::thread(AutoSpaceThread).detach();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    timeEndPeriod(1);
    return (int)msg.wParam;
}
