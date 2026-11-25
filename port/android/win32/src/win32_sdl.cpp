// Android Win32 layer: windows, messages, keyboard, mouse/touch, monitors and XInput, over SDL2.
//
// An HWND is the SDL_Window* itself: DXVK-native's WSI takes the HWND passed to
// CreateSwapChainForHwnd as an SDL_Window*. An HMONITOR is the SDL display index + 1 (also
// DXVK-native's convention). Messages: PeekMessage pumps SDL and turns its events into the
// WM_* messages the game's window procedure expects; GetAsyncKeyState reads SDL's keyboard
// state. XInput pads come from SDL game controllers, merged with the on-screen touch pad that
// the launcher's Java overlay reports through Ttt2SetTouchPad (slot 0).
#include "win32_internal.h"
#include <Xinput.h>

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace w32;

namespace {

struct WindowInfo
{
    WNDPROC proc = nullptr;
    LONG style = 0, exStyle = 0;
    LONG_PTR userData = 0;
    std::string className, title;
    bool visible = true;
};
std::recursive_mutex g_winMutex;
std::unordered_map<SDL_Window*, WindowInfo>& Windows() { static auto* m = new std::unordered_map<SDL_Window*, WindowInfo>(); return *m; }
std::map<std::string, WNDPROC>& Classes() { static auto* m = new std::map<std::string, WNDPROC>(); return *m; }
std::deque<MSG>& Posted() { static auto* d = new std::deque<MSG>(); return *d; }
std::atomic<SDL_Window*> g_focus{ nullptr };
std::atomic<bool> g_quit{ false };

bool EnsureSdl()
{
    static const bool ok = [] {
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        const Uint32 want = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_HAPTIC;
        if ((SDL_WasInit(want) & want) == want) return true;
        if (SDL_InitSubSystem(want) != 0)
        {
            std::fprintf(stderr, "[sdl] SDL_InitSubSystem failed: %s\n", SDL_GetError());
            return false;
        }
        return true;
    }();
    return ok;
}

// SDL scancode -> Windows virtual key
int VkFromScancode(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (s - SDL_SCANCODE_KP_1);
    switch (s)
    {
    case SDL_SCANCODE_RETURN: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_AC_BACK: return VK_ESCAPE;     // Android back button
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
    case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;
    case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
    case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;
    case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
    case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
    case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;
    case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
    case SDL_SCANCODE_APPLICATION: return VK_APPS;
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
    case SDL_SCANCODE_LALT: return VK_LMENU;
    case SDL_SCANCODE_LGUI: return VK_LWIN;
    case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_RALT: return VK_RMENU;
    case SDL_SCANCODE_RGUI: return VK_RWIN;
    default: return 0;
    }
}
SDL_Scancode ScancodeFromVk(int vk)
{
    for (int s = 0; s < SDL_NUM_SCANCODES; ++s) if (VkFromScancode(SDL_Scancode(s)) == vk) return SDL_Scancode(s);
    return SDL_SCANCODE_UNKNOWN;
}
int GenericVk(int vk)
{
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) return VK_SHIFT;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) return VK_CONTROL;
    if (vk == VK_LMENU || vk == VK_RMENU) return VK_MENU;
    return vk;
}

// Mouse buttons from SDL
std::atomic<uint32_t> g_mouseButtons{ 0 };

// ---- XInput state ----
struct TouchPad { std::atomic<uint32_t> buttons{ 0 }; std::atomic<int32_t> lx{ 0 }, ly{ 0 }, rx{ 0 }, ry{ 0 }; std::atomic<uint32_t> lt{ 0 }, rt{ 0 }; std::atomic<bool> active{ false }; };
TouchPad g_touch;
std::mutex g_padMutex;
SDL_GameController* g_pads[XUSER_MAX_COUNT] = {};
std::atomic<DWORD> g_packet{ 0 };

void OpenPads()
{
    std::lock_guard lk(g_padMutex);
    for (auto& p : g_pads) if (p && !SDL_GameControllerGetAttached(p)) { SDL_GameControllerClose(p); p = nullptr; }
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XUSER_MAX_COUNT; ++i)
    {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (auto* p : g_pads) if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) open = true;
        if (open) continue;
        while (slot < XUSER_MAX_COUNT && g_pads[slot]) ++slot;
        if (slot < XUSER_MAX_COUNT) g_pads[slot] = SDL_GameControllerOpen(i);
    }
}

void Translate(const SDL_Event& e)
{
    auto post = [](SDL_Window* w, UINT msg, WPARAM wp, LPARAM lp) {
        MSG m{}; m.hwnd = reinterpret_cast<HWND>(w); m.message = msg; m.wParam = wp; m.lParam = lp; m.time = SDL_GetTicks();
        std::lock_guard lk(g_winMutex);
        Posted().push_back(m);
    };
    SDL_Window* focus = g_focus.load();
    switch (e.type)
    {
    case SDL_QUIT:
        g_quit = true;
        if (focus) post(focus, WM_CLOSE, 0, 0);
        break;
    case SDL_KEYDOWN: case SDL_KEYUP:
    {
        int vk = VkFromScancode(e.key.keysym.scancode);
        if (!vk) break;
        const bool alt = (e.key.keysym.mod & KMOD_ALT) != 0;
        const bool extended = vk == VK_RCONTROL || vk == VK_RMENU || (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE;
        const int sendVk = GenericVk(vk);
        LPARAM lp = 1 | (LPARAM(e.key.keysym.scancode & 0xFF) << 16) | (extended ? (1 << 24) : 0);
        if (alt) lp |= (1 << 29);
        if (e.key.repeat) lp |= (1 << 30);
        if (e.type == SDL_KEYUP) lp |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
        const UINT msg = (alt || sendVk == VK_MENU) ? (e.type == SDL_KEYDOWN ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (e.type == SDL_KEYDOWN ? WM_KEYDOWN : WM_KEYUP);
        post(SDL_GetWindowFromID(e.key.windowID) ? SDL_GetWindowFromID(e.key.windowID) : focus, msg, WPARAM(sendVk), lp);
        if (e.type == SDL_KEYDOWN && (sendVk == VK_BACK || sendVk == VK_RETURN || sendVk == VK_ESCAPE || sendVk == VK_TAB))
            post(focus, WM_CHAR, WPARAM(sendVk == VK_BACK ? 8 : sendVk == VK_RETURN ? 13 : sendVk == VK_ESCAPE ? 27 : 9), lp);
        break;
    }
    case SDL_TEXTINPUT:
    {
        const std::wstring w = Widen(e.text.text);
        for (wchar_t c : w) post(SDL_GetWindowFromID(e.text.windowID) ? SDL_GetWindowFromID(e.text.windowID) : focus, WM_CHAR, WPARAM(c), 1);
        break;
    }
    case SDL_MOUSEMOTION:
        post(SDL_GetWindowFromID(e.motion.windowID), WM_MOUSEMOVE, 0, LPARAM(MAKELONG(e.motion.x, e.motion.y)));
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
    {
        const bool down = e.type == SDL_MOUSEBUTTONDOWN;
        UINT msg = 0;
        uint32_t bit = 0;
        if (e.button.button == SDL_BUTTON_LEFT) { msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; bit = 1; }
        else if (e.button.button == SDL_BUTTON_RIGHT) { msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; bit = 2; }
        else if (e.button.button == SDL_BUTTON_MIDDLE) { msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; bit = 4; }
        if (down) g_mouseButtons |= bit; else g_mouseButtons &= ~bit;
        if (msg) post(SDL_GetWindowFromID(e.button.windowID), msg, 0, LPARAM(MAKELONG(e.button.x, e.button.y)));
        break;
    }
    case SDL_MOUSEWHEEL:
        post(SDL_GetWindowFromID(e.wheel.windowID), WM_MOUSEWHEEL, WPARAM(MAKELONG(0, e.wheel.y * WHEEL_DELTA)), 0);
        break;
    case SDL_WINDOWEVENT:
    {
        SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
        switch (e.window.event)
        {
        case SDL_WINDOWEVENT_FOCUS_GAINED: g_focus = w; post(w, WM_ACTIVATEAPP, TRUE, 0); post(w, WM_SETFOCUS, 0, 0); break;
        case SDL_WINDOWEVENT_FOCUS_LOST: post(w, WM_ACTIVATEAPP, FALSE, 0); post(w, WM_KILLFOCUS, 0, 0); break;
        case SDL_WINDOWEVENT_SIZE_CHANGED: case SDL_WINDOWEVENT_RESIZED:
            post(w, WM_SIZE, SIZE_RESTORED, LPARAM(MAKELONG(e.window.data1, e.window.data2))); break;
        case SDL_WINDOWEVENT_MINIMIZED: post(w, WM_SIZE, SIZE_MINIMIZED, 0); break;
        case SDL_WINDOWEVENT_CLOSE: post(w, WM_CLOSE, 0, 0); break;
        default: break;
        }
        break;
    }
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
        OpenPads();
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        if (focus) post(focus, WM_ACTIVATEAPP, FALSE, 0);
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        if (focus) post(focus, WM_ACTIVATEAPP, TRUE, 0);
        break;
    default:
        break;
    }
}

void Pump()
{
    if (!EnsureSdl()) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) Translate(e);
}

} // namespace

extern "C" void Ttt2PumpSdlEvents(DWORD ms)
{
    Pump();
    if (ms && ms != INFINITE) { SDL_Event e; if (SDL_WaitEventTimeout(&e, int(ms))) Translate(e); }
}
extern "C" SDL_Window* Ttt2SdlWindow(HWND w) { return reinterpret_cast<SDL_Window*>(w); }
// The Java touch overlay: XInput button bits, sticks -32768..32767, triggers 0..255.
extern "C" void Ttt2SetTouchPad(uint32_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry, uint32_t lt, uint32_t rt)
{
    g_touch.buttons = buttons; g_touch.lx = lx; g_touch.ly = ly; g_touch.rx = rx; g_touch.ry = ry; g_touch.lt = lt; g_touch.rt = rt;
    g_touch.active = true;
    g_packet.fetch_add(1);
}

// ======================================================================================
// Classes and windows
// ======================================================================================
extern "C" ATOM RegisterClassA(const WNDCLASSA* wc)
{
    if (!wc || !wc->lpszClassName) return 0;
    std::lock_guard lk(g_winMutex);
    Classes()[wc->lpszClassName] = wc->lpfnWndProc;
    return ATOM(Classes().size());
}
extern "C" ATOM RegisterClassW(const WNDCLASSW* wc)
{
    if (!wc || !wc->lpszClassName) return 0;
    std::lock_guard lk(g_winMutex);
    Classes()[Narrow(wc->lpszClassName)] = wc->lpfnWndProc;
    return ATOM(Classes().size());
}
extern "C" ATOM RegisterClassExA(const WNDCLASSEXA* wc) { WNDCLASSA a{}; a.lpfnWndProc = wc->lpfnWndProc; a.lpszClassName = wc->lpszClassName; return RegisterClassA(&a); }
extern "C" ATOM RegisterClassExW(const WNDCLASSEXW* wc) { WNDCLASSW a{}; a.lpfnWndProc = wc->lpfnWndProc; a.lpszClassName = wc->lpszClassName; return RegisterClassW(&a); }
extern "C" BOOL UnregisterClassA(LPCSTR name, HINSTANCE) { std::lock_guard lk(g_winMutex); return Classes().erase(name ? name : "") > 0; }
extern "C" BOOL UnregisterClassW(LPCWSTR name, HINSTANCE) { std::lock_guard lk(g_winMutex); return Classes().erase(Narrow(name)) > 0; }

extern "C" HWND CreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int, int, int w, int h, HWND, HMENU, HINSTANCE inst, LPVOID param)
{
    if (!EnsureSdl()) return nullptr;
    WNDPROC proc = nullptr;
    {
        std::lock_guard lk(g_winMutex);
        auto it = Classes().find(cls ? cls : "");
        if (it != Classes().end()) proc = it->second;
    }
    // Android: one fullscreen landscape surface, whatever size was asked for.
    SDL_DisplayMode dm{};
    SDL_GetDesktopDisplayMode(0, &dm);
    const int ww = dm.w > 0 ? std::max(dm.w, dm.h) : (w > 0 ? w : 1280);
    const int hh = dm.h > 0 ? std::min(dm.w, dm.h) : (h > 0 ? h : 720);
    SDL_Window* win = SDL_CreateWindow(title ? title : "", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, hh,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win)
    {
        std::fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    {
        std::lock_guard lk(g_winMutex);
        WindowInfo& info = Windows()[win];
        info.proc = proc; info.style = LONG(style); info.exStyle = LONG(exStyle);
        info.className = cls ? cls : ""; info.title = title ? title : "";
    }
    g_focus = win;
    OpenPads();
    HWND hwnd = reinterpret_cast<HWND>(win);
    if (proc)
    {
        CREATESTRUCTA cs{}; cs.lpCreateParams = param; cs.hInstance = inst; cs.cx = ww; cs.cy = hh; cs.style = LONG(style);
        cs.lpszName = title; cs.lpszClass = cls; cs.dwExStyle = exStyle;
        proc(hwnd, WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&cs));
        proc(hwnd, WM_CREATE, 0, reinterpret_cast<LPARAM>(&cs));
        proc(hwnd, WM_SIZE, SIZE_RESTORED, LPARAM(MAKELONG(ww, hh)));
        proc(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    }
    std::fprintf(stderr, "[sdl] window %dx%d (requested %dx%d)\n", ww, hh, w, h);
    return hwnd;
}
extern "C" HWND CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    const std::string c = Narrow(cls), t = Narrow(title);
    return CreateWindowExA(exStyle, c.c_str(), t.c_str(), style, x, y, w, h, parent, menu, inst, param);
}
static WindowInfo* Info(HWND w)
{
    auto it = Windows().find(reinterpret_cast<SDL_Window*>(w));
    return it == Windows().end() ? nullptr : &it->second;
}
extern "C" BOOL DestroyWindow(HWND w)
{
    WNDPROC proc = nullptr;
    {
        std::lock_guard lk(g_winMutex);
        WindowInfo* i = Info(w);
        if (!i) return FALSE;
        proc = i->proc;
    }
    if (proc) proc(w, WM_DESTROY, 0, 0);
    {
        std::lock_guard lk(g_winMutex);
        Windows().erase(reinterpret_cast<SDL_Window*>(w));
    }
    if (g_focus.load() == reinterpret_cast<SDL_Window*>(w)) g_focus = nullptr;
    SDL_DestroyWindow(reinterpret_cast<SDL_Window*>(w));
    return TRUE;
}
extern "C" BOOL ShowWindow(HWND w, int cmd)
{
    std::lock_guard lk(g_winMutex);
    WindowInfo* i = Info(w);
    if (!i) return FALSE;
    const BOOL was = i->visible;
    i->visible = cmd != SW_HIDE;
    if (cmd == SW_HIDE) SDL_HideWindow(reinterpret_cast<SDL_Window*>(w)); else SDL_ShowWindow(reinterpret_cast<SDL_Window*>(w));
    return was;
}
extern "C" BOOL UpdateWindow(HWND) { return TRUE; }
extern "C" BOOL IsWindow(HWND w) { std::lock_guard lk(g_winMutex); return Info(w) != nullptr; }
extern "C" BOOL IsWindowVisible(HWND w) { std::lock_guard lk(g_winMutex); WindowInfo* i = Info(w); return i && i->visible; }
extern "C" BOOL IsIconic(HWND w) { return w && (SDL_GetWindowFlags(reinterpret_cast<SDL_Window*>(w)) & SDL_WINDOW_MINIMIZED) != 0; }
extern "C" BOOL IsZoomed(HWND) { return TRUE; }
// Size and position are fixed (the surface is the screen): accept and ignore.
extern "C" BOOL SetWindowPos(HWND w, HWND, int, int, int, int, UINT flags)
{
    if (flags & SWP_SHOWWINDOW) ShowWindow(w, SW_SHOW);
    if (flags & SWP_HIDEWINDOW) ShowWindow(w, SW_HIDE);
    return IsWindow(w);
}
extern "C" BOOL MoveWindow(HWND w, int, int, int, int, BOOL) { return IsWindow(w); }
extern "C" BOOL GetClientRect(HWND w, LPRECT r)
{
    if (!r) return FALSE;
    int ww = 0, hh = 0;
    if (w) SDL_GetWindowSizeInPixels(reinterpret_cast<SDL_Window*>(w), &ww, &hh);
    if (ww <= 0 || hh <= 0) { SDL_DisplayMode dm{}; SDL_GetDesktopDisplayMode(0, &dm); ww = dm.w; hh = dm.h; }
    r->left = 0; r->top = 0; r->right = ww; r->bottom = hh;
    return TRUE;
}
extern "C" BOOL GetWindowRect(HWND w, LPRECT r) { return GetClientRect(w, r); }
extern "C" BOOL AdjustWindowRect(LPRECT, DWORD, BOOL) { return TRUE; }
extern "C" BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }
extern "C" BOOL AdjustWindowRectExForDpi(LPRECT, DWORD, BOOL, DWORD, UINT) { return TRUE; }
extern "C" BOOL ClientToScreen(HWND, LPPOINT) { return TRUE; }
extern "C" BOOL ScreenToClient(HWND, LPPOINT) { return TRUE; }
static LONG_PTR GetLong(HWND w, int index)
{
    std::lock_guard lk(g_winMutex);
    WindowInfo* i = Info(w);
    if (!i) return 0;
    switch (index)
    {
    case GWL_STYLE: return i->style;
    case GWL_EXSTYLE: return i->exStyle;
    case GWLP_USERDATA: return i->userData;
    case GWLP_WNDPROC: return reinterpret_cast<LONG_PTR>(i->proc);
    default: return 0;
    }
}
static LONG_PTR SetLong(HWND w, int index, LONG_PTR v)
{
    std::lock_guard lk(g_winMutex);
    WindowInfo* i = Info(w);
    if (!i) return 0;
    LONG_PTR old = 0;
    switch (index)
    {
    case GWL_STYLE: old = i->style; i->style = LONG(v); break;
    case GWL_EXSTYLE: old = i->exStyle; i->exStyle = LONG(v); break;
    case GWLP_USERDATA: old = i->userData; i->userData = v; break;
    case GWLP_WNDPROC: old = reinterpret_cast<LONG_PTR>(i->proc); i->proc = reinterpret_cast<WNDPROC>(v); break;
    default: break;
    }
    return old;
}
extern "C" LONG GetWindowLongA(HWND w, int i) { return LONG(GetLong(w, i)); }
extern "C" LONG GetWindowLongW(HWND w, int i) { return LONG(GetLong(w, i)); }
extern "C" LONG SetWindowLongA(HWND w, int i, LONG v) { return LONG(SetLong(w, i, v)); }
extern "C" LONG SetWindowLongW(HWND w, int i, LONG v) { return LONG(SetLong(w, i, v)); }
extern "C" LONG_PTR GetWindowLongPtrA(HWND w, int i) { return GetLong(w, i); }
extern "C" LONG_PTR GetWindowLongPtrW(HWND w, int i) { return GetLong(w, i); }
extern "C" LONG_PTR SetWindowLongPtrA(HWND w, int i, LONG_PTR v) { return SetLong(w, i, v); }
extern "C" LONG_PTR SetWindowLongPtrW(HWND w, int i, LONG_PTR v) { return SetLong(w, i, v); }
extern "C" BOOL SetWindowTextA(HWND w, LPCSTR t) { if (w && t) SDL_SetWindowTitle(reinterpret_cast<SDL_Window*>(w), t); return TRUE; }
extern "C" BOOL SetWindowTextW(HWND w, LPCWSTR t) { return SetWindowTextA(w, Narrow(t).c_str()); }
extern "C" int GetWindowTextA(HWND w, LPSTR text, int size)
{
    if (!text || size <= 0) return 0;
    std::lock_guard lk(g_winMutex);
    WindowInfo* i = Info(w);
    const std::string t = i ? i->title : "";
    strncpy_s(text, size_t(size), t.c_str(), size_t(size - 1));
    return int(std::strlen(text));
}
extern "C" int GetWindowTextW(HWND w, LPWSTR text, int size)
{
    char b[512];
    GetWindowTextA(w, b, sizeof(b));
    const std::wstring s = Widen(b);
    if (!text || size <= 0) return 0;
    wcsncpy_s(text, size_t(size), s.c_str(), size_t(size - 1));
    return int(std::wcslen(text));
}
extern "C" int GetClassNameA(HWND w, LPSTR name, int size)
{
    if (!name || size <= 0) return 0;
    std::lock_guard lk(g_winMutex);
    WindowInfo* i = Info(w);
    strncpy_s(name, size_t(size), i ? i->className.c_str() : "", size_t(size - 1));
    return int(std::strlen(name));
}
extern "C" int GetClassNameW(HWND w, LPWSTR name, int size)
{
    char b[256];
    GetClassNameA(w, b, sizeof(b));
    const std::wstring s = Widen(b);
    if (!name || size <= 0) return 0;
    wcsncpy_s(name, size_t(size), s.c_str(), size_t(size - 1));
    return int(std::wcslen(name));
}
extern "C" HWND GetWindow(HWND, UINT) { return nullptr; }
extern "C" BOOL GetWindowPlacement(HWND w, WINDOWPLACEMENT* p) { if (!p) return FALSE; std::memset(p, 0, sizeof(*p)); p->length = sizeof(*p); p->showCmd = SW_SHOWMAXIMIZED; GetClientRect(w, &p->rcNormalPosition); return TRUE; }
extern "C" BOOL SetWindowPlacement(HWND, const WINDOWPLACEMENT*) { return TRUE; }

// ======================================================================================
// Messages
// ======================================================================================
static BOOL PeekImpl(LPMSG msg, HWND w, UINT first, UINT last, UINT remove)
{
    Pump();
    std::lock_guard lk(g_winMutex);
    auto& q = Posted();
    for (auto it = q.begin(); it != q.end(); ++it)
    {
        if (w && it->hwnd != w) continue;
        if ((first || last) && (it->message < first || it->message > last)) continue;
        *msg = *it;
        if (remove & PM_REMOVE) q.erase(it);
        return TRUE;
    }
    return FALSE;
}
extern "C" BOOL PeekMessageA(LPMSG msg, HWND w, UINT first, UINT last, UINT remove) { return PeekImpl(msg, w, first, last, remove); }
extern "C" BOOL PeekMessageW(LPMSG msg, HWND w, UINT first, UINT last, UINT remove) { return PeekImpl(msg, w, first, last, remove); }
extern "C" BOOL GetMessageA(LPMSG msg, HWND w, UINT first, UINT last)
{
    while (!PeekImpl(msg, w, first, last, PM_REMOVE)) Ttt2PumpSdlEvents(10);
    return msg->message != WM_QUIT;
}
extern "C" BOOL GetMessageW(LPMSG msg, HWND w, UINT first, UINT last) { return GetMessageA(msg, w, first, last); }
extern "C" BOOL TranslateMessage(const MSG*) { return TRUE; }   // WM_CHAR comes from SDL_TEXTINPUT already
extern "C" LRESULT DispatchMessageA(const MSG* msg)
{
    if (!msg || !msg->hwnd) return 0;
    WNDPROC proc = nullptr;
    {
        std::lock_guard lk(g_winMutex);
        if (WindowInfo* i = Info(msg->hwnd)) proc = i->proc;
    }
    return proc ? proc(msg->hwnd, msg->message, msg->wParam, msg->lParam) : DefWindowProcA(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}
extern "C" LRESULT DispatchMessageW(const MSG* msg) { return DispatchMessageA(msg); }
extern "C" LRESULT DefWindowProcA(HWND w, UINT msg, WPARAM, LPARAM)
{
    if (msg == WM_CLOSE) { DestroyWindow(w); return 0; }
    return 0;
}
extern "C" LRESULT DefWindowProcW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, msg, wp, lp); }
extern "C" LRESULT CallWindowProcA(WNDPROC fn, HWND w, UINT msg, WPARAM wp, LPARAM lp) { return fn ? fn(w, msg, wp, lp) : 0; }
extern "C" LRESULT CallWindowProcW(WNDPROC fn, HWND w, UINT msg, WPARAM wp, LPARAM lp) { return fn ? fn(w, msg, wp, lp) : 0; }
extern "C" BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    MSG m{}; m.hwnd = w; m.message = msg; m.wParam = wp; m.lParam = lp; m.time = SDL_GetTicks();
    std::lock_guard lk(g_winMutex);
    Posted().push_back(m);
    return TRUE;
}
extern "C" BOOL PostMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return PostMessageA(w, msg, wp, lp); }
extern "C" LRESULT SendMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC proc = nullptr;
    {
        std::lock_guard lk(g_winMutex);
        if (WindowInfo* i = Info(w)) proc = i->proc;
    }
    return proc ? proc(w, msg, wp, lp) : 0;
}
extern "C" LRESULT SendMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return SendMessageA(w, msg, wp, lp); }
extern "C" BOOL PostThreadMessageA(DWORD, UINT msg, WPARAM wp, LPARAM lp) { return PostMessageA(nullptr, msg, wp, lp); }
extern "C" BOOL PostThreadMessageW(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { return PostThreadMessageA(tid, msg, wp, lp); }
extern "C" void PostQuitMessage(int code) { PostMessageA(nullptr, WM_QUIT, WPARAM(code), 0); }
extern "C" HWND GetForegroundWindow(void) { return reinterpret_cast<HWND>(g_focus.load()); }
extern "C" BOOL SetForegroundWindow(HWND w) { if (w) { SDL_RaiseWindow(reinterpret_cast<SDL_Window*>(w)); g_focus = reinterpret_cast<SDL_Window*>(w); } return TRUE; }
extern "C" HWND GetActiveWindow(void) { return GetForegroundWindow(); }
extern "C" HWND SetActiveWindow(HWND w) { HWND old = GetForegroundWindow(); SetForegroundWindow(w); return old; }
extern "C" HWND GetFocus(void) { return GetForegroundWindow(); }
extern "C" HWND SetFocus(HWND w) { return SetActiveWindow(w); }
extern "C" HWND GetDesktopWindow(void) { return nullptr; }
extern "C" HWND GetShellWindow(void) { return nullptr; }
extern "C" HWND WindowFromPoint(POINT) { return GetForegroundWindow(); }
extern "C" HWND FindWindowA(LPCSTR, LPCSTR) { return nullptr; }
extern "C" HWND FindWindowW(LPCWSTR, LPCWSTR) { return nullptr; }
extern "C" DWORD GetWindowThreadProcessId(HWND, LPDWORD pid) { if (pid) *pid = GetCurrentProcessId(); return GetCurrentThreadId(); }
extern "C" HWND GetParent(HWND) { return nullptr; }
extern "C" HWND GetAncestor(HWND w, UINT) { return w; }
extern "C" BOOL FlashWindow(HWND, BOOL) { return TRUE; }
extern "C" BOOL FlashWindowEx(FLASHWINFO*) { return TRUE; }
extern "C" BOOL BringWindowToTop(HWND w) { return SetForegroundWindow(w); }
extern "C" BOOL LockSetForegroundWindow(UINT) { return TRUE; }
extern "C" BOOL AllowSetForegroundWindow(DWORD) { return TRUE; }
extern "C" BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp)
{
    std::vector<HWND> list;
    { std::lock_guard lk(g_winMutex); for (auto& kv : Windows()) list.push_back(reinterpret_cast<HWND>(kv.first)); }
    for (HWND w : list) if (!fn(w, lp)) break;
    return TRUE;
}
extern "C" BOOL EnumThreadWindows(DWORD, WNDENUMPROC fn, LPARAM lp) { return EnumWindows(fn, lp); }
extern "C" UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, TIMERPROC) { return id ? id : 1; }
extern "C" BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }
extern "C" HMENU GetSystemMenu(HWND, BOOL) { return nullptr; }
extern "C" BOOL EnableMenuItem(HMENU, UINT, UINT) { return TRUE; }

// ---- painting (the software presenter's GDI path is not used on Android) ----
extern "C" BOOL InvalidateRect(HWND, const RECT*, BOOL) { return TRUE; }
extern "C" BOOL ValidateRect(HWND, const RECT*) { return TRUE; }
extern "C" HDC BeginPaint(HWND w, LPPAINTSTRUCT ps) { if (ps) std::memset(ps, 0, sizeof(*ps)); return reinterpret_cast<HDC>(w); }
extern "C" BOOL EndPaint(HWND, const PAINTSTRUCT*) { return TRUE; }
extern "C" HDC GetDC(HWND w) { return reinterpret_cast<HDC>(w ? w : reinterpret_cast<HWND>(uintptr_t(1))); }
extern "C" int ReleaseDC(HWND, HDC) { return 1; }
extern "C" int FillRect(HDC, const RECT*, HBRUSH) { return 1; }
extern "C" HGDIOBJ GetStockObject(int) { return nullptr; }
extern "C" BOOL DeleteObject(HGDIOBJ) { return TRUE; }
extern "C" HBRUSH CreateSolidBrush(COLORREF) { return nullptr; }
extern "C" int StretchDIBits(HDC, int, int, int, int, int, int, int, int hs, const void*, const BITMAPINFO*, UINT, DWORD) { return hs; }
extern "C" int SetDIBitsToDevice(HDC, int, int, DWORD, DWORD h, int, int, UINT, UINT, const void*, const BITMAPINFO*, UINT) { return int(h); }

// ---- rectangles ----
extern "C" BOOL IntersectRect(LPRECT d, const RECT* a, const RECT* b)
{
    RECT r{ std::max(a->left, b->left), std::max(a->top, b->top), std::min(a->right, b->right), std::min(a->bottom, b->bottom) };
    if (r.left >= r.right || r.top >= r.bottom) { *d = RECT{ 0, 0, 0, 0 }; return FALSE; }
    *d = r; return TRUE;
}
extern "C" BOOL UnionRect(LPRECT d, const RECT* a, const RECT* b)
{
    *d = RECT{ std::min(a->left, b->left), std::min(a->top, b->top), std::max(a->right, b->right), std::max(a->bottom, b->bottom) };
    return TRUE;
}
extern "C" BOOL PtInRect(const RECT* r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
extern "C" BOOL SetRect(LPRECT r, int l, int t, int ri, int b) { *r = RECT{ l, t, ri, b }; return TRUE; }
extern "C" BOOL IsRectEmpty(const RECT* r) { return r->right <= r->left || r->bottom <= r->top; }
extern "C" BOOL OffsetRect(LPRECT r, int dx, int dy) { r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return TRUE; }
extern "C" BOOL EqualRect(const RECT* a, const RECT* b) { return std::memcmp(a, b, sizeof(RECT)) == 0; }

// ---- cursor, mouse ----
extern "C" HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HCURSOR>(uintptr_t(1)); }
extern "C" HCURSOR LoadCursorW(HINSTANCE, LPCWSTR) { return reinterpret_cast<HCURSOR>(uintptr_t(1)); }
extern "C" HICON LoadIconA(HINSTANCE, LPCSTR) { return nullptr; }
extern "C" HICON LoadIconW(HINSTANCE, LPCWSTR) { return nullptr; }
extern "C" HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT) { return nullptr; }
extern "C" HANDLE LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return nullptr; }
extern "C" HCURSOR SetCursor(HCURSOR c) { return c; }
extern "C" int ShowCursor(BOOL show) { return show ? 0 : -1; }
extern "C" BOOL GetCursorPos(LPPOINT p) { if (!p) return FALSE; int x = 0, y = 0; SDL_GetMouseState(&x, &y); p->x = x; p->y = y; return TRUE; }
extern "C" BOOL SetCursorPos(int, int) { return TRUE; }
extern "C" BOOL ClipCursor(const RECT*) { return TRUE; }
extern "C" HWND SetCapture(HWND w) { return w; }
extern "C" BOOL ReleaseCapture(void) { return TRUE; }
extern "C" BOOL TrackMouseEvent(TRACKMOUSEEVENT*) { return TRUE; }
extern "C" BOOL RegisterRawInputDevices(const RAWINPUTDEVICE*, UINT, UINT) { return TRUE; }

// ---- keyboard ----
extern "C" SHORT GetAsyncKeyState(int vk)
{
    if (vk == VK_LBUTTON) return (g_mouseButtons & 1) ? SHORT(0x8000) : 0;
    if (vk == VK_RBUTTON) return (g_mouseButtons & 2) ? SHORT(0x8000) : 0;
    if (vk == VK_MBUTTON) return (g_mouseButtons & 4) ? SHORT(0x8000) : 0;
    if (!EnsureSdl()) return 0;
    SDL_PumpEvents();
    int n = 0;
    const Uint8* keys = SDL_GetKeyboardState(&n);
    auto down = [&](int v) { const SDL_Scancode s = ScancodeFromVk(v); return s != SDL_SCANCODE_UNKNOWN && int(s) < n && keys[s]; };
    bool pressed;
    if (vk == VK_SHIFT) pressed = down(VK_LSHIFT) || down(VK_RSHIFT);
    else if (vk == VK_CONTROL) pressed = down(VK_LCONTROL) || down(VK_RCONTROL);
    else if (vk == VK_MENU) pressed = down(VK_LMENU) || down(VK_RMENU);
    else if (vk == VK_ESCAPE) pressed = down(VK_ESCAPE) || (SDL_SCANCODE_AC_BACK < n && keys[SDL_SCANCODE_AC_BACK]);
    else pressed = down(vk);
    return pressed ? SHORT(0x8000) : 0;
}
extern "C" SHORT GetKeyState(int vk) { return GetAsyncKeyState(vk); }
extern "C" BOOL GetKeyboardState(PBYTE keys) { if (!keys) return FALSE; for (int i = 0; i < 256; ++i) keys[i] = (GetAsyncKeyState(i) & 0x8000) ? 0x80 : 0; return TRUE; }
extern "C" UINT MapVirtualKeyA(UINT code, UINT type)
{
    switch (type)
    {
    case MAPVK_VK_TO_VSC: return UINT(ScancodeFromVk(int(code)));
    case MAPVK_VSC_TO_VK: return UINT(GenericVk(VkFromScancode(SDL_Scancode(code))));
    case MAPVK_VSC_TO_VK_EX: return UINT(VkFromScancode(SDL_Scancode(code)));
    case MAPVK_VK_TO_CHAR: return (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') ? code : 0;
    default: return 0;
    }
}
extern "C" UINT MapVirtualKeyW(UINT code, UINT type) { return MapVirtualKeyA(code, type); }
extern "C" int GetKeyNameTextA(LONG lp, LPSTR out, int size)
{
    const SDL_Scancode s = SDL_Scancode((lp >> 16) & 0xFF);
    const char* n = SDL_GetScancodeName(s);
    if (!out || size <= 0) return 0;
    strncpy_s(out, size_t(size), n ? n : "", size_t(size - 1));
    return int(std::strlen(out));
}
extern "C" int GetKeyNameTextW(LONG lp, LPWSTR out, int size)
{
    char b[64];
    GetKeyNameTextA(lp, b, sizeof(b));
    const std::wstring w = Widen(b);
    if (!out || size <= 0) return 0;
    wcsncpy_s(out, size_t(size), w.c_str(), size_t(size - 1));
    return int(std::wcslen(out));
}
extern "C" int ToUnicode(UINT vk, UINT, const BYTE*, LPWSTR out, int size, UINT)
{
    if (!out || size <= 0) return 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == ' ') { out[0] = wchar_t(vk >= 'A' && vk <= 'Z' ? vk + 32 : vk); return 1; }
    return 0;
}

// ---- monitors / display ----
static int DisplayOf(HMONITOR m) { const int i = int(reinterpret_cast<intptr_t>(m)) - 1; return i < 0 ? 0 : i; }
extern "C" HMONITOR MonitorFromWindow(HWND w, DWORD)
{
    const int i = w ? SDL_GetWindowDisplayIndex(reinterpret_cast<SDL_Window*>(w)) : 0;
    return reinterpret_cast<HMONITOR>(intptr_t((i < 0 ? 0 : i) + 1));
}
extern "C" HMONITOR MonitorFromPoint(POINT, DWORD) { return reinterpret_cast<HMONITOR>(intptr_t(1)); }
extern "C" HMONITOR MonitorFromRect(LPCRECT, DWORD) { return reinterpret_cast<HMONITOR>(intptr_t(1)); }
extern "C" BOOL GetMonitorInfoA(HMONITOR m, LPMONITORINFO info)
{
    if (!info) return FALSE;
    EnsureSdl();
    SDL_DisplayMode dm{};
    SDL_GetDesktopDisplayMode(DisplayOf(m), &dm);
    const int w = std::max(dm.w, dm.h), h = std::min(dm.w, dm.h);   // landscape
    info->rcMonitor = RECT{ 0, 0, w, h };
    info->rcWork = info->rcMonitor;
    info->dwFlags = MONITORINFOF_PRIMARY;
    if (info->cbSize >= sizeof(MONITORINFOEXA))
        std::strcpy(reinterpret_cast<MONITORINFOEXA*>(info)->szDevice, "\\\\.\\DISPLAY1");
    return TRUE;
}
extern "C" BOOL GetMonitorInfoW(HMONITOR m, LPMONITORINFO info)
{
    const DWORD cb = info ? info->cbSize : 0;
    if (cb >= sizeof(MONITORINFOEXW)) info->cbSize = sizeof(MONITORINFO);
    const BOOL ok = GetMonitorInfoA(m, info);
    if (info) info->cbSize = cb;
    if (ok && cb >= sizeof(MONITORINFOEXW)) std::wcscpy(reinterpret_cast<MONITORINFOEXW*>(info)->szDevice, L"\\\\.\\DISPLAY1");
    return ok;
}
extern "C" BOOL EnumDisplayMonitors(HDC, LPCRECT, MONITORENUMPROC fn, LPARAM data)
{
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoA(reinterpret_cast<HMONITOR>(intptr_t(1)), &mi);
    if (fn) fn(reinterpret_cast<HMONITOR>(intptr_t(1)), nullptr, &mi.rcMonitor, data);
    return TRUE;
}
template <typename DM> static BOOL FillDevMode(DM* dm)
{
    if (!dm) return FALSE;
    EnsureSdl();
    SDL_DisplayMode m{};
    SDL_GetDesktopDisplayMode(0, &m);
    dm->dmPelsWidth = DWORD(std::max(m.w, m.h)); dm->dmPelsHeight = DWORD(std::min(m.w, m.h));
    dm->dmDisplayFrequency = DWORD(m.refresh_rate > 0 ? m.refresh_rate : 60);
    dm->dmBitsPerPel = 32;
    dm->dmPosition = POINT{ 0, 0 };
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    return TRUE;
}
extern "C" BOOL EnumDisplaySettingsW(LPCWSTR, DWORD mode, DEVMODEW* dm) { return (mode == ENUM_CURRENT_SETTINGS || mode == 0) ? FillDevMode(dm) : FALSE; }
extern "C" BOOL EnumDisplaySettingsA(LPCSTR, DWORD mode, DEVMODEA* dm) { return (mode == ENUM_CURRENT_SETTINGS || mode == 0) ? FillDevMode(dm) : FALSE; }
extern "C" BOOL EnumDisplayDevicesA(LPCSTR device, DWORD index, DISPLAY_DEVICEA* dd, DWORD)
{
    if (device || index > 0 || !dd) return FALSE;
    std::strcpy(dd->DeviceName, "\\\\.\\DISPLAY1");
    std::strcpy(dd->DeviceString, "Android display");
    dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}
extern "C" BOOL EnumDisplayDevicesW(LPCWSTR device, DWORD index, DISPLAY_DEVICEW* dd, DWORD)
{
    if (device || index > 0 || !dd) return FALSE;
    std::wcscpy(dd->DeviceName, L"\\\\.\\DISPLAY1");
    std::wcscpy(dd->DeviceString, L"Android display");
    dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}
extern "C" LONG ChangeDisplaySettingsW(DEVMODEW*, DWORD) { return DISP_CHANGE_SUCCESSFUL; }
extern "C" LONG ChangeDisplaySettingsExW(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID) { return DISP_CHANGE_SUCCESSFUL; }
extern "C" int GetSystemMetrics(int index)
{
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoA(reinterpret_cast<HMONITOR>(intptr_t(1)), &mi);
    switch (index)
    {
    case SM_CXSCREEN: case SM_CXVIRTUALSCREEN: return mi.rcMonitor.right;
    case SM_CYSCREEN: case SM_CYVIRTUALSCREEN: return mi.rcMonitor.bottom;
    case SM_CMONITORS: return 1;
    default: return 0;
    }
}
extern "C" BOOL SystemParametersInfoW(UINT action, UINT, PVOID out, UINT)
{
    if (action == SPI_GETWORKAREA && out) { MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoA(reinterpret_cast<HMONITOR>(intptr_t(1)), &mi); *static_cast<RECT*>(out) = mi.rcWork; return TRUE; }
    return FALSE;
}
extern "C" BOOL SystemParametersInfoA(UINT action, UINT p, PVOID out, UINT w) { return SystemParametersInfoW(action, p, out, w); }
extern "C" UINT GetDpiForWindow(HWND) { return 96; }
extern "C" UINT GetDpiForSystem(void) { return 96; }
extern "C" BOOL SetProcessDPIAware(void) { return TRUE; }
extern "C" BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT) { return TRUE; }
extern "C" HRESULT SetProcessDpiAwareness(int) { return S_OK; }
extern "C" HRESULT DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD) { return S_OK; }
extern "C" HRESULT DwmGetWindowAttribute(HWND w, DWORD attr, PVOID value, DWORD size)
{
    if (attr == DWMWA_EXTENDED_FRAME_BOUNDS && value && size >= sizeof(RECT)) { GetClientRect(w, static_cast<RECT*>(value)); return S_OK; }
    if (attr == DWMWA_CLOAKED && value && size >= sizeof(DWORD)) { *static_cast<DWORD*>(value) = 0; return S_OK; }
    return E_INVALIDARG;
}
extern "C" HRESULT DwmFlush(void) { return S_OK; }
extern "C" HRESULT DwmIsCompositionEnabled(BOOL* e) { if (e) *e = TRUE; return S_OK; }
extern "C" HRESULT DwmGetCompositionTimingInfo(HWND, DWM_TIMING_INFO*) { return E_NOTIMPL; }

// ---- message boxes ----
extern "C" int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    std::fprintf(stderr, "[messagebox] %s: %s\n", caption ? caption : "", text ? text : "");
    if (EnsureSdl())
    {
        const Uint32 flags = (type & 0xF0) == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR : (type & 0xF0) == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
        const bool yesNo = (type & 0x0F) == MB_YESNO || (type & 0x0F) == MB_YESNOCANCEL;
        const bool okCancel = (type & 0x0F) == MB_OKCANCEL;
        SDL_MessageBoxButtonData buttons[2];
        int n = 0;
        if (yesNo) { buttons[n++] = { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDYES, "Yes" }; buttons[n++] = { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDNO, "No" }; }
        else if (okCancel) { buttons[n++] = { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDOK, "OK" }; buttons[n++] = { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDCANCEL, "Cancel" }; }
        else buttons[n++] = { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDOK, "OK" };
        SDL_MessageBoxData data{ flags, reinterpret_cast<SDL_Window*>(w), caption ? caption : "", text ? text : "", n, buttons, nullptr };
        int id = IDOK;
        if (SDL_ShowMessageBox(&data, &id) == 0 && id >= 0) return id;
    }
    return (type & 0x0F) == MB_YESNO ? IDNO : IDOK;
}
extern "C" int MessageBoxW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type) { return MessageBoxA(w, Narrow(text).c_str(), Narrow(caption).c_str(), type); }
extern "C" BOOL MessageBeep(UINT) { return TRUE; }
extern "C" BOOL Beep(DWORD, DWORD) { return TRUE; }

// ---- clipboard ----
static std::string g_clip;
extern "C" BOOL OpenClipboard(HWND) { return EnsureSdl(); }
extern "C" BOOL CloseClipboard(void) { return TRUE; }
extern "C" BOOL EmptyClipboard(void) { SDL_SetClipboardText(""); return TRUE; }
extern "C" HANDLE SetClipboardData(UINT fmt, HANDLE mem)
{
    if (fmt == CF_UNICODETEXT) SDL_SetClipboardText(Narrow(static_cast<const wchar_t*>(mem)).c_str());
    else if (fmt == CF_TEXT) SDL_SetClipboardText(static_cast<const char*>(mem));
    return mem;
}
extern "C" HANDLE GetClipboardData(UINT fmt)
{
    static std::wstring wide;
    char* t = SDL_GetClipboardText();
    g_clip = t ? t : "";
    SDL_free(t);
    if (fmt == CF_UNICODETEXT) { wide = Widen(g_clip.c_str()); return wide.data(); }
    return g_clip.data();
}

// ======================================================================================
// XInput
// ======================================================================================
static SHORT Axis(Sint16 v) { return v == -32768 ? SHORT(-32768) : SHORT(v); }
extern "C" DWORD XInputGetState(DWORD user, XINPUT_STATE* state)
{
    if (!state || user >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    std::memset(state, 0, sizeof(*state));
    bool connected = false;
    {
        std::lock_guard lk(g_padMutex);
        SDL_GameController* p = g_pads[user];
        if (p && SDL_GameControllerGetAttached(p))
        {
            connected = true;
            SDL_GameControllerUpdate();
            WORD b = 0;
            auto btn = [&](SDL_GameControllerButton s, WORD x) { if (SDL_GameControllerGetButton(p, s)) b |= x; };
            btn(SDL_CONTROLLER_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP); btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN);
            btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT); btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
            btn(SDL_CONTROLLER_BUTTON_START, XINPUT_GAMEPAD_START); btn(SDL_CONTROLLER_BUTTON_BACK, XINPUT_GAMEPAD_BACK);
            btn(SDL_CONTROLLER_BUTTON_LEFTSTICK, XINPUT_GAMEPAD_LEFT_THUMB); btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK, XINPUT_GAMEPAD_RIGHT_THUMB);
            btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER); btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER);
            btn(SDL_CONTROLLER_BUTTON_GUIDE, XINPUT_GAMEPAD_GUIDE);
            btn(SDL_CONTROLLER_BUTTON_A, XINPUT_GAMEPAD_A); btn(SDL_CONTROLLER_BUTTON_B, XINPUT_GAMEPAD_B);
            btn(SDL_CONTROLLER_BUTTON_X, XINPUT_GAMEPAD_X); btn(SDL_CONTROLLER_BUTTON_Y, XINPUT_GAMEPAD_Y);
            state->Gamepad.wButtons = b;
            state->Gamepad.bLeftTrigger = BYTE(std::clamp(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128, 0, 255));
            state->Gamepad.bRightTrigger = BYTE(std::clamp(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128, 0, 255));
            state->Gamepad.sThumbLX = Axis(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX));
            state->Gamepad.sThumbLY = SHORT(-std::max<int>(-32767, SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY)));
            state->Gamepad.sThumbRX = Axis(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX));
            state->Gamepad.sThumbRY = SHORT(-std::max<int>(-32767, SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTY)));
        }
    }
    if (user == 0 && g_touch.active)
    {
        connected = true;
        state->Gamepad.wButtons |= WORD(g_touch.buttons.load());
        auto merge = [](SHORT a, int32_t b) { return std::abs(int(b)) > std::abs(int(a)) ? SHORT(std::clamp<int32_t>(b, -32768, 32767)) : a; };
        state->Gamepad.sThumbLX = merge(state->Gamepad.sThumbLX, g_touch.lx);
        state->Gamepad.sThumbLY = merge(state->Gamepad.sThumbLY, g_touch.ly);
        state->Gamepad.sThumbRX = merge(state->Gamepad.sThumbRX, g_touch.rx);
        state->Gamepad.sThumbRY = merge(state->Gamepad.sThumbRY, g_touch.ry);
        state->Gamepad.bLeftTrigger = BYTE(std::max<uint32_t>(state->Gamepad.bLeftTrigger, std::min<uint32_t>(g_touch.lt, 255)));
        state->Gamepad.bRightTrigger = BYTE(std::max<uint32_t>(state->Gamepad.bRightTrigger, std::min<uint32_t>(g_touch.rt, 255)));
    }
    if (!connected) return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = g_packet.fetch_add(1) + 1;
    return ERROR_SUCCESS;
}
extern "C" DWORD XInputSetState(DWORD user, XINPUT_VIBRATION* v)
{
    if (!v || user >= XUSER_MAX_COUNT) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lk(g_padMutex);
    SDL_GameController* p = g_pads[user];
    if (!p) return user == 0 && g_touch.active ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    SDL_GameControllerRumble(p, v->wLeftMotorSpeed, v->wRightMotorSpeed, 200);
    return ERROR_SUCCESS;
}
extern "C" DWORD XInputGetCapabilities(DWORD user, DWORD, XINPUT_CAPABILITIES* caps)
{
    if (!caps) return ERROR_INVALID_PARAMETER;
    XINPUT_STATE s;
    if (XInputGetState(user, &s) != ERROR_SUCCESS) return ERROR_DEVICE_NOT_CONNECTED;
    std::memset(caps, 0, sizeof(*caps));
    caps->Type = XINPUT_DEVTYPE_GAMEPAD; caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF; caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = SHORT(-64);
    caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 255;
    return ERROR_SUCCESS;
}
extern "C" DWORD XInputGetBatteryInformation(DWORD user, BYTE, XINPUT_BATTERY_INFORMATION* info)
{
    if (!info) return ERROR_INVALID_PARAMETER;
    XINPUT_STATE s;
    const bool c = XInputGetState(user, &s) == ERROR_SUCCESS;
    info->BatteryType = c ? BATTERY_TYPE_WIRED : BATTERY_TYPE_DISCONNECTED;
    info->BatteryLevel = BATTERY_LEVEL_FULL;
    return c ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD XInputGetKeystroke(DWORD, DWORD, PXINPUT_KEYSTROKE) { return ERROR_EMPTY; }
extern "C" void XInputEnable(BOOL) {}
