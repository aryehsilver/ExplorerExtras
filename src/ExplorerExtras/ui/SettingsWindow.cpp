#include "SettingsWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>  // gdiplus.h is written against IStream
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <string>

// The project builds with NOMINMAX, and gdiplus.h uses min and max unqualified.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus

#include <gdiplus.h>

#include "../core/DarkMode.h"
#include "../core/Logging.h"
#include "../core/ShellItems.h"  // AppsUseDarkTheme
#include "../resource.h"

namespace ee {

// One line of the window. The header forward-declares this so a Control can
// point back at the row it was built from.
struct Row {
    RowKind kind;
    const wchar_t* label;
    const wchar_t* note;     // the second line of a card, or nullptr
    bool Settings::* field;  // Toggle rows only
    UINT command;
    int depth;  // 1 means "inside the group above", and hidden until it opens
    const wchar_t* second_label;  // Buttons rows only
    UINT second_command;
    wchar_t glyph;  // Segoe Fluent Icons, or 0 for none
};

namespace {

constexpr wchar_t kClassName[] = L"ExplorerExtras.Settings";
constexpr wchar_t kTitle[] = L"Explorer Extras";

// Everything below is in DIPs, scaled once the window knows its monitor. The
// figures are Windows 11's: 14px body text, 12px secondary, a card a little
// over three body lines tall, four pixels between them.
constexpr int kMarginDip = 20;
constexpr int kTopMarginDip = 16;
constexpr int kCardHeightDip = 52;
constexpr int kCardWithNoteDip = 72;
constexpr int kCardGapDip = 4;
constexpr int kCardRadiusDip = 6;
constexpr int kCardPadDip = 16;
constexpr int kIconLeftDip = 18;
constexpr int kTextLeftDip = 52;
constexpr int kStateGapDip = 12;
constexpr int kChevronWidthDip = 40;
constexpr int kIconPx = 16;
constexpr int kChevronPx = 12;
constexpr int kSectionTopDip = 24;
constexpr int kSectionHeightDip = 22;
constexpr int kSectionGapDip = 6;
constexpr int kButtonHeightDip = 32;
constexpr int kButtonGapDip = 8;
constexpr int kButtonPadDip = 28;
constexpr int kTrackWidthDip = 40;
constexpr int kTrackHeightDip = 20;
constexpr int kKnobDip = 12;
constexpr int kBodyPx = 14;
constexpr int kNotePx = 12;
constexpr int kMinWidthDip = 400;
constexpr int kMinHeightDip = 220;
constexpr int kMaxWidthDip = 640;

// One command per group header, for the chevron that opens it.
constexpr UINT kExpandFirst = 40200;

int Scale(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

// The order here is the order on screen. A row at depth 1 belongs to the row
// above it and only appears when that group is open.
const Row kRows[] = {
    {RowKind::Toggle, L"Enabled", L"Turning this off leaves Explorer exactly as it was.",
     &Settings::enabled, IDM_ENABLED, 0, nullptr, 0, L'\xE7E8'},

    {RowKind::Section, L"Hovering", nullptr, nullptr, 0, 0, nullptr, 0, 0},
    {RowKind::Toggle, L"Subfolder tips on hover", L"What is inside a folder, without opening it.",
     &Settings::subfolderTips, IDM_SUBFOLDER_TIPS, 0, nullptr, 0, L'\xE8B7'},
    {RowKind::Toggle, L"Show item counts on folders", nullptr, &Settings::folderItemCounts,
     IDM_FOLDER_COUNTS, 1, nullptr, 0, 0},
    {RowKind::Toggle, L"File previews on hover", L"A look inside a file, without opening it.",
     &Settings::filePreviews, IDM_FILE_PREVIEWS, 0, nullptr, 0, L'\xE890'},
    {RowKind::Toggle, L"Preview audio and video", nullptr, &Settings::mediaPlayback,
     IDM_MEDIA_PLAYBACK, 1, nullptr, 0, 0},
    {RowKind::Toggle, L"Start playing on hover", nullptr, &Settings::mediaAutoPlay,
     IDM_MEDIA_AUTOPLAY, 1, nullptr, 0, 0},

    {RowKind::Section, L"Getting around", nullptr, nullptr, 0, 0, nullptr, 0, 0},
    {RowKind::Toggle, L"Double-click empty space to go up", nullptr,
     &Settings::navigateUpOnDoubleClick, IDM_NAVIGATE_UP, 0, nullptr, 0, L'\xE74A'},
    {RowKind::Toggle, L"Remember recent folders",
     L"The tray menu offers them. Ctrl opens one in the tab you were in.",
     &Settings::rememberRecentFolders, IDM_REMEMBER_RECENT, 0, nullptr, 0, L'\xE81C'},
    {RowKind::Buttons, L"Forget them all", nullptr, nullptr, IDM_CLEAR_RECENT, 0, nullptr, 0, 0},

    {RowKind::Section, L"This app", nullptr, nullptr, 0, 0, nullptr, 0, 0},
    {RowKind::Toggle, L"Start with Windows", nullptr, &Settings::runAtStartup, IDM_RUN_AT_STARTUP, 0,
     nullptr, 0, L'\xE713'},
    {RowKind::Buttons, L"Show log file", nullptr, nullptr, IDM_OPEN_LOG, 0, L"Copy log path",
     IDM_COPY_LOG_PATH, 0},
    {RowKind::Buttons, L"Reset file previews", nullptr, nullptr, IDM_RESET_PREVIEWS, 0,
     L"Log what is under the pointer", IDM_DIAGNOSTICS, 0},
};

constexpr size_t kRowCount = ARRAYSIZE(kRows);

bool HasChildren(size_t index) {
    // Only a top-level row opens a group; a row inside one does not nest
    // further, however many of them follow each other.
    return index + 1 < kRowCount && kRows[index].depth == 0 && kRows[index + 1].depth > 0;
}

bool IsLastChild(size_t index) {
    return kRows[index].depth > 0 && (index + 1 >= kRowCount || kRows[index + 1].depth == 0);
}

// Which rows are live, mirroring what the menu greyed out: a setting whose
// feature is off should look unavailable rather than quietly doing nothing.
bool RowEnabled(const Settings& s, UINT command) {
    switch (command) {
        case IDM_ENABLED:
        case IDM_RUN_AT_STARTUP:
        case IDM_OPEN_LOG:
        case IDM_COPY_LOG_PATH:
        case IDM_DIAGNOSTICS:
            return true;
        case IDM_FOLDER_COUNTS:
            return s.enabled && s.subfolderTips;
        case IDM_MEDIA_PLAYBACK:
            return s.enabled && s.filePreviews;
        case IDM_MEDIA_AUTOPLAY:
            return s.enabled && s.filePreviews && s.mediaPlayback;
        case IDM_CLEAR_RECENT:
            return s.rememberRecentFolders;
        default:
            return s.enabled;
    }
}

// Windows 11's own surface colours. A card sits on the window, a hovered card
// lifts slightly, the inside of an open group sits a shade below the card, and
// the hairline around it all is what stops a stack of rows reading as a block.
struct Palette {
    COLORREF window;
    COLORREF card;
    COLORREF card_hot;
    COLORREF card_pressed;
    COLORREF group;
    COLORREF group_hot;
    COLORREF border;
    COLORREF text;
    COLORREF secondary;
    COLORREF disabled;
    COLORREF accent;
    COLORREF knob_on;
    COLORREF track_off;
    COLORREF track_off_border;
    COLORREF knob_off;
};

// The accent as Windows itself would use it here: the lighter variant on a dark
// background, the darker one on light, so the toggle keeps its contrast either
// way. The palette is eight colours, lightest first, with the chosen accent in
// the middle.
COLORREF AccentColour(bool dark) {
    BYTE palette[32]{};
    DWORD size = sizeof(palette);
    DWORD type = 0;
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
        L"AccentPalette", RRF_RT_REG_BINARY, &type, palette, &size);
    if (status == ERROR_SUCCESS && size == sizeof(palette)) {
        const int index = dark ? 1 : 4;  // AccentLight2 on dark, AccentDark1 on light
        return RGB(palette[index * 4], palette[index * 4 + 1], palette[index * 4 + 2]);
    }

    // No palette to read: the DWM knows the accent even when Explorer's copy of
    // it is missing, and failing that, Windows' own default blue.
    DWORD colour = 0;
    size = sizeof(colour);
    if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, &type, &colour, &size) == ERROR_SUCCESS) {
        return RGB(GetRValue(colour), GetGValue(colour), GetBValue(colour));  // stored as ABGR
    }
    return dark ? RGB(76, 194, 255) : RGB(0, 95, 184);
}

Palette PaletteFor(bool dark) {
    Palette p{};
    if (dark) {
        p.window = RGB(32, 32, 32);
        p.card = RGB(43, 43, 43);
        p.card_hot = RGB(50, 50, 50);
        p.card_pressed = RGB(39, 39, 39);
        p.group = RGB(39, 39, 39);
        p.group_hot = RGB(46, 46, 46);
        p.border = RGB(56, 56, 56);
        p.text = RGB(255, 255, 255);
        p.secondary = RGB(200, 200, 200);
        p.disabled = RGB(118, 118, 118);
        p.knob_on = RGB(0, 0, 0);
        p.track_off = RGB(39, 39, 39);
        p.track_off_border = RGB(154, 154, 154);
        p.knob_off = RGB(209, 209, 209);
    } else {
        p.window = RGB(243, 243, 243);
        p.card = RGB(255, 255, 255);
        p.card_hot = RGB(249, 249, 249);
        p.card_pressed = RGB(242, 242, 242);
        p.group = RGB(249, 249, 249);
        p.group_hot = RGB(243, 243, 243);
        p.border = RGB(229, 229, 229);
        p.text = RGB(26, 26, 26);
        p.secondary = RGB(95, 95, 95);
        p.disabled = RGB(157, 157, 157);
        p.knob_on = RGB(255, 255, 255);
        p.track_off = RGB(240, 240, 240);
        p.track_off_border = RGB(134, 134, 134);
        p.knob_off = RGB(94, 94, 94);
    }
    p.accent = AccentColour(dark);
    return p;
}

Gdiplus::Color Argb(COLORREF colour, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(colour), GetGValue(colour), GetBValue(colour));
}

// A rectangle with only some of its corners rounded, so a stack of rows can be
// one shape with square joins in the middle.
Gdiplus::GraphicsPath* RoundedPath(const Gdiplus::RectF& r, float radius, int corners) {
    auto* path = new Gdiplus::GraphicsPath();
    const float d = radius * 2;
    const float l = r.X, t = r.Y, right = r.GetRight(), b = r.GetBottom();
    const bool tl = (corners & kTopLeft) != 0;
    const bool tr = (corners & kTopRight) != 0;
    const bool br = (corners & kBottomRight) != 0;
    const bool bl = (corners & kBottomLeft) != 0;

    path->StartFigure();
    if (tl) path->AddArc(l, t, d, d, 180.0f, 90.0f);
    path->AddLine(tl ? l + radius : l, t, tr ? right - radius : right, t);
    if (tr) path->AddArc(right - d, t, d, d, 270.0f, 90.0f);
    path->AddLine(right, tr ? t + radius : t, right, br ? b - radius : b);
    if (br) path->AddArc(right - d, b - d, d, d, 0.0f, 90.0f);
    path->AddLine(br ? right - radius : right, b, bl ? l + radius : l, b);
    if (bl) path->AddArc(l, b - d, d, d, 90.0f, 90.0f);
    path->AddLine(l, bl ? b - radius : b, l, tl ? t + radius : t);
    path->CloseFigure();
    return path;
}

// A font at a pixel size, in the face Windows 11 sets its own text in. The
// mapper substitutes silently when a face is missing, so the result is checked
// and the system's message font used instead rather than whatever it picked.
HFONT BuildFont(UINT dpi, int pixels, bool semibold) {
    LOGFONTW request{};
    request.lfHeight = -Scale(pixels, dpi);
    request.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    request.lfQuality = CLEARTYPE_QUALITY;
    request.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(request.lfFaceName, L"Segoe UI Variable Text");

    if (HFONT font = CreateFontIndirectW(&request)) {
        const HDC screen = GetDC(nullptr);
        const HGDIOBJ previous = SelectObject(screen, font);
        wchar_t face[LF_FACESIZE]{};
        GetTextFaceW(screen, LF_FACESIZE, face);
        SelectObject(screen, previous);
        ReleaseDC(nullptr, screen);
        if (CompareStringOrdinal(face, -1, request.lfFaceName, -1, TRUE) == CSTR_EQUAL) return font;
        DeleteObject(font);
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        LOGFONTW fallback = metrics.lfMessageFont;
        fallback.lfHeight = -Scale(pixels, dpi);
        fallback.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
        if (HFONT font = CreateFontIndirectW(&fallback)) return font;
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// The icon font Windows 11 uses for its own settings rows. Older builds have
// its predecessor; a machine with neither simply gets rows without icons, which
// is why this may return null.
HFONT BuildIconFont(UINT dpi, int pixels) {
    for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        LOGFONTW request{};
        request.lfHeight = -Scale(pixels, dpi);
        request.lfWeight = FW_NORMAL;
        // Greyscale rather than ClearType: subpixel antialiasing puts colour
        // fringes on the curves of an icon, which is why Windows does the same.
        request.lfQuality = ANTIALIASED_QUALITY;
        request.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(request.lfFaceName, face);
        const HFONT font = CreateFontIndirectW(&request);
        if (!font) continue;

        const HDC screen = GetDC(nullptr);
        const HGDIOBJ previous = SelectObject(screen, font);
        wchar_t resolved[LF_FACESIZE]{};
        GetTextFaceW(screen, LF_FACESIZE, resolved);
        SelectObject(screen, previous);
        ReleaseDC(nullptr, screen);
        if (CompareStringOrdinal(resolved, -1, face, -1, TRUE) == CSTR_EQUAL) return font;
        DeleteObject(font);
    }
    return nullptr;
}

int TextWidth(HDC dc, HFONT font, const wchar_t* text) {
    const HGDIOBJ previous = SelectObject(dc, font);
    SIZE extent{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &extent);
    SelectObject(dc, previous);
    return extent.cx;
}

bool CursorOver(HWND window) {
    if (!window || !IsWindowVisible(window)) return false;
    POINT cursor{};
    GetCursorPos(&cursor);
    RECT bounds{};
    GetWindowRect(window, &bounds);
    return PtInRect(&bounds, cursor) != FALSE;
}

ULONG_PTR g_gdiplus_token = 0;
int g_gdiplus_users = 0;

void AcquireGdiplus() {
    if (g_gdiplus_users++ > 0) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr) != Gdiplus::Ok) {
        g_gdiplus_token = 0;
    }
}

void ReleaseGdiplus() {
    if (--g_gdiplus_users > 0 || g_gdiplus_token == 0) return;
    Gdiplus::GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
}

}  // namespace

SettingsWindow::~SettingsWindow() {
    if (window_) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_) DeleteObject(font_);
    if (note_font_) DeleteObject(note_font_);
    if (section_font_) DeleteObject(section_font_);
    if (icon_font_) DeleteObject(icon_font_);
    if (chevron_font_) DeleteObject(chevron_font_);
    if (background_) DeleteObject(background_);
    if (created_) ReleaseGdiplus();
}

bool SettingsWindow::Show(HINSTANCE instance, HWND commands, const Settings* settings) {
    instance_ = instance;
    commands_ = commands;
    settings_ = settings;

    if (!window_ && !Create(instance)) return false;

    // Reopening starts at the top. The size it was given is kept - that was a
    // decision - but where it happened to be scrolled to was not.
    if (!IsWindowVisible(window_) && scroll_ != 0) {
        scroll_ = 0;
        Layout();
    }

    Refresh();
    ShowWindow(window_, SW_SHOW);
    // A tray app has no business stealing focus at random, but this window only
    // ever opens because someone asked for it.
    SetForegroundWindow(window_);
    return true;
}

bool SettingsWindow::Create(HINSTANCE instance) {
    static bool registered = false;
    if (!registered) {
        // Custom-drawn buttons report their state through custom draw, which is
        // a common controls feature: make sure the v6 classes are up.
        INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SettingsWindow::WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
        wc.hIconSm = wc.hIcon;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return false;
    }

    AcquireGdiplus();
    created_ = true;
    dark_ = AppsUseDarkTheme();

    // Resizable, with a scroll bar for when the rows do not fit - a laptop
    // screen is shorter than this window would like to be. No maximise box:
    // a column of rows gains nothing from filling a monitor.
    window_ = CreateWindowExW(
        0, kClassName, kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance, this);
    if (!window_) {
        EE_ERR(L"settings window failed to create, gle=%lu", GetLastError());
        return false;
    }

    dpi_ = GetDpiForWindow(window_);
    BuildControls();
    Layout();          // measures the content
    SizeToContent();   // ...so the window can open around it
    Layout();          // ...and the rows settle into the size they got
    ApplyTheme();

    // Open over the pointer's monitor - which is the one the tray icon is on.
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT frame{};
    GetWindowRect(window_, &frame);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - width) / 2;
    const int y = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;
    SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

int SettingsWindow::TextLeftFor(const Row& row) const {
    const bool has_icon = row.glyph != 0 && icon_font_ != nullptr;
    return Scale(has_icon ? kTextLeftDip : kCardPadDip, dpi_);
}

void SettingsWindow::BuildControls() {
    for (const Control& control : controls_) {
        if (control.window) DestroyWindow(control.window);
    }
    controls_.clear();
    expanded_.assign(kRowCount, false);

    if (font_) DeleteObject(font_);
    if (note_font_) DeleteObject(note_font_);
    if (section_font_) DeleteObject(section_font_);
    if (icon_font_) DeleteObject(icon_font_);
    if (chevron_font_) DeleteObject(chevron_font_);
    font_ = BuildFont(dpi_, kBodyPx, false);
    note_font_ = BuildFont(dpi_, kNotePx, false);
    section_font_ = BuildFont(dpi_, kBodyPx, true);
    icon_font_ = BuildIconFont(dpi_, kIconPx);
    chevron_font_ = BuildIconFont(dpi_, kChevronPx);

    // Siblings that overlap - a header and the chevron sitting on it - need to
    // stay out of each other's paint.
    // BS_NOTIFY so focus moving in and out is announced: a row and its chevron
    // share one focus rectangle, and the half that did not change still has to
    // redraw. WS_CLIPSIBLINGS because those two overlap.
    const DWORD common = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_NOTIFY;

    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];
        switch (row.kind) {
            case RowKind::Section: {
                const HWND label =
                    CreateWindowExW(0, L"STATIC", row.label, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0,
                                    10, 10, window_, nullptr, instance_, nullptr);
                SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
                controls_.push_back({label, 0, RowKind::Section, &row, i});
                break;
            }

            case RowKind::Toggle: {
                // The whole card is the checkbox, so anywhere on the row is a
                // hit - which is how every toggle row in Windows 11 behaves.
                const HWND check = CreateWindowExW(
                    0, L"BUTTON", row.label, common | BS_AUTOCHECKBOX, 0, 0, 10, 10, window_,
                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(row.command)), instance_, nullptr);
                SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                controls_.push_back({check, row.command, RowKind::Toggle, &row, i});

                if (HasChildren(i)) {
                    // Its own button, so the keyboard can open the group and a
                    // screen reader has something to announce. It sits on top
                    // of the card and paints the card's own colour behind it.
                    const UINT command = kExpandFirst + static_cast<UINT>(i);
                    const HWND chevron = CreateWindowExW(
                        0, L"BUTTON", L"Show more", common | BS_PUSHBUTTON, 0, 0, 10, 10, window_,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(command)), instance_, nullptr);
                    SendMessageW(chevron, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                    Control entry{chevron, command, RowKind::Toggle, &row, i};
                    entry.chevron = true;
                    entry.partner = check;
                    controls_.push_back(entry);
                    controls_[controls_.size() - 2].partner = chevron;
                }
                break;
            }

            case RowKind::Buttons: {
                const HWND button = CreateWindowExW(
                    0, L"BUTTON", row.label, common | BS_PUSHBUTTON, 0, 0, 10, 10, window_,
                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(row.command)), instance_, nullptr);
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                controls_.push_back({button, row.command, RowKind::Buttons, &row, i});

                if (row.second_label) {
                    const HWND other = CreateWindowExW(
                        0, L"BUTTON", row.second_label, common | BS_PUSHBUTTON, 0, 0, 10, 10,
                        window_,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(row.second_command)),
                        instance_, nullptr);
                    SendMessageW(other, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                    controls_.push_back({other, row.second_command, RowKind::Buttons, &row, i});
                }
                break;
            }
        }
    }
}

int SettingsWindow::NaturalWidth() {
    const int chevron_width = Scale(kChevronWidthDip, dpi_);
    const HDC dc = GetDC(window_);
    int widest = 0;
    for (const Row& row : kRows) {
        int width = 0;
        switch (row.kind) {
            case RowKind::Section:
                width = TextWidth(dc, section_font_, row.label);
                break;
            case RowKind::Toggle: {
                // Text starts after the icon and ends before "On" and the
                // switch, and the explanation underneath has the same room.
                const int furniture = TextLeftFor(row) + Scale(kStateGapDip * 2, dpi_) +
                                      TextWidth(dc, font_, L"Off") + Scale(kTrackWidthDip, dpi_) +
                                      Scale(kCardPadDip, dpi_) + chevron_width;
                width = furniture + std::max(TextWidth(dc, font_, row.label),
                                             row.note ? TextWidth(dc, note_font_, row.note) : 0);
                break;
            }
            case RowKind::Buttons:
                width = TextWidth(dc, font_, row.label) + Scale(kButtonPadDip, dpi_);
                if (row.second_label) {
                    width += TextWidth(dc, font_, row.second_label) + Scale(kButtonPadDip, dpi_) +
                             Scale(kButtonGapDip, dpi_);
                }
                break;
        }
        widest = std::max(widest, width);
    }
    ReleaseDC(window_, dc);
    return std::clamp(widest, Scale(kMinWidthDip, dpi_), Scale(kMaxWidthDip, dpi_));
}

void SettingsWindow::Layout() {
    if (!window_ || controls_.empty()) return;

    const int margin = Scale(kMarginDip, dpi_);
    const int chevron_width = Scale(kChevronWidthDip, dpi_);

    RECT client{};
    GetClientRect(window_, &client);
    // The rows fill the window's width, so widening it widens the cards. Below
    // the natural width they would start ellipsising, which is what the minimum
    // size in WM_GETMINMAXINFO is for.
    const int content = std::max(static_cast<int>(client.right) - margin * 2, NaturalWidth());
    const int left = margin;

    // Everything is placed against the top of the content first, and the scroll
    // offset applied at the end - the total height is not known until the walk
    // is over, and it is what the offset has to be clamped against.
    struct Placement {
        HWND window;
        int x, y, width, height;
    };
    std::vector<Placement> places;
    places.reserve(controls_.size());

    int y = Scale(kTopMarginDip, dpi_);
    bool first_section = true;
    size_t group = kRowCount;  // the header whose children we are inside

    for (size_t index = 0; index < controls_.size(); ++index) {
        Control& control = controls_[index];
        const Row& row = kRows[control.row_index];
        const bool child = row.depth > 0;
        if (!child) group = HasChildren(control.row_index) ? control.row_index : kRowCount;

        const bool visible = !child || (group < kRowCount && expanded_[group]);
        control.child = child;
        control.last_child = child && IsLastChild(control.row_index);

        if (!visible) {
            ShowWindow(control.window, SW_HIDE);
            continue;
        }

        // A chevron and the second button of a pair share a row with the
        // control before them, and are placed by it.
        if (control.chevron) {
            control.corners = 0;
            continue;
        }
        if (index > 0 && control.kind == RowKind::Buttons &&
            controls_[index - 1].row_index == control.row_index) {
            continue;
        }

        switch (row.kind) {
            case RowKind::Section: {
                if (!first_section) y += Scale(kSectionTopDip, dpi_);
                first_section = false;
                places.push_back({control.window, left, y, content, Scale(kSectionHeightDip, dpi_)});
                y += Scale(kSectionHeightDip, dpi_) + Scale(kSectionGapDip, dpi_);
                break;
            }

            case RowKind::Toggle: {
                const int height = Scale(row.note ? kCardWithNoteDip : kCardHeightDip, dpi_);
                if (child) {
                    control.corners = control.last_child ? (kBottomLeft | kBottomRight) : 0;
                } else if (HasChildren(control.row_index) && expanded_[control.row_index]) {
                    control.corners = kTopLeft | kTopRight;
                } else {
                    control.corners = kTopLeft | kTopRight | kBottomLeft | kBottomRight;
                }
                // A row with a chevron gives up its right-hand end to it. The
                // two do not overlap - each draws its share of one card - so
                // the tab order can stay in reading order: row, then chevron.
                const bool has_chevron =
                    index + 1 < controls_.size() && controls_[index + 1].chevron;
                const int row_width = content - (has_chevron ? chevron_width : 0);
                places.push_back({control.window, left, y, row_width, height});
                if (has_chevron) {
                    places.push_back(
                        {controls_[index + 1].window, left + row_width, y, chevron_width, height});
                }
                y += height;
                // Rows inside a group are one shape; separate cards are not.
                const bool group_continues =
                    (HasChildren(control.row_index) && expanded_[control.row_index]) ||
                    (child && !control.last_child);
                if (!group_continues) y += Scale(kCardGapDip, dpi_);
                break;
            }

            case RowKind::Buttons: {
                const int height = Scale(kButtonHeightDip, dpi_);
                const int button_pad = Scale(kButtonPadDip, dpi_);
                const HDC measure = GetDC(window_);
                const int width = TextWidth(measure, font_, row.label) + button_pad;
                const int second =
                    row.second_label ? TextWidth(measure, font_, row.second_label) + button_pad : 0;
                ReleaseDC(window_, measure);
                control.corners = kTopLeft | kTopRight | kBottomLeft | kBottomRight;

                places.push_back({control.window, left, y, width, height});
                // The second button of a pair follows the first one along.
                if (second > 0 && index + 1 < controls_.size() &&
                    controls_[index + 1].row_index == control.row_index) {
                    controls_[index + 1].corners = control.corners;
                    places.push_back({controls_[index + 1].window,
                                      left + width + Scale(kButtonGapDip, dpi_), y, second, height});
                }
                y += height + Scale(kCardGapDip, dpi_);
                break;
            }
        }
    }

    content_height_ = y + Scale(kMarginDip, dpi_) - Scale(kCardGapDip, dpi_);
    const int page = static_cast<int>(client.bottom);
    scroll_ = std::clamp(scroll_, 0, std::max(0, content_height_ - page));

    HDWP defer = BeginDeferWindowPos(static_cast<int>(places.size()));
    for (const Placement& place : places) {
        defer = DeferWindowPos(defer, place.window, nullptr, place.x, place.y - scroll_, place.width,
                               place.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    EndDeferWindowPos(defer);

    UpdateScrollBar();
    InvalidateRect(window_, nullptr, TRUE);
}

void SettingsWindow::UpdateScrollBar() {
    RECT client{};
    GetClientRect(window_, &client);

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(content_height_ - 1, 0);
    info.nPage = static_cast<UINT>(std::max<LONG>(client.bottom, 1));
    info.nPos = scroll_;
    // Without SIF_DISABLENOSCROLL the bar takes itself away when everything
    // fits, which is the behaviour wanted: no furniture on a big screen.
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
}

void SettingsWindow::ScrollTo(int offset) {
    RECT client{};
    GetClientRect(window_, &client);
    const int limit = std::max(0, content_height_ - static_cast<int>(client.bottom));
    const int wanted = std::clamp(offset, 0, limit);
    if (wanted == scroll_) return;

    const int delta = scroll_ - wanted;
    scroll_ = wanted;
    // Scrolling the window moves the rows with it, children included, which is
    // smoother than putting each one back by hand.
    ScrollWindowEx(window_, 0, delta, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    UpdateScrollBar();
    UpdateWindow(window_);
}

void SettingsWindow::EnsureVisible(HWND control) {
    if (!control || !window_) return;
    RECT bounds{};
    GetWindowRect(control, &bounds);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);

    RECT client{};
    GetClientRect(window_, &client);
    const int margin = Scale(kCardGapDip, dpi_);

    if (bounds.top < margin) {
        ScrollTo(scroll_ + bounds.top - margin);
    } else if (bounds.bottom > client.bottom - margin) {
        ScrollTo(scroll_ + bounds.bottom - client.bottom + margin);
    }
}

void SettingsWindow::SizeToContent() {
    // Open showing everything, unless that would be taller than the screen -
    // in which case open as tall as the screen sensibly allows and let the
    // rest scroll.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const int room = monitor.rcWork.bottom - monitor.rcWork.top;

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi_);
    const int chrome_height = (frame.bottom - frame.top);
    const int chrome_width = (frame.right - frame.left);

    // Not std::clamp: on a screen short enough that the minimum height is more
    // than the room available, its bounds would cross over, which is undefined.
    // The minimum wins there, and the window overhangs slightly rather than
    // becoming a sliver.
    const int fits = std::min(content_height_, room - room / 10 - chrome_height);
    const int height = std::max(fits, Scale(kMinHeightDip, dpi_));
    int width = NaturalWidth() + Scale(kMarginDip, dpi_) * 2;
    // Room for the scroll bar, so the rows are not squeezed when one appears.
    if (height < content_height_) width += GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);

    SetWindowPos(window_, nullptr, 0, 0, width + chrome_width, height + chrome_height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void SettingsWindow::ApplyTheme() {
    if (!window_) return;
    dark_ = AppsUseDarkTheme();

    if (background_) DeleteObject(background_);
    background_ = CreateSolidBrush(PaletteFor(dark_).window);

    // The title bar is the system's; this is the only way to ask for a dark one.
    const BOOL dark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    AllowDarkModeForWindow(window_);
    // The window's own theme is what colours the scroll bar, which is drawn by
    // the frame rather than by anything here.
    SetWindowTheme(window_, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
    for (const Control& control : controls_) {
        if (!control.window) continue;
        AllowDarkModeForWindow(control.window);
        SetWindowTheme(control.window, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
        InvalidateRect(control.window, nullptr, TRUE);
    }
    InvalidateRect(window_, nullptr, TRUE);
}

void SettingsWindow::OnThemeChanged() {
    if (!window_) return;
    if (AppsUseDarkTheme() == dark_) return;
    ApplyTheme();
}

void SettingsWindow::Refresh() {
    if (!window_ || !settings_) return;

    for (const Control& control : controls_) {
        if (!control.window || control.command == 0 || control.chevron) continue;
        EnableWindow(control.window, RowEnabled(*settings_, control.command) ? TRUE : FALSE);

        // Check states come from the settings rather than from the click, so a
        // setting the host declined to change shows what actually happened.
        if (control.kind == RowKind::Toggle && control.row && control.row->field) {
            SendMessageW(control.window, BM_SETCHECK,
                         settings_->*(control.row->field) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        InvalidateRect(control.window, nullptr, TRUE);
    }
    for (const Control& control : controls_) {
        if (control.chevron) InvalidateRect(control.window, nullptr, TRUE);
    }
}

SettingsWindow::Control* SettingsWindow::ControlFor(HWND window) {
    for (Control& control : controls_) {
        if (control.window == window) return &control;
    }
    return nullptr;
}

// One row, drawn the way Windows 11 draws them: a rounded panel, the name on
// the left, an explanation under it when there is one, and the switch itself
// hard against the right edge - or, for a row inside an open group, the same
// without the card's own edges.
void SettingsWindow::PaintCard(NMCUSTOMDRAW* custom, Control& control) {
    const Palette palette = PaletteFor(dark_);
    const RECT bounds = custom->rc;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return;

    const bool enabled = (custom->uItemState & CDIS_DISABLED) == 0;
    const bool pressed = (custom->uItemState & CDIS_SELECTED) != 0;
    // Windows hides focus rectangles until the keyboard has been used, and
    // says so in one flag among several - so test the flag, not the whole word.
    const bool show_focus =
        (SendMessageW(window_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
    const bool focused = (custom->uItemState & CDIS_FOCUS) != 0 && show_focus;
    // A header and its chevron light up together, so the pointer crossing the
    // seam between them does not look like it left the row.
    const bool hot = CursorOver(control.window) ||
                     (control.partner != nullptr && CursorOver(control.partner));
    const bool on = control.row && control.row->field &&
                    SendMessageW(control.window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool expanded = control.row_index < expanded_.size() && expanded_[control.row_index];

    // Drawn away from the screen and blitted, so hovering a row does not make
    // it blink.
    const HDC target = custom->hdc;
    const HDC memory = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    const HGDIOBJ old_bitmap = SelectObject(memory, bitmap);

    RECT local{0, 0, width, height};
    FillRect(memory, &local, background_);

    const float radius = static_cast<float>(Scale(kCardRadiusDip, dpi_));
    const int pad = Scale(kCardPadDip, dpi_);

    {
        Gdiplus::Graphics graphics(memory);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        COLORREF fill = control.child ? palette.group : palette.card;
        if (!enabled) {
            fill = control.child ? palette.group : RGB(dark_ ? 39 : 249, dark_ ? 39 : 249,
                                                       dark_ ? 39 : 249);
        } else if (pressed && !control.chevron) {
            fill = palette.card_pressed;
        } else if (hot) {
            fill = control.child ? palette.group_hot : palette.card_hot;
        }

        if (control.chevron) {
            // The chevron sits on top of the right-hand end of its header and
            // hides whatever the header drew there. Rather than patch the hole,
            // it draws the same card again, shifted: one shape, one outline, no
            // seam down the middle of a row.
            RECT header_bounds{};
            RECT own_bounds{};
            GetWindowRect(control.partner, &header_bounds);
            GetWindowRect(control.window, &own_bounds);
            const float offset = static_cast<float>(own_bounds.left - header_bounds.left);
            // The card is both halves together: the row up to here, and this.
            const float card_width = static_cast<float>(own_bounds.right - header_bounds.left);
            const Control* header = ControlFor(control.partner);

            graphics.TranslateTransform(-offset, 0.0f);
            const Gdiplus::RectF card(0.5f, 0.5f, card_width - 1.0f,
                                      static_cast<float>(height) - 1.0f);
            Gdiplus::GraphicsPath* path =
                RoundedPath(card, radius, header ? header->corners : 0);
            Gdiplus::SolidBrush brush(Argb(fill));
            graphics.FillPath(&brush, path);
            Gdiplus::Pen pen(Argb(palette.border), 1.0f);
            const bool open_bottom = header && (header->corners & (kBottomLeft | kBottomRight)) == 0;
            if (open_bottom) {
                graphics.SetClip(Gdiplus::RectF(0.0f, 0.0f, card_width,
                                                static_cast<float>(height) - 1.0f));
            }
            graphics.DrawPath(&pen, path);
            graphics.ResetClip();
            delete path;

            // The focus rectangle belongs to the row, so it runs through here
            // too when the row is what has focus.
            if (GetFocus() == control.partner && show_focus) {
                const float inset = static_cast<float>(Scale(2, dpi_));
                const Gdiplus::RectF ring(inset, inset, card_width - inset * 2,
                                          static_cast<float>(height) - inset * 2);
                Gdiplus::GraphicsPath* focus_path = RoundedPath(
                    ring, std::max(radius - inset, 2.0f),
                    kTopLeft | kTopRight | kBottomLeft | kBottomRight);
                Gdiplus::Pen focus_pen(Argb(palette.accent), static_cast<float>(Scale(2, dpi_)));
                graphics.DrawPath(&focus_pen, focus_path);
                delete focus_path;
            }
            graphics.ResetTransform();

            // Its own highlight, for when the pointer is on the chevron itself
            // rather than anywhere on the row.
            if (CursorOver(control.window) || pressed || focused) {
                const float inset = static_cast<float>(Scale(4, dpi_));
                const Gdiplus::RectF spot(inset, inset, width - inset * 2, height - inset * 2);
                Gdiplus::GraphicsPath* spot_path = RoundedPath(
                    spot, static_cast<float>(Scale(4, dpi_)),
                    kTopLeft | kTopRight | kBottomLeft | kBottomRight);
                if (CursorOver(control.window) || pressed) {
                    Gdiplus::SolidBrush spot_brush(
                        Argb(pressed ? palette.card_pressed : palette.card_hot));
                    graphics.FillPath(&spot_brush, spot_path);
                }
                if (focused) {
                    Gdiplus::Pen focus_pen(Argb(palette.accent), static_cast<float>(Scale(2, dpi_)));
                    graphics.DrawPath(&focus_pen, spot_path);
                }
                delete spot_path;
            }
        } else {
            // A row that has a chevron is only part of its card: the shape is
            // drawn at the card's full width and the rest of it falls outside
            // this control, where the chevron draws the same shape again.
            const float card_width =
                static_cast<float>(width + (control.partner ? Scale(kChevronWidthDip, dpi_) : 0));
            const Gdiplus::RectF card(0.5f, 0.5f, card_width - 1.0f,
                                      static_cast<float>(height) - 1.0f);
            Gdiplus::GraphicsPath* path = RoundedPath(card, radius, control.corners);
            Gdiplus::SolidBrush brush(Argb(fill));
            graphics.FillPath(&brush, path);

            // The outline is continuous down a group, so the edges that meet
            // the next row are clipped away rather than drawn twice.
            Gdiplus::Pen pen(Argb(palette.border), 1.0f);
            const bool open_bottom = (control.corners & (kBottomLeft | kBottomRight)) == 0;
            const bool open_top = (control.corners & (kTopLeft | kTopRight)) == 0;
            graphics.SetClip(Gdiplus::RectF(0.0f, open_top ? 1.0f : 0.0f, card_width,
                                            static_cast<float>(height) - (open_top ? 1.0f : 0.0f) -
                                                (open_bottom ? 1.0f : 0.0f)));
            graphics.DrawPath(&pen, path);
            graphics.ResetClip();
            delete path;

            // The hairline between rows of a group.
            if (control.child) {
                Gdiplus::Pen line(Argb(palette.border), 1.0f);
                graphics.DrawLine(&line, 0.5f, 0.5f, width - 1.0f, 0.5f);
            }

            if (focused) {
                // Windows 11's focus rectangle: a thick accent outline just
                // inside the row's own edge.
                const float inset = static_cast<float>(Scale(2, dpi_));
                const Gdiplus::RectF ring(inset, inset, card_width - inset * 2,
                                          static_cast<float>(height) - inset * 2);
                Gdiplus::GraphicsPath* focus_path = RoundedPath(
                    ring, std::max(radius - inset, 2.0f),
                    kTopLeft | kTopRight | kBottomLeft | kBottomRight);
                Gdiplus::Pen focus_pen(Argb(palette.accent), static_cast<float>(Scale(2, dpi_)));
                graphics.DrawPath(&focus_pen, focus_path);
                delete focus_path;
            }

            if (control.kind == RowKind::Toggle) {
                const float track_w = static_cast<float>(Scale(kTrackWidthDip, dpi_));
                const float track_h = static_cast<float>(Scale(kTrackHeightDip, dpi_));
                const float knob = static_cast<float>(Scale(kKnobDip, dpi_));
                // A row inside a group is full width, but its switch lines up
                // with the one on the header above it - which gave up its right
                // hand end to the chevron.
                const int chevron_room = control.child ? Scale(kChevronWidthDip, dpi_) : 0;
                const float right = static_cast<float>(width - pad - chevron_room);
                const Gdiplus::RectF track(right - track_w, (height - track_h) / 2.0f, track_w,
                                           track_h);

                if (on) {
                    const Gdiplus::Color accent =
                        enabled ? Argb(palette.accent) : Argb(palette.disabled);
                    Gdiplus::GraphicsPath* pill = RoundedPath(
                        track, track_h / 2.0f,
                        kTopLeft | kTopRight | kBottomLeft | kBottomRight);
                    Gdiplus::SolidBrush pill_brush(accent);
                    graphics.FillPath(&pill_brush, pill);
                    delete pill;
                    Gdiplus::SolidBrush thumb(Argb(enabled ? palette.knob_on : palette.card));
                    graphics.FillEllipse(&thumb, track.GetRight() - (track_h - knob) / 2.0f - knob,
                                         track.Y + (track_h - knob) / 2.0f, knob, knob);
                } else {
                    Gdiplus::GraphicsPath* pill = RoundedPath(
                        track, track_h / 2.0f,
                        kTopLeft | kTopRight | kBottomLeft | kBottomRight);
                    Gdiplus::SolidBrush pill_brush(Argb(palette.track_off));
                    graphics.FillPath(&pill_brush, pill);
                    Gdiplus::Pen edge(Argb(enabled ? palette.track_off_border : palette.disabled),
                                      1.0f);
                    graphics.DrawPath(&edge, pill);
                    delete pill;
                    Gdiplus::SolidBrush thumb(Argb(enabled ? palette.knob_off : palette.disabled));
                    graphics.FillEllipse(&thumb, track.X + (track_h - knob) / 2.0f,
                                         track.Y + (track_h - knob) / 2.0f, knob, knob);
                }
            }
        }
    }

    SetBkMode(memory, TRANSPARENT);
    const COLORREF ink = enabled ? palette.text : palette.disabled;

    if (control.chevron) {
        if (chevron_font_) {
            SelectObject(memory, chevron_font_);
            SetTextColor(memory, ink);
            const wchar_t glyph[2] = {expanded ? L'\xE70E' : L'\xE70D', L'\0'};
            DrawTextW(memory, glyph, 1, &local, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        }
    } else if (control.kind == RowKind::Buttons) {
        SelectObject(memory, font_);
        SetTextColor(memory, ink);
        wchar_t label[128]{};
        GetWindowTextW(control.window, label, ARRAYSIZE(label));
        DrawTextW(memory, label, -1, &local,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        wchar_t label[256]{};
        GetWindowTextW(control.window, label, ARRAYSIZE(label));
        const wchar_t* note = control.row ? control.row->note : nullptr;
        const int chevron_room = control.child ? Scale(kChevronWidthDip, dpi_) : 0;

        // "On" or "Off" beside the switch, the way Windows labels its own.
        const wchar_t* state = on ? L"On" : L"Off";
        SelectObject(memory, font_);
        SIZE state_extent{};
        GetTextExtentPoint32W(memory, state, static_cast<int>(wcslen(state)), &state_extent);
        const int track_left = width - pad - chevron_room - Scale(kTrackWidthDip, dpi_);
        RECT state_rect{track_left - Scale(kStateGapDip, dpi_) - state_extent.cx, 0,
                        track_left - Scale(kStateGapDip, dpi_), height};
        SetTextColor(memory, ink);
        DrawTextW(memory, state, -1, &state_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // A row inside a group has no icon of its own; its text lines up with
        // the title of the row that opened it.
        const int text_left = control.child ? Scale(kTextLeftDip, dpi_)
                                            : (control.row ? TextLeftFor(*control.row) : pad);
        const int text_right = state_rect.left - Scale(kStateGapDip, dpi_);

        if (icon_font_ && control.row && control.row->glyph && !control.child) {
            SelectObject(memory, icon_font_);
            SetTextColor(memory, ink);
            const wchar_t glyph[2] = {control.row->glyph, L'\0'};
            RECT icon{Scale(kIconLeftDip, dpi_), 0, text_left, height};
            DrawTextW(memory, glyph, 1, &icon, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        }

        SelectObject(memory, font_);
        SetTextColor(memory, ink);
        if (note) {
            RECT title{text_left, Scale(14, dpi_), text_right, Scale(34, dpi_)};
            DrawTextW(memory, label, -1, &title, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT second{text_left, Scale(36, dpi_), text_right, Scale(56, dpi_)};
            SelectObject(memory, note_font_);
            SetTextColor(memory, enabled ? palette.secondary : palette.disabled);
            DrawTextW(memory, note, -1, &second, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            RECT title{text_left, 0, text_right, height};
            DrawTextW(memory, label, -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    BitBlt(target, bounds.left, bounds.top, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);

    // The other half of the row shows the same hover state, so when this one
    // changes its mind the other is asked to catch up. It does not ask back.
    if (control.partner && control.partner_hot != hot) {
        control.partner_hot = hot;
        InvalidateRect(control.partner, nullptr, TRUE);
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (!header || header->code != NM_CUSTOMDRAW) break;
            auto* custom = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
            if (custom->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
            Control* control = self->ControlFor(header->hwndFrom);
            if (!control) return CDRF_DODEFAULT;
            self->PaintCard(custom, *control);
            return CDRF_SKIPDEFAULT;
        }

        case WM_COMMAND: {
            const UINT id = LOWORD(wparam);
            const UINT code = HIWORD(wparam);

            // Focus moved rather than something being clicked: redraw both
            // halves of the row, since the focus rectangle spans them.
            if (code == BN_SETFOCUS || code == BN_KILLFOCUS) {
                if (Control* control = self->ControlFor(reinterpret_cast<HWND>(lparam))) {
                    // Tabbing onto a row below the fold brings it into view,
                    // or the keyboard would be driving something off screen.
                    if (code == BN_SETFOCUS) self->EnsureVisible(control->window);
                    InvalidateRect(control->window, nullptr, TRUE);
                    if (control->partner) InvalidateRect(control->partner, nullptr, TRUE);
                }
                return 0;
            }

            // Escape and Enter arrive as these, by way of IsDialogMessage in
            // the host's message loop. Both mean "done here".
            if (id == IDCANCEL || id == IDOK) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            if (id >= kExpandFirst) {
                const size_t row = id - kExpandFirst;
                if (row < self->expanded_.size()) {
                    self->expanded_[row] = !self->expanded_[row];
                    for (Control& control : self->controls_) {
                        if (control.chevron && control.row_index == row) {
                            SetWindowTextW(control.window,
                                           self->expanded_[row] ? L"Show less" : L"Show more");
                        }
                    }
                    self->Layout();
                    // Opening a group makes the window taller if there is room
                    // for it - but never once the window has been sized by
                    // hand, since that size was a decision.
                    if (!self->user_sized_) {
                        self->SizeToContent();
                        self->Layout();
                    }
                    self->Refresh();
                }
                return 0;
            }
            if (id != 0 && self->commands_) {
                // The host owns the settings and knows how to persist them;
                // this window only reports what was clicked and redraws after.
                SendMessageW(self->commands_, WM_COMMAND, wparam, lparam);
                self->Refresh();
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            const Palette palette = PaletteFor(self->dark_);
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, palette.text);
            SetBkColor(dc, palette.window);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(self->background_);
        }

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, self->background_);
            return 1;
        }

        case WM_SIZE:
            self->Layout();
            return 0;

        case WM_EXITSIZEMOVE:
            // Once the window has been given a size by hand, it keeps it:
            // opening a group no longer grows the window under the pointer.
            self->user_sized_ = true;
            return 0;

        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            if (!self->window_) break;
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
            RECT chrome{0, 0, 0, 0};
            AdjustWindowRectExForDpi(&chrome, style, FALSE, 0, self->dpi_);
            const int chrome_width = chrome.right - chrome.left;
            const int chrome_height = chrome.bottom - chrome.top;

            // Narrower than this and the rows would start ellipsising; shorter
            // than this and there is nothing left to look at.
            limits->ptMinTrackSize.x = self->NaturalWidth() + Scale(kMarginDip, self->dpi_) * 2 +
                                       GetSystemMetricsForDpi(SM_CXVSCROLL, self->dpi_) +
                                       chrome_width;
            limits->ptMinTrackSize.y = Scale(kMinHeightDip, self->dpi_) + chrome_height;
            // No point being taller than the rows themselves.
            limits->ptMaxTrackSize.y = self->content_height_ + chrome_height;
            return 0;
        }

        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &info);
            const int line = Scale(kCardHeightDip, self->dpi_) / 2;
            int position = self->scroll_;
            switch (LOWORD(wparam)) {
                case SB_LINEUP: position -= line; break;
                case SB_LINEDOWN: position += line; break;
                case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
                case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: position = info.nTrackPos; break;
                case SB_TOP: position = 0; break;
                case SB_BOTTOM: position = self->content_height_; break;
                default: return 0;
            }
            self->ScrollTo(position);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // A button hands the wheel up to its parent unhandled, so this is
            // reached wherever the pointer is inside the window.
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) {
                RECT client{};
                GetClientRect(window, &client);
                lines = static_cast<UINT>(std::max<LONG>(client.bottom / Scale(26, self->dpi_), 1));
            }
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            self->ScrollTo(self->scroll_ - delta * static_cast<int>(lines) *
                                               Scale(26, self->dpi_) / WHEEL_DELTA);
            return 0;
        }

        case WM_SETTINGCHANGE:
            // A theme switch arrives as a broadcast naming the colour set.
            if (lparam && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lparam), -1,
                                               L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
                self->OnThemeChanged();
            }
            return 0;

        case WM_DPICHANGED: {
            self->dpi_ = HIWORD(wparam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            self->BuildControls();
            self->Layout();
            self->ApplyTheme();
            self->Refresh();
            return 0;
        }

        case WM_CLOSE:
            // Kept rather than destroyed: reopening is instant, and the window
            // comes back where it was left.
            ShowWindow(window, SW_HIDE);
            return 0;

        case WM_DESTROY:
            self->window_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace ee
