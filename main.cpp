// PolaroidPins — tiny desktop widget showing two pinned polaroid photos.
//
// Controls:
//   drag            move the widget (unless position is locked)
//   click a photo   choose a new image for that polaroid
//   Ctrl + wheel    resize (aspect ratio is fixed by design)
//   right-click     menu (also on the tray icon)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------- constants

static const wchar_t* kClassName  = L"PolaroidPinsWnd";
static const wchar_t* kAppName    = L"PolaroidPins";
static const wchar_t* kRunKey     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Logical (unscaled) canvas everything is laid out on.
static const int   kCanvasW = 1000;
static const int   kCanvasH = 760;
static const float kMinScale = 0.25f;
static const float kMaxScale = 2.5f;

enum { WM_TRAY = WM_APP + 1 };
enum {
    IDM_PHOTO_LEFT = 1, IDM_PHOTO_RIGHT, IDM_LOCK, IDM_STARTUP,
    IDM_HIDESHOW, IDM_EXIT
};

struct Polaroid {
    float cx, cy;          // center on the logical canvas
    float w, h;            // frame size
    float angle;           // rotation in degrees
    float pinX;            // pin anchor, 0..1 across the frame width
    std::wstring path;     // current photo file ("" = placeholder)
    Bitmap*      photo = nullptr;
};

// ---------------------------------------------------------------- globals

static HWND      g_hwnd;
static HINSTANCE g_hinst;
static Image*    g_pin = nullptr;
static Polaroid  g_pol[2] = {
    { 330.f, 330.f, 430.f, 510.f, -6.f, 0.50f },
    { 660.f, 440.f, 410.f, 480.f,  5.f, 0.50f },
};
static float g_scale   = 0.55f;
static bool  g_locked  = false;
static bool  g_visible = true;
static NOTIFYICONDATAW g_nid = {};


static std::wstring ConfigPath() {
    wchar_t dir[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir);
    std::wstring d = std::wstring(dir) + L"\\PolaroidPins";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\config.ini";
}

static void SaveConfig() {
    std::wstring ini = ConfigPath();
    RECT r; GetWindowRect(g_hwnd, &r);
    wchar_t buf[64];
    swprintf_s(buf, L"%d", r.left);  WritePrivateProfileStringW(L"win", L"x", buf, ini.c_str());
    swprintf_s(buf, L"%d", r.top);   WritePrivateProfileStringW(L"win", L"y", buf, ini.c_str());
    swprintf_s(buf, L"%.4f", g_scale);
    WritePrivateProfileStringW(L"win", L"scale", buf, ini.c_str());
    WritePrivateProfileStringW(L"win", L"locked", g_locked ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"photos", L"left",  g_pol[0].path.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"photos", L"right", g_pol[1].path.c_str(), ini.c_str());
}

static void LoadPhoto(Polaroid& p, const std::wstring& path) {
    delete p.photo; p.photo = nullptr; p.path.clear();
    if (path.empty()) return;
    Bitmap* bmp = Bitmap::FromFile(path.c_str());
    if (bmp && bmp->GetLastStatus() == Ok) { p.photo = bmp; p.path = path; }
    else delete bmp;
}

static void LoadConfig(int* x, int* y) {
    std::wstring ini = ConfigPath();
    *x = GetPrivateProfileIntW(L"win", L"x", CW_USEDEFAULT, ini.c_str());
    *y = GetPrivateProfileIntW(L"win", L"y", CW_USEDEFAULT, ini.c_str());
    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"win", L"scale", L"", buf, 64, ini.c_str());
    float s = (float)_wtof(buf);
    if (s >= kMinScale && s <= kMaxScale) g_scale = s;
    g_locked = GetPrivateProfileIntW(L"win", L"locked", 0, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"photos", L"left", L"", buf, MAX_PATH, ini.c_str());
    LoadPhoto(g_pol[0], buf);
    GetPrivateProfileStringW(L"photos", L"right", L"", buf, MAX_PATH, ini.c_str());
    LoadPhoto(g_pol[1], buf);
}

// ---------------------------------------------------------------- startup with Windows

static bool StartupEnabled() {
    HKEY k; bool on = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void ToggleStartup() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (StartupEnabled()) {
        RegDeleteValueW(k, kAppName);
    } else {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- drawing

static void DrawPolaroid(Graphics& g, Polaroid& p) {
    GraphicsState st = g.Save();
    g.TranslateTransform(p.cx, p.cy);
    g.RotateTransform(p.angle);
    g.TranslateTransform(-p.w / 2, -p.h / 2);

    // soft drop shadow: GDI+ has no blur, so stack translucent expanding
    // rectangles — alpha accumulates toward the paper and fades outward,
    // which reads as depth where one polaroid overlaps the other
    SolidBrush shadow(Color(11, 0, 0, 0));
    for (int i = 0; i < 12; ++i) {
        float f = 12.f - i;
        g.FillRectangle(&shadow, 8.f - f, 11.f - f, p.w + 2 * f, p.h + 2 * f);
    }

    // white frame
    SolidBrush paper(Color(255, 250, 250, 248));
    g.FillRectangle(&paper, 0.f, 0.f, p.w, p.h);

    // photo area: classic polaroid margins (wide strip at the bottom)
    float m = p.w * 0.055f;
    RectF ph(m, m, p.w - 2 * m, p.h - m - p.w * 0.22f);

    if (p.photo) {
        // cover-crop the photo into the frame
        float iw = (float)p.photo->GetWidth(), ih = (float)p.photo->GetHeight();
        float s  = max(ph.Width / iw, ph.Height / ih);
        float sw = ph.Width / s, sh = ph.Height / s;
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(p.photo, ph, (iw - sw) / 2, (ih - sh) / 2, sw, sh, UnitPixel);
    } else {
        SolidBrush fill(Color(255, 225, 223, 218));
        g.FillRectangle(&fill, ph);
        Font f(L"Segoe UI", p.w * 0.045f);
        SolidBrush ink(Color(255, 130, 128, 124));
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"Click to add a photo", -1, &f, ph, &sf, &ink);
    }
    g.Restore(st);

    // pin: drawn unrotated in canvas space at the frame's top edge
    if (g_pin) {
        PointF top((p.pinX - 0.5f) * p.w, -p.h / 2 + p.w * 0.02f);
        // rotate that frame-local point into canvas space
        double a = p.angle * 3.14159265 / 180.0;
        float px = p.cx + (float)(top.X * cos(a) - top.Y * sin(a));
        float py = p.cy + (float)(top.X * sin(a) + top.Y * cos(a));
        float pw = p.w * 0.30f;
        float phh = pw * g_pin->GetHeight() / g_pin->GetWidth();
        // the PNG's baked-in shadow fills its lower-left; the pin body sits
        // around (0.70, 0.40) of the image — anchor on the body, not the image
        g.DrawImage(g_pin, px - pw * 0.70f, py - phh * 0.40f, pw, phh);
    }
}

static void Render() {
    int w = (int)(kCanvasW * g_scale), h = (int)(kCanvasH * g_scale);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        g.ScaleTransform(g_scale, g_scale);
        DrawPolaroid(g, g_pol[0]);
        DrawPolaroid(g, g_pol[1]);
    }

    RECT r; GetWindowRect(g_hwnd, &r);
    POINT dst = { r.left, r.top }, src = { 0, 0 };
    SIZE  sz  = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------- hit-testing

// Returns the polaroid index under the mouse (topmost first), -1 if none.
// If onPhoto is given, reports whether the point is inside the photo area.
static int HitTest(int mx, int my, bool* onPhoto = nullptr) {
    float x = mx / g_scale, y = my / g_scale;
    for (int i = 1; i >= 0; --i) {              // topmost (drawn last) first
        Polaroid& p = g_pol[i];
        // rotate the point into the polaroid's local frame
        double a = -p.angle * 3.14159265 / 180.0;
        float dx = x - p.cx, dy = y - p.cy;
        float lx = (float)(dx * cos(a) - dy * sin(a)) + p.w / 2;
        float ly = (float)(dx * sin(a) + dy * cos(a)) + p.h / 2;
        if (lx >= 0 && lx <= p.w && ly >= 0 && ly <= p.h) {
            if (onPhoto) {
                float mg = p.w * 0.055f;
                *onPhoto = lx >= mg && lx <= p.w - mg &&
                           ly >= mg && ly <= p.h - p.w * 0.22f;
            }
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- actions

static void PickPhoto(int idx) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Images (*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp)\0"
                      L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        LoadPhoto(g_pol[idx], file);
        Render();
        SaveConfig();
    }
}

static void ToggleVisible() {
    g_visible = !g_visible;
    ShowWindow(g_hwnd, g_visible ? SW_SHOWNA : SW_HIDE);
}

static void ShowMenu(POINT pt, int photoIdx) {
    HMENU m = CreatePopupMenu();
    if (photoIdx >= 0)
        AppendMenuW(m, MF_STRING, photoIdx == 0 ? IDM_PHOTO_LEFT : IDM_PHOTO_RIGHT,
                    L"Change this photo...");
    else {
        AppendMenuW(m, MF_STRING, IDM_PHOTO_LEFT,  L"Change left photo...");
        AppendMenuW(m, MF_STRING, IDM_PHOTO_RIGHT, L"Change right photo...");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_locked ? MF_CHECKED : 0), IDM_LOCK, L"Lock position");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), IDM_STARTUP,
                L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_HIDESHOW, g_visible ? L"Hide" : L"Show");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(g_hwnd);   // required so the menu closes properly from the tray
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- window proc

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT scr = pt; ClientToScreen(h, &scr);
        if (DragDetect(h, scr)) {
            if (!g_locked) {
                ReleaseCapture();
                SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        } else {
            bool onPhoto = false;
            int i = HitTest(pt.x, pt.y, &onPhoto);
            if (i >= 0 && onPhoto) PickPhoto(i);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int i = HitTest(pt.x, pt.y);
        POINT scr = pt; ClientToScreen(h, &scr);
        ShowMenu(scr, i);
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            float f = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.08f : 1.f / 1.08f;
            float ns = g_scale * f;
            if (ns < kMinScale) ns = kMinScale;
            if (ns > kMaxScale) ns = kMaxScale;
            if (ns != g_scale) { g_scale = ns; Render(); SaveConfig(); }
        }
        return 0;

    case WM_EXITSIZEMOVE:
        SaveConfig();
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ToggleVisible();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);
            ShowMenu(pt, -1);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_PHOTO_LEFT:  PickPhoto(0); break;
        case IDM_PHOTO_RIGHT: PickPhoto(1); break;
        case IDM_LOCK:        g_locked = !g_locked; SaveConfig(); break;
        case IDM_STARTUP:     ToggleStartup(); break;
        case IDM_HIDESHOW:    ToggleVisible(); break;
        case IDM_EXIT:        DestroyWindow(h); break;
        }
        return 0;

    case WM_DESTROY:
        SaveConfig();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- setup

static Image* LoadPinResource() {
    HRSRC rc = FindResourceW(g_hinst, L"PIN", RT_RCDATA);
    if (!rc) return nullptr;
    HGLOBAL hg = LoadResource(g_hinst, rc);
    void* data = LockResource(hg);
    DWORD size = SizeofResource(g_hinst, rc);
    IStream* stream = SHCreateMemStream((const BYTE*)data, size);
    if (!stream) return nullptr;
    Image* img = Image::FromStream(stream);
    stream->Release();
    if (img && img->GetLastStatus() != Ok) { delete img; img = nullptr; }
    return img;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    // single instance
    CreateMutexW(nullptr, TRUE, L"PolaroidPinsSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hinst = hinst;

    ULONG_PTR token;
    GdiplusStartupInput gsi;
    GdiplusStartup(&token, &gsi, nullptr);
    g_pin = LoadPinResource();

    int x, y;
    LoadConfig(&x, &y);
    int w = (int)(kCanvasW * g_scale), h = (int)(kCanvasH * g_scale);
    if (x == CW_USEDEFAULT) {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.right - w - 40; y = wa.top + 40;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, kAppName,
                             WS_POPUP, x, y, w, h, nullptr, nullptr, hinst, nullptr);

    // tray icon (made from the pin image)
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    if (g_pin) {
        Bitmap trayBmp(32, 32, PixelFormat32bppARGB);
        Graphics gg(&trayBmp);
        gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gg.DrawImage(g_pin, 0, 0, 32, 32);
        trayBmp.GetHICON(&g_nid.hIcon);
    }
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"PolaroidPins — click to show/hide");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    Render();
    ShowWindow(g_hwnd, SW_SHOWNA);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (auto& p : g_pol) delete p.photo;
    delete g_pin;
    GdiplusShutdown(token);
    return 0;
}
