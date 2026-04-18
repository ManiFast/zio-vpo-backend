#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

// cfg
const wchar_t CLASS_NAME[] = L"SAV_Main_Window";
const wchar_t WINDOW_TITLE[] = L"SAV";
const wchar_t MUTEX_NAME[] = L"Local\\SAV_Antivirus_SingleInstance";

const int SIDEBAR_WIDTH = 240;
const UINT WM_TRAYICON = WM_APP + 1;

// ids
enum
{
    IDC_BTN_OVERVIEW = 1001,
    IDC_BTN_SCAN = 1002,
    IDC_BTN_ABOUT = 1003,

    ID_TRAY_OPEN = 2001,
    ID_TRAY_ABOUT = 2002,
    ID_TRAY_EXIT = 2003,

    ID_FILE_EXIT = 3001
};

enum Page
{
    PAGE_OVERVIEW = 0,
    PAGE_SCAN,
    PAGE_ABOUT
};

// globals
HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;

HWND g_btnOverview = nullptr;
HWND g_btnScan = nullptr;
HWND g_btnAbout = nullptr;

HFONT g_hFont = nullptr;

HMENU g_hTrayMenu = nullptr;
HMENU g_hMainMenu = nullptr;

HICON g_hTrayIcon = nullptr;
HICON g_hSmallIcon = nullptr;
HICON g_hBigIcon = nullptr;

Image* g_pLogo = nullptr;

ULONG_PTR g_gdiplusToken = 0;
UINT g_TaskbarCreatedMsg = 0;

HANDLE g_hMutex = nullptr;

int g_currentPage = PAGE_OVERVIEW;
bool g_realExit = false;
bool g_startHidden = false;

std::wstring g_mainLogoPath;
std::wstring g_trayLogoPath;

// file utils
bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);

    std::wstring path = buf;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        path.erase(pos);

    return path;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring FindAsset(const wchar_t* fileName)
{
    std::wstring exeDir = GetExeDir();

    std::wstring tries[] =
    {
        JoinPath(exeDir, std::wstring(L"Addons\\") + fileName),
        JoinPath(exeDir, std::wstring(L"..\\Addons\\") + fileName),
        JoinPath(exeDir, std::wstring(L"..\\..\\Addons\\") + fileName),
        JoinPath(exeDir, std::wstring(L"..\\..\\..\\Addons\\") + fileName)
    };

    for (const auto& p : tries)
    {
        if (FileExists(p))
            return p;
    }

    return tries[0];
}

// start args
bool HasTrayArg(const wchar_t* lpCmdLine)
{
    if (!lpCmdLine)
        return false;

    std::wstring cmd = lpCmdLine;

    return cmd.find(L"--tray") != std::wstring::npos ||
        cmd.find(L"/tray") != std::wstring::npos ||
        cmd.find(L"-tray") != std::wstring::npos ||
        cmd.find(L"--hidden") != std::wstring::npos;
}

// single instance
bool CreateSingleInstanceGuard()
{
    g_hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);

    if (!g_hMutex)
        return true;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return false;

    return true;
}

void FreeSingleInstanceGuard()
{
    if (g_hMutex)
    {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
    }
}

// png load
Image* LoadPngImage(const std::wstring& path)
{
    Image* img = Image::FromFile(path.c_str(), FALSE);
    if (!img || img->GetLastStatus() != Ok)
    {
        delete img;
        return nullptr;
    }
    return img;
}

HICON LoadPngIcon(const std::wstring& path, int size)
{
    std::unique_ptr<Bitmap> src(Bitmap::FromFile(path.c_str(), FALSE));
    if (!src || src->GetLastStatus() != Ok)
        return nullptr;

    Bitmap canvas(size, size, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(Color(0, 0, 0, 0));
    g.DrawImage(src.get(), 0, 0, size, size);

    HICON hIcon = nullptr;
    if (canvas.GetHICON(&hIcon) != Ok)
        return nullptr;

    return hIcon;
}

// free res
void FreeResources()
{
    if (g_pLogo)
    {
        delete g_pLogo;
        g_pLogo = nullptr;
    }

    if (g_hTrayIcon)
    {
        DestroyIcon(g_hTrayIcon);
        g_hTrayIcon = nullptr;
    }

    if (g_hSmallIcon)
    {
        DestroyIcon(g_hSmallIcon);
        g_hSmallIcon = nullptr;
    }

    if (g_hBigIcon)
    {
        DestroyIcon(g_hBigIcon);
        g_hBigIcon = nullptr;
    }

    if (g_hFont)
    {
        DeleteObject(g_hFont);
        g_hFont = nullptr;
    }

    if (g_hTrayMenu)
    {
        DestroyMenu(g_hTrayMenu);
        g_hTrayMenu = nullptr;
    }
}

// top menu
HMENU CreateMainMenuBar()
{
    HMENU hMenuBar = CreateMenu();
    HMENU hFileMenu = CreatePopupMenu();

    AppendMenuW(hFileMenu, MF_STRING, ID_FILE_EXIT, L"Выход");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, L"Файл");

    return hMenuBar;
}

// left menu
void UpdateButtonFonts()
{
    if (g_btnOverview) SendMessageW(g_btnOverview, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    if (g_btnScan)     SendMessageW(g_btnScan, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    if (g_btnAbout)    SendMessageW(g_btnAbout, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

void LayoutControls(HWND hWnd)
{
    RECT rc{};
    GetClientRect(hWnd, &rc);

    int x = 20;
    int w = SIDEBAR_WIDTH - 40;
    int h = 42;
    int top = 150;

    MoveWindow(g_btnOverview, x, top, w, h, TRUE);
    MoveWindow(g_btnScan, x, top + 52, w, h, TRUE);
    MoveWindow(g_btnAbout, x, top + 104, w, h, TRUE);
}

void SetCurrentPage(int page)
{
    g_currentPage = page;

    InvalidateRect(g_btnOverview, nullptr, TRUE);
    InvalidateRect(g_btnScan, nullptr, TRUE);
    InvalidateRect(g_btnAbout, nullptr, TRUE);
    InvalidateRect(g_hWnd, nullptr, TRUE);
}

// tray
void CreateTrayMenu()
{
    if (g_hTrayMenu)
        DestroyMenu(g_hTrayMenu);

    g_hTrayMenu = CreatePopupMenu();
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_OPEN, L"Открыть");
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_ABOUT, L"О программе");
    AppendMenuW(g_hTrayMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");
}

bool AddTrayIcon(HWND hWnd)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hTrayIcon ? g_hTrayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    (void)lstrcpynW(nid.szTip, L"SAV", ARRAYSIZE(nid.szTip));

    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    if (ok)
    {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }

    return ok == TRUE;
}

void RemoveTrayIcon(HWND hWnd)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void HideToTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_HIDE);
}

void RestoreFromTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

void ShowTrayMenu(HWND hWnd)
{
    POINT pt{};
    GetCursorPos(&pt);

    SetForegroundWindow(hWnd);
    TrackPopupMenu(
        g_hTrayMenu,
        TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
        pt.x, pt.y, 0, hWnd, nullptr
    );
    PostMessageW(hWnd, WM_NULL, 0, 0);
}

// draw
void DrawLogo(Graphics& g, const RECT& rc)
{
    if (!g_pLogo)
        return;

    REAL imgW = (REAL)g_pLogo->GetWidth();
    REAL imgH = (REAL)g_pLogo->GetHeight();

    REAL boxW = (REAL)(rc.right - rc.left);
    REAL boxH = (REAL)(rc.bottom - rc.top);

    REAL scaleX = boxW / imgW;
    REAL scaleY = boxH / imgH;
    REAL ratio = (scaleX < scaleY) ? scaleX : scaleY;

    REAL drawW = imgW * ratio;
    REAL drawH = imgH * ratio;

    REAL x = (REAL)rc.left + (boxW - drawW) / 2.0f;
    REAL y = (REAL)rc.top + (boxH - drawH) / 2.0f;

    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(g_pLogo, x, y, drawW, drawH);
}

void DrawContent(HDC hdc, RECT rcClient)
{
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    SolidBrush whiteBrush(Color(255, 255, 255));
    SolidBrush sideBrush(Color(244, 247, 250));
    Pen linePen(Color(224, 230, 236), 1.0f);

    g.FillRectangle(&whiteBrush, 0, 0, rcClient.right, rcClient.bottom);
    g.FillRectangle(&sideBrush, 0, 0, SIDEBAR_WIDTH, rcClient.bottom);
    g.DrawLine(&linePen, (REAL)SIDEBAR_WIDTH, 0.0f, (REAL)SIDEBAR_WIDTH, (REAL)rcClient.bottom);

    RECT logoRect{ 20, 18, SIDEBAR_WIDTH - 20, 120 };
    DrawLogo(g, logoRect);

    FontFamily ff(L"Segoe UI");
    Font titleFont(&ff, 28, FontStyleBold, UnitPixel);
    Font textFont(&ff, 18, FontStyleRegular, UnitPixel);
    Font smallFont(&ff, 14, FontStyleRegular, UnitPixel);

    SolidBrush titleBrush(Color(35, 45, 55));
    SolidBrush textBrush(Color(90, 102, 115));
    SolidBrush greenBrush(Color(40, 125, 85));

    RectF titleRect((REAL)SIDEBAR_WIDTH + 35, 35.0f, 500.0f, 40.0f);
    RectF textRect((REAL)SIDEBAR_WIDTH + 35, 80.0f, 760.0f, 40.0f);
    RectF versionRect(20.0f, (REAL)rcClient.bottom - 35.0f, 140.0f, 20.0f);

    if (g_currentPage == PAGE_OVERVIEW)
    {
        g.DrawString(L"Обзор", -1, &titleFont, titleRect, nullptr, &titleBrush);
        g.DrawString(L"Главная страница SAV.", -1, &textFont, textRect, nullptr, &textBrush);

        RectF okRect((REAL)SIDEBAR_WIDTH + 35, 130.0f, 400.0f, 30.0f);
        g.DrawString(L"Статус: интерфейс работает.", -1, &textFont, okRect, nullptr, &greenBrush);
    }
    else if (g_currentPage == PAGE_SCAN)
    {
        g.DrawString(L"Сканирование", -1, &titleFont, titleRect, nullptr, &titleBrush);
        g.DrawString(L"Тут скоро что-то будет.", -1, &textFont, textRect, nullptr, &textBrush);
    }
    else if (g_currentPage == PAGE_ABOUT)
    {
        g.DrawString(L"О программе", -1, &titleFont, titleRect, nullptr, &titleBrush);
        g.DrawString(L"SAV — минималистичная заготовка антивируса.", -1, &textFont, textRect, nullptr, &textBrush);

        RectF infoRect((REAL)SIDEBAR_WIDTH + 35, 130.0f, 820.0f, 100.0f);
        g.DrawString(L"Крестик скрывает окно в трей.\nПосле перезапуска explorer.exe иконка в трее возвращается.", -1, &textFont, infoRect, nullptr, &textBrush);
    }

    g.DrawString(L"Версия 0.1", -1, &smallFont, versionRect, nullptr, &textBrush);
}

void DrawMenuButton(LPDRAWITEMSTRUCT dis, const wchar_t* text, bool selected)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    HBRUSH bg = CreateSolidBrush(selected ? RGB(217, 236, 252) : RGB(244, 247, 250));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (selected)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(180, 214, 245));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 14, 14);

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, selected ? RGB(24, 78, 126) : RGB(60, 72, 84));

    HFONT oldFont = (HFONT)SelectObject(hdc, g_hFont);

    RECT textRc = rc;
    textRc.left += 14;
    DrawTextW(hdc, text, -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    SelectObject(hdc, oldFont);
}

// about
void ShowAbout(HWND hWnd)
{
    MessageBoxW(
        hWnd,
        L"SAV\n\nМинималистичная заготовка антивируса.\nКрестик скрывает окно в трей.\nИконка в трее восстанавливается после перезапуска Explorer.",
        L"О программе",
        MB_OK | MB_ICONINFORMATION
    );
}

// wnd proc
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_TaskbarCreatedMsg)
    {
        AddTrayIcon(hWnd);
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        g_btnOverview = CreateWindowW(L"BUTTON", L"Обзор",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_OVERVIEW, g_hInst, nullptr);

        g_btnScan = CreateWindowW(L"BUTTON", L"Сканирование",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_SCAN, g_hInst, nullptr);

        g_btnAbout = CreateWindowW(L"BUTTON", L"О программе",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hWnd, (HMENU)IDC_BTN_ABOUT, g_hInst, nullptr);

        g_hFont = CreateFontW(
            -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
        );

        UpdateButtonFonts();
        LayoutControls(hWnd);
        CreateTrayMenu();
        AddTrayIcon(hWnd);

        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hSmallIcon);
        SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)g_hBigIcon);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_BTN_OVERVIEW:
            SetCurrentPage(PAGE_OVERVIEW);
            return 0;

        case IDC_BTN_SCAN:
            SetCurrentPage(PAGE_SCAN);
            return 0;

        case IDC_BTN_ABOUT:
            SetCurrentPage(PAGE_ABOUT);
            return 0;

        case ID_TRAY_OPEN:
            RestoreFromTray(hWnd);
            return 0;

        case ID_TRAY_ABOUT:
            RestoreFromTray(hWnd);
            SetCurrentPage(PAGE_ABOUT);
            ShowAbout(hWnd);
            return 0;

        case ID_TRAY_EXIT:
        case ID_FILE_EXIT:
            g_realExit = true;
            DestroyWindow(hWnd);
            return 0;
        }
        return 0;

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;

        if (dis->CtlID == IDC_BTN_OVERVIEW)
            DrawMenuButton(dis, L"Обзор", g_currentPage == PAGE_OVERVIEW);
        else if (dis->CtlID == IDC_BTN_SCAN)
            DrawMenuButton(dis, L"Сканирование", g_currentPage == PAGE_SCAN);
        else if (dis->CtlID == IDC_BTN_ABOUT)
            DrawMenuButton(dis, L"О программе", g_currentPage == PAGE_ABOUT);

        return TRUE;
    }

    case WM_CLOSE:
        if (!g_realExit)
        {
            HideToTray(hWnd);
            return 0;
        }
        break;

    case WM_TRAYICON:
    {
        UINT code = LOWORD(lParam);

        switch (code)
        {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            ShowTrayMenu(hWnd);
            return 0;

        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            RestoreFromTray(hWnd);
            return 0;
        }
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient{};
        GetClientRect(hWnd, &rcClient);
        DrawContent(hdc, rcClient);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        RemoveTrayIcon(hWnd);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// entry
int WINAPI wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    g_hInst = hInstance;
    g_startHidden = HasTrayArg(lpCmdLine);

    if (!CreateSingleInstanceGuard())
        return 0;

    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Ok)
    {
        FreeSingleInstanceGuard();
        return 0;
    }

    g_mainLogoPath = FindAsset(L"sav.png");
    g_trayLogoPath = FindAsset(L"sav-sq.png");

    g_pLogo = LoadPngImage(g_mainLogoPath);
    g_hTrayIcon = LoadPngIcon(g_trayLogoPath, 16);
    g_hSmallIcon = LoadPngIcon(g_trayLogoPath, 16);
    g_hBigIcon = LoadPngIcon(g_trayLogoPath, 32);

    if (!g_hTrayIcon)  g_hTrayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_hSmallIcon) g_hSmallIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_hBigIcon)   g_hBigIcon = LoadIconW(nullptr, IDI_APPLICATION);

    g_TaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_hMainMenu = CreateMainMenuBar();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_hBigIcon;
    wc.hIconSm = g_hSmallIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassExW(&wc))
    {
        FreeResources();
        GdiplusShutdown(g_gdiplusToken);
        FreeSingleInstanceGuard();
        return 0;
    }

    g_hWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 700,
        nullptr,
        g_hMainMenu,
        hInstance,
        nullptr
    );

    if (!g_hWnd)
    {
        FreeResources();
        GdiplusShutdown(g_gdiplusToken);
        FreeSingleInstanceGuard();
        return 0;
    }

    if (g_startHidden)
    {
        ShowWindow(g_hWnd, SW_HIDE);
    }
    else
    {
        ShowWindow(g_hWnd, nCmdShow);
        UpdateWindow(g_hWnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FreeResources();
    GdiplusShutdown(g_gdiplusToken);
    FreeSingleInstanceGuard();

    return (int)msg.wParam;
}
















/* 

Иконка в трее
AddTrayIcon(hWnd); WM_CREATE

Иконка в трее - создание при появлении новой панели задач

Иконка в трее - открытие главного окна при клике левой кнопкой мыши


Иконка в трее - показ контекстного меню при клике правой кнопкой мыши

Контекстное меню иконки в трее - показ главного окна приложения

Контекстное меню иконки в трее - выход из приложения

Главное окно - закрытие окна не приводит к выгрузке приложения

Главное меню главного окна - пункт Выход приводит к завершению приложения

Графическое приложение - не запускаетcz более одного экземпляра в пользовательской сессии

Графическое приложение - настроен конвейер сборки


*/