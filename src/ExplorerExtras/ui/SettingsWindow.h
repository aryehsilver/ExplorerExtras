// SettingsWindow.h - the app's own window, so the tray menu can stay a menu.
//
// Every option lives here. The tray keeps only what someone reaches for in a
// hurry - the master switch, the recent folders, a way in here - because a
// context menu of fifteen checkboxes is a settings window with worse manners:
// it cannot group anything, it cannot explain anything, and it closes the
// moment you change one thing.
//
// The window owns no state beyond which groups are open. It reads the host's
// Settings to draw itself and sends the host a WM_COMMAND - the same IDM_ ids
// the menu uses - when a row is clicked, so both surfaces run through one
// handler. The host calls Refresh() afterwards and the rows redraw from
// whatever the settings now say.
//
// The rows are real checkboxes underneath, custom drawn as Windows 11 cards
// with a toggle switch on the right. Drawing them by hand is what gets the
// current look out of a plain Win32 process - WinUI would mean dragging the
// Windows App SDK behind a tray utility - and keeping real controls underneath
// is what keeps Tab, Space, focus rectangles and screen readers working.
//
// A setting that only matters while another one is on lives inside that one,
// behind a chevron, the way the Settings app does it. Collapsed by default:
// the window stays short enough for a laptop screen, and the detail is there
// when it is wanted.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <vector>

#include "../core/Settings.h"

namespace ee {

enum class RowKind { Section, Toggle, Buttons };

// Which corners of a row are rounded. A group of rows is one rounded shape:
// the header rounds the top, the last row rounds the bottom, and everything
// between is square.
enum CardCorner { kTopLeft = 1, kTopRight = 2, kBottomRight = 4, kBottomLeft = 8 };

struct Row;  // the row table, defined with the layout in the .cpp

class SettingsWindow {
public:
    ~SettingsWindow();

    // Creates the window on first call and brings it forward after that.
    // |commands| is sent a WM_COMMAND for every row the user touches.
    bool Show(HINSTANCE instance, HWND commands, const Settings* settings);

    // Redraws the rows from the settings. Cheap, and safe when nothing is open.
    void Refresh();

    bool IsOpen() const { return window_ != nullptr && IsWindowVisible(window_); }

    // For the host message loop: a window of controls needs IsDialogMessage
    // to make Tab, the arrow keys and Escape behave.
    HWND Window() const { return window_; }

private:
    struct Control {
        HWND window = nullptr;
        UINT command = 0;
        RowKind kind = RowKind::Section;
        const Row* row = nullptr;
        size_t row_index = 0;

        bool chevron = false;     // the expand button rather than the row itself
        bool child = false;       // drawn on a group's inner surface
        bool last_child = false;  // closes the group off
        int corners = 0;
        HWND partner = nullptr;  // a header and its chevron, pointing at each other
        bool partner_hot = false;
    };

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    bool Create(HINSTANCE instance);
    void BuildControls();  // creates the controls and the fonts
    void Layout();         // places them, and decides what is visible
    int NaturalWidth();    // the width at which nothing has to ellipsise
    void SizeToContent();  // the opening size: all of it, or as much as fits
    void UpdateScrollBar();
    void ScrollTo(int offset);
    void EnsureVisible(HWND control);  // scrolls a focused row into view
    void ApplyTheme();
    void OnThemeChanged();
    void PaintCard(NMCUSTOMDRAW* custom, Control& control);
    // Where a card's text begins: after the icon, or at the padding when the
    // row has no icon or the icon font is missing.
    int TextLeftFor(const Row& row) const;
    Control* ControlFor(HWND window);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND commands_ = nullptr;
    const Settings* settings_ = nullptr;

    HFONT font_ = nullptr;          // 14px, the body
    HFONT note_font_ = nullptr;     // 12px, the line under a title
    HFONT section_font_ = nullptr;  // 14px semibold, the group headings
    HFONT icon_font_ = nullptr;     // Segoe Fluent Icons, or null where it is missing
    HFONT chevron_font_ = nullptr;  // the same face, smaller
    HBRUSH background_ = nullptr;
    bool dark_ = false;
    bool created_ = false;
    UINT dpi_ = 96;

    // The window is resizable and the rows scroll inside it, so that a small
    // screen gets a window that fits rather than one that runs off the bottom.
    int scroll_ = 0;          // pixels hidden above the top of the client area
    int content_height_ = 0;  // how tall the rows are, open groups included
    bool user_sized_ = false; // whether the window has been resized by hand

    std::vector<Control> controls_;
    std::vector<bool> expanded_;  // one per row; only the group headers matter
};

}  // namespace ee
