#include "ExplorerSession.h"

#include <exdisp.h>
#include <objbase.h>
#include <ocidl.h>  // IObjectWithSite
#include <oleauto.h>
#include <servprov.h>
#include <shellapi.h>  // CMIC_MASK_* expand to SEE_MASK_*
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {
namespace {

constexpr wchar_t kTabWindowClass[] = L"ShellTabWindowClass";

// The site handed to a context menu when invoking a browser-scoped verb.
//
// A verb marked OnlyInBrowserWindow - "Open in new tab" is one - asks its site
// for a browser before it will even appear in the menu, and it asks through
// IServiceProvider. Handing the IShellBrowser over directly is not enough:
// the shell queries for SID_SShellBrowser and gets E_NOINTERFACE, decides
// there is no browser, and filters the verb out.
class BrowserSite : public IServiceProvider, public IOleWindow {
public:
    BrowserSite(IShellBrowser* browser, HWND frame) : browser_(browser), frame_(frame) {}
    virtual ~BrowserSite() = default;

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IServiceProvider) {
            *ppv = static_cast<IServiceProvider*>(this);
        } else if (riid == IID_IOleWindow) {
            *ppv = static_cast<IOleWindow*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG remaining = InterlockedDecrement(&refs_);
        if (remaining == 0) delete this;
        return remaining;
    }

    // IServiceProvider
    IFACEMETHODIMP QueryService(REFGUID service, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (browser_ && (service == SID_SShellBrowser || service == SID_STopLevelBrowser)) {
            return browser_->QueryInterface(riid, ppv);
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IOleWindow - some handlers ask the site which window to parent UI to.
    IFACEMETHODIMP GetWindow(HWND* window) override {
        if (!window) return E_POINTER;
        *window = frame_;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

private:
    LONG refs_ = 1;
    IShellBrowser* browser_ = nullptr;  // outlives this short-lived site
    HWND frame_ = nullptr;
};

bool ClassNameIs(HWND window, const wchar_t* expected) {
    wchar_t buffer[64]{};
    if (GetClassNameW(window, buffer, ARRAYSIZE(buffer)) == 0) return false;
    return CompareStringOrdinal(buffer, -1, expected, -1, FALSE) == CSTR_EQUAL;
}

}  // namespace

bool IsExplorerWindow(HWND top_level) {
    if (!top_level) return false;
    return ClassNameIs(top_level, L"CabinetWClass") || ClassNameIs(top_level, L"ExploreWClass");
}

HWND FindTabWindow(HWND from) {
    for (HWND window = from; window != nullptr; window = GetAncestor(window, GA_PARENT)) {
        if (ClassNameIs(window, kTabWindowClass)) return window;
        if (IsExplorerWindow(window)) break;  // reached the frame, no tab window below it
    }
    return nullptr;
}

std::optional<ActiveTab> ResolveActiveTab(HWND top_level, HWND tab_window) {
    if (!top_level) return std::nullopt;

    ComPtr<IShellWindows> shell_windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shell_windows));
    if (FAILED(hr)) {
        EE_ERR(L"CoCreateInstance(ShellWindows) failed, hr=0x%08X", hr);
        return std::nullopt;
    }

    long count = 0;
    if (FAILED(shell_windows->get_Count(&count))) return std::nullopt;

    std::optional<ActiveTab> visible_fallback;
    int matching_frames = 0;

    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        ComPtr<IDispatch> dispatch;
        hr = shell_windows->Item(index, &dispatch);
        VariantClear(&index);
        if (FAILED(hr) || !dispatch) continue;

        ComPtr<IWebBrowser2> web_browser;
        if (FAILED(dispatch.As(&web_browser))) continue;

        SHANDLE_PTR frame = 0;
        if (FAILED(web_browser->get_HWND(&frame))) continue;
        if (reinterpret_cast<HWND>(frame) != top_level) continue;
        ++matching_frames;

        ComPtr<IServiceProvider> provider;
        if (FAILED(dispatch.As(&provider))) continue;

        ComPtr<IShellBrowser> shell_browser;
        if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser)))) continue;

        HWND entry_tab = nullptr;
        shell_browser->GetWindow(&entry_tab);

        ComPtr<IShellView> shell_view;
        if (FAILED(shell_browser->QueryActiveShellView(&shell_view)) || !shell_view) continue;

        HWND def_view = nullptr;
        if (FAILED(shell_view->GetWindow(&def_view)) || !def_view) continue;

        // The tab the pointer is over. This is exact, unlike visibility:
        // several background tabs can carry WS_VISIBLE at once, and driving one
        // of those makes BrowseObject fail with E_FAIL.
        if (tab_window && entry_tab == tab_window) {
            return ActiveTab{shell_browser, def_view, entry_tab, true};
        }

        if (!visible_fallback && IsWindowVisible(def_view)) {
            visible_fallback = ActiveTab{shell_browser, def_view, entry_tab, false};
        }
    }

    if (tab_window) {
        EE_WARN(L"no IShellWindows entry matched tab 0x%p (frame 0x%p, %d entries for this frame)",
                tab_window, top_level, matching_frames);
        return std::nullopt;
    }
    return visible_fallback;
}

namespace {

std::wstring CurrentFolderOf(IShellBrowser* browser) {
    if (!browser) return {};

    ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) return {};

    ComPtr<IFolderView> folder_view;
    if (FAILED(view.As(&folder_view))) return {};

    ComPtr<IPersistFolder2> persist;
    if (FAILED(folder_view->GetFolder(IID_PPV_ARGS(&persist)))) return {};

    LPITEMIDLIST pidl = nullptr;
    if (FAILED(persist->GetCurFolder(&pidl)) || !pidl) return {};

    std::wstring result;
    PWSTR name = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
        result = name;
        CoTaskMemFree(name);
    }
    CoTaskMemFree(pidl);
    return result;
}

}  // namespace

std::wstring GetCurrentFolder(const ActiveTab& tab) {
    return CurrentFolderOf(tab.browser.Get());
}

std::wstring FindTabFolder(HWND top_level, const std::wstring& tab_name) {
    if (!top_level || tab_name.empty()) return {};

    ComPtr<IShellWindows> shell_windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&shell_windows)))) {
        return {};
    }

    long count = 0;
    if (FAILED(shell_windows->get_Count(&count))) return {};

    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        ComPtr<IDispatch> dispatch;
        const HRESULT hr = shell_windows->Item(index, &dispatch);
        VariantClear(&index);
        if (FAILED(hr) || !dispatch) continue;

        ComPtr<IWebBrowser2> web_browser;
        if (FAILED(dispatch.As(&web_browser))) continue;

        SHANDLE_PTR frame = 0;
        if (FAILED(web_browser->get_HWND(&frame))) continue;
        if (reinterpret_cast<HWND>(frame) != top_level) continue;

        ComPtr<IServiceProvider> provider;
        if (FAILED(dispatch.As(&provider))) continue;

        ComPtr<IShellBrowser> shell_browser;
        if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser)))) {
            continue;
        }

        const std::wstring path = CurrentFolderOf(shell_browser.Get());
        if (path.empty()) continue;

        const wchar_t* leaf = PathFindFileNameW(path.c_str());
        if (leaf && CompareStringOrdinal(leaf, -1, tab_name.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return path;
        }
    }
    return {};
}

HRESULT NavigateUp(const ActiveTab& tab) {
    if (!tab.browser) return E_POINTER;
    return tab.browser->BrowseObject(nullptr, SBSP_SAMEBROWSER | SBSP_PARENT);
}

HRESULT OpenInNewTab(const ActiveTab& tab, const std::wstring& path) {
    if (!tab.browser) return E_POINTER;
    if (path.empty()) return E_INVALIDARG;

    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
    if (FAILED(hr)) return hr;

    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr)) {
        ComPtr<IContextMenu> menu;
        hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr,
                                   reinterpret_cast<void**>(menu.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            // The site must answer SID_SShellBrowser through IServiceProvider,
            // or the verb is filtered out of the menu entirely.
            const HWND frame = GetAncestor(tab.def_view, GA_ROOT);
            ComPtr<IObjectWithSite> site;
            ComPtr<IUnknown> site_object;
            site_object.Attach(static_cast<IServiceProvider*>(new BrowserSite(tab.browser.Get(), frame)));
            if (SUCCEEDED(menu.As(&site))) {
                site->SetSite(site_object.Get());
            }

            // QueryContextMenu must run before InvokeCommand: it is what builds
            // the verb table. Skipping it is why invoking by name returned
            // E_INVALIDARG. Invoke by command id rather than by name, which is
            // what DelegateExecute verbs reliably answer to.
            const HMENU scratch = CreatePopupMenu();
            constexpr UINT kFirstId = 1;
            hr = menu->QueryContextMenu(scratch, 0, kFirstId, 0x7FFF, CMF_NORMAL);

            UINT command = 0;
            bool found = false;
            std::wstring seen;  // for diagnostics when the verb is missing
            if (SUCCEEDED(hr)) {
                const int count = GetMenuItemCount(scratch);
                for (int i = 0; i < count && !found; ++i) {
                    const UINT id = GetMenuItemID(scratch, i);
                    if (id == static_cast<UINT>(-1) || id < kFirstId) continue;

                    wchar_t verb[64]{};
                    if (FAILED(menu->GetCommandString(id - kFirstId, GCS_VERBW, nullptr,
                                                      reinterpret_cast<CHAR*>(verb),
                                                      ARRAYSIZE(verb)))) {
                        continue;
                    }
                    seen += verb;
                    seen += L' ';
                    if (CompareStringOrdinal(verb, -1, L"opennewtab", -1, TRUE) == CSTR_EQUAL) {
                        command = id - kFirstId;
                        found = true;
                    }
                }
            }

            if (found) {
                CMINVOKECOMMANDINFOEX info{};
                info.cbSize = sizeof(info);
                info.fMask = CMIC_MASK_UNICODE;
                info.hwnd = frame;
                info.lpVerb = MAKEINTRESOURCEA(command);
                info.lpVerbW = MAKEINTRESOURCEW(command);
                info.nShow = SW_SHOWNORMAL;
                hr = menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
            } else {
                // Explorer contributes the browser verbs from its own frame
                // menu, so an out-of-process item menu never carries them.
                // Worth one line per session, not one per click.
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    EE_WARN(L"opennewtab unavailable out-of-process; verbs offered were: %s",
                            seen.c_str());
                }
                hr = E_NOTIMPL;
            }

            DestroyMenu(scratch);
            if (site) site->SetSite(nullptr);
        }
    }

    CoTaskMemFree(pidl);
    return hr;
}

}  // namespace ee
