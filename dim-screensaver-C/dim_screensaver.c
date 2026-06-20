#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define TIMER_ID 1
#define FADE_DURATION_MS 10000.0
#define EXIT_FADE_DURATION_MS 1000.0
#define FINAL_DARKNESS 0.94
#define TRANSPARENCY_STEPS 16
#define WAKE_MOVE_PIXELS 6

typedef enum AppMode {
    MODE_SAVER,
    MODE_PREVIEW
} AppMode;

typedef struct Command {
    AppMode mode;
    BOOL configure;
    HWND preview_parent;
} Command;

typedef struct AppState {
    HINSTANCE instance;
    AppMode mode;
    HWND hwnd;
    HWND preview_parent;
    int virtual_x;
    int virtual_y;
    int virtual_width;
    int virtual_height;
    ULONGLONG start_tick;
    ULONGLONG dismiss_start_tick;
    BYTE dismiss_start_alpha;
    int applied_alpha;
    POINT last_mouse;
    BOOL ignore_initial_mouse_move;
    BOOL dismissing;
    BOOL cursor_hidden;
} AppState;

typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT)(HANDLE);
typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARE)(void);

static const wchar_t SaverClassName[] = L"DimScreensaverCWindow";
static const wchar_t PreviewClassName[] = L"DimScreensaverCPreviewWindow";

static void EnableDpiAwareness(void);
static Command ParseCommandLine(void);
static HWND ParseWindowHandle(const wchar_t *text);
static int RunSaver(HINSTANCE instance);
static int RunPreview(HINSTANCE instance, HWND parent);
static BOOL RegisterWindowClasses(HINSTANCE instance);
static LRESULT CALLBACK SaverWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static void GetVirtualDesktopBounds(AppState *state);
static BYTE CurrentFadeAlpha(const AppState *state);
static BYTE QuantizedAlpha(int max_alpha, double fraction);
static double EaseInOutCubic(double value);
static void SetSaverOpacity(AppState *state);
static void PaintSaver(HWND hwnd, AppState *state);
static void PaintPreview(HWND hwnd, AppState *state);
static void DismissSaver(HWND hwnd);
static void HideCursor(AppState *state);
static void ShowCursorIfHidden(AppState *state);
static void ShowConfigurationDialog(HWND owner);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance, PWSTR command_line, int show_command)
{
    Command command;

    (void)previous_instance;
    (void)command_line;
    (void)show_command;

    EnableDpiAwareness();

    command = ParseCommandLine();
    if (command.configure) {
        ShowConfigurationDialog(NULL);
        return 0;
    }

    if (!RegisterWindowClasses(instance)) {
        MessageBoxW(NULL, L"Could not register window classes.", L"Dim Screensaver C", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (command.mode == MODE_PREVIEW) {
        return RunPreview(instance, command.preview_parent);
    }

    return RunSaver(instance);
}

static void EnableDpiAwareness(void)
{
    HMODULE user32;
    PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT set_context;
    PFN_SET_PROCESS_DPI_AWARE set_dpi_aware;

    user32 = LoadLibraryW(L"user32.dll");
    if (user32 == NULL) {
        return;
    }

    set_context = (PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (set_context != NULL && set_context((HANDLE)(LONG_PTR)-4)) {
        FreeLibrary(user32);
        return;
    }

    set_dpi_aware = (PFN_SET_PROCESS_DPI_AWARE)GetProcAddress(user32, "SetProcessDPIAware");
    if (set_dpi_aware != NULL) {
        set_dpi_aware();
    }

    FreeLibrary(user32);
}

static Command ParseCommandLine(void)
{
    Command command;
    int argc;
    LPWSTR *argv;

    command.mode = MODE_SAVER;
    command.configure = FALSE;
    command.preview_parent = NULL;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL || argc <= 1) {
        command.configure = TRUE;
        if (argv != NULL) {
            LocalFree(argv);
        }
        return command;
    }

    if ((argv[1][0] == L'/' || argv[1][0] == L'-') && argv[1][1] != L'\0') {
        wchar_t option = (wchar_t)towlower(argv[1][1]);

        if (option == L'p') {
            const wchar_t *handle_text = argc > 2 ? argv[2] : argv[1] + 2;
            if (*handle_text == L':') {
                handle_text++;
            }

            command.mode = MODE_PREVIEW;
            command.preview_parent = ParseWindowHandle(handle_text);
        } else if (option == L'c') {
            command.configure = TRUE;
        } else {
            command.mode = MODE_SAVER;
        }
    }

    LocalFree(argv);
    return command;
}

static HWND ParseWindowHandle(const wchar_t *text)
{
    wchar_t *end = NULL;
    unsigned long long value;

    if (text == NULL || *text == L'\0') {
        return NULL;
    }

    value = wcstoull(text, &end, 10);
    if (end == text) {
        return NULL;
    }

    return (HWND)(ULONG_PTR)value;
}

static int RunSaver(HINSTANCE instance)
{
    AppState state;
    HWND hwnd;
    MSG message;

    ZeroMemory(&state, sizeof(state));
    state.instance = instance;
    state.mode = MODE_SAVER;
    state.start_tick = GetTickCount64();
    state.applied_alpha = -1;
    state.ignore_initial_mouse_move = TRUE;
    GetCursorPos(&state.last_mouse);
    GetVirtualDesktopBounds(&state);

    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        SaverClassName,
        L"Dim Screensaver C",
        WS_POPUP,
        state.virtual_x,
        state.virtual_y,
        state.virtual_width,
        state.virtual_height,
        NULL,
        NULL,
        instance,
        &state);

    if (hwnd == NULL) {
        MessageBoxW(NULL, L"Could not create the screensaver window.", L"Dim Screensaver C", MB_OK | MB_ICONERROR);
        return 1;
    }

    state.hwnd = hwnd;
    SetSaverOpacity(&state);
    HideCursor(&state);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ShowCursorIfHidden(&state);
    return (int)message.wParam;
}

static int RunPreview(HINSTANCE instance, HWND parent)
{
    AppState state;
    RECT parent_rect;
    HWND hwnd;
    MSG message;

    ZeroMemory(&state, sizeof(state));
    state.instance = instance;
    state.mode = MODE_PREVIEW;
    state.preview_parent = parent;
    state.start_tick = GetTickCount64();

    if (parent != NULL && GetClientRect(parent, &parent_rect)) {
        state.virtual_width = parent_rect.right - parent_rect.left;
        state.virtual_height = parent_rect.bottom - parent_rect.top;
    } else {
        state.virtual_width = 240;
        state.virtual_height = 160;
    }

    hwnd = CreateWindowExW(
        0,
        PreviewClassName,
        L"Dim Screensaver C Preview",
        parent != NULL ? WS_CHILD | WS_VISIBLE : WS_POPUP | WS_VISIBLE,
        0,
        0,
        state.virtual_width,
        state.virtual_height,
        parent,
        NULL,
        instance,
        &state);

    if (hwnd == NULL) {
        return 1;
    }

    state.hwnd = hwnd;

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return (int)message.wParam;
}

static BOOL RegisterWindowClasses(HINSTANCE instance)
{
    WNDCLASSW window_class;

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.lpfnWndProc = SaverWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = SaverClassName;
    window_class.hCursor = NULL;
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    if (!RegisterClassW(&window_class)) {
        return FALSE;
    }

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.lpfnWndProc = SaverWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = PreviewClassName;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = NULL;

    return RegisterClassW(&window_class) != 0;
}

static LRESULT CALLBACK SaverWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    AppState *state;

    state = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_NCCREATE:
    {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }

    case WM_CREATE:
        state = (AppState *)((CREATESTRUCTW *)lparam)->lpCreateParams;
        SetTimer(hwnd, TIMER_ID, state->mode == MODE_PREVIEW ? 33 : 16, NULL);
        return 0;

    case WM_TIMER:
        if (state != NULL && state->mode == MODE_SAVER) {
            SetSaverOpacity(state);
        } else {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        if (state != NULL && state->mode == MODE_SAVER) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        if (state != NULL && state->mode == MODE_SAVER) {
            POINT current_mouse;
            BOOL moved_far_enough;

            GetCursorPos(&current_mouse);
            moved_far_enough =
                abs(current_mouse.x - state->last_mouse.x) > WAKE_MOVE_PIXELS ||
                abs(current_mouse.y - state->last_mouse.y) > WAKE_MOVE_PIXELS;
            state->last_mouse = current_mouse;

            if (state->ignore_initial_mouse_move) {
                state->ignore_initial_mouse_move = FALSE;
                return 0;
            }

            if (moved_far_enough) {
                DismissSaver(hwnd);
            }
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (state != NULL && state->mode == MODE_SAVER) {
            DismissSaver(hwnd);
            return 0;
        }
        break;

    case WM_PAINT:
        if (state != NULL && state->mode == MODE_PREVIEW) {
            PaintPreview(hwnd, state);
        } else if (state != NULL) {
            PaintSaver(hwnd, state);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void GetVirtualDesktopBounds(AppState *state)
{
    state->virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    state->virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    state->virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    state->virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (state->virtual_width <= 0 || state->virtual_height <= 0) {
        state->virtual_x = 0;
        state->virtual_y = 0;
        state->virtual_width = GetSystemMetrics(SM_CXSCREEN);
        state->virtual_height = GetSystemMetrics(SM_CYSCREEN);
    }
}

static BYTE CurrentFadeAlpha(const AppState *state)
{
    ULONGLONG elapsed;
    double progress;
    double eased;

    elapsed = GetTickCount64() - (state->dismissing ? state->dismiss_start_tick : state->start_tick);
    progress = (double)elapsed / (state->dismissing ? EXIT_FADE_DURATION_MS : FADE_DURATION_MS);
    if (progress < 0.0) {
        progress = 0.0;
    } else if (progress > 1.0) {
        progress = 1.0;
    }

    eased = EaseInOutCubic(progress);
    if (state->dismissing) {
        return QuantizedAlpha(state->dismiss_start_alpha, 1.0 - eased);
    }

    return QuantizedAlpha((int)(255.0 * FINAL_DARKNESS + 0.5), eased);
}

static BYTE QuantizedAlpha(int max_alpha, double fraction)
{
    int intervals = TRANSPARENCY_STEPS - 1;
    int level;
    int alpha;

    if (max_alpha <= 0 || fraction <= 0.0) {
        return 0;
    }

    if (fraction > 1.0) {
        fraction = 1.0;
    }

    if (intervals < 1) {
        intervals = 1;
    }

    level = (int)(fraction * intervals + 0.5);
    alpha = (max_alpha * level + intervals / 2) / intervals;

    if (alpha > 255) {
        return 255;
    }

    return (BYTE)alpha;
}

static double EaseInOutCubic(double value)
{
    if (value < 0.5) {
        return 4.0 * value * value * value;
    }

    return 1.0 - pow(-2.0 * value + 2.0, 3.0) / 2.0;
}

static void SetSaverOpacity(AppState *state)
{
    if (state->hwnd != NULL) {
        BYTE alpha = CurrentFadeAlpha(state);

        if (state->applied_alpha != (int)alpha) {
            SetLayeredWindowAttributes(state->hwnd, 0, alpha, LWA_ALPHA);
            state->applied_alpha = alpha;
        }

        if (state->dismissing && alpha == 0) {
            DestroyWindow(state->hwnd);
        }
    }
}

static void PaintSaver(HWND hwnd, AppState *state)
{
    PAINTSTRUCT paint;
    HDC hdc;
    RECT client;
    HBRUSH brush;

    (void)state;
    hdc = BeginPaint(hwnd, &paint);
    GetClientRect(hwnd, &client);
    brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &client, brush);
    DeleteObject(brush);

    EndPaint(hwnd, &paint);
}

static void PaintPreview(HWND hwnd, AppState *state)
{
    PAINTSTRUCT paint;
    HDC hdc;
    RECT client;
    ULONGLONG elapsed;
    double phase;
    BYTE alpha;
    int shade;
    HBRUSH base_brush;

    hdc = BeginPaint(hwnd, &paint);
    GetClientRect(hwnd, &client);

    base_brush = CreateSolidBrush(RGB(36, 48, 62));
    FillRect(hdc, &client, base_brush);
    DeleteObject(base_brush);

    elapsed = GetTickCount64() - state->start_tick;
    phase = (double)(elapsed % 3000) / 3000.0;
    alpha = (BYTE)(72.0 + 96.0 * (0.5 + sin(phase * 6.283185307179586) * 0.5));
    shade = (int)(36.0 * (255.0 - alpha) / 255.0);
    base_brush = CreateSolidBrush(RGB(shade, shade + 6, shade + 12));
    FillRect(hdc, &client, base_brush);
    DeleteObject(base_brush);

    EndPaint(hwnd, &paint);
}

static void DismissSaver(HWND hwnd)
{
    AppState *state = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (state == NULL) {
        DestroyWindow(hwnd);
        return;
    }

    if (state->dismissing) {
        return;
    }

    state->dismiss_start_alpha = CurrentFadeAlpha(state);
    state->dismiss_start_tick = GetTickCount64();
    state->dismissing = TRUE;
    SetSaverOpacity(state);
}

static void HideCursor(AppState *state)
{
    while (ShowCursor(FALSE) >= 0) {
    }

    state->cursor_hidden = TRUE;
}

static void ShowCursorIfHidden(AppState *state)
{
    if (!state->cursor_hidden) {
        return;
    }

    while (ShowCursor(TRUE) < 0) {
    }

    state->cursor_hidden = FALSE;
}

static void ShowConfigurationDialog(HWND owner)
{
    MessageBoxW(
        owner,
        L"Dim Screensaver C fades a transparent black fullscreen window to opacity over 10 seconds.",
        L"Dim Screensaver C",
        MB_OK | MB_ICONINFORMATION);
}
