#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define TIMER_ID 1
#define DEFAULT_FADE_IN_DURATION_MS 10000.0
#define DEFAULT_FADE_OUT_DURATION_MS 1000.0
#define DEFAULT_LOCK_WORKSTATION TRUE
#define FINAL_DARKNESS 0.94
/* 67 ms is roughly 15 FPS: smooth enough for this fade, but cheap to run. */
#define OPACITY_TIMER_INTERVAL_MS 67
#define WAKE_MOVE_PIXELS 6
#define SETTINGS_MAX_BYTES 4096
#define MIN_SETTING_SECONDS 1L
#define MAX_SETTING_SECONDS 3600L

typedef enum AppMode {
    MODE_SAVER,
    MODE_PREVIEW
} AppMode;

typedef struct Command {
    AppMode mode;
    BOOL configure;
    HWND preview_parent;
} Command;

typedef struct Settings {
    double fade_in_duration_ms;
    double fade_out_duration_ms;
    BOOL lock_workstation;
} Settings;

typedef struct AppState {
    HINSTANCE instance;
    AppMode mode;
    HWND hwnd;
    HWND preview_parent;
    HBITMAP desktop_bitmap;
    int virtual_x;
    int virtual_y;
    int virtual_width;
    int virtual_height;
    ULONGLONG start_tick;
    ULONGLONG dismiss_start_tick;
    double fade_in_duration_ms;
    double fade_out_duration_ms;
    BYTE dismiss_start_alpha;
    POINT last_mouse;
    BOOL ignore_initial_mouse_move;
    BOOL dismissing;
    BOOL lock_requested;
    BOOL lock_workstation;
    BOOL cursor_hidden;
} AppState;

typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT)(HANDLE);
typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARE)(void);

static const wchar_t SaverClassName[] = L"DimScreensaverWindow";
static const wchar_t PreviewClassName[] = L"DimScreensaverPreviewWindow";

static void EnableDpiAwareness(void);
static Command ParseCommandLine(void);
static HWND ParseWindowHandle(const wchar_t *text);
static int RunSaver(HINSTANCE instance);
static int RunPreview(HINSTANCE instance, HWND parent);
static BOOL RegisterWindowClasses(HINSTANCE instance);
static LRESULT CALLBACK SaverWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static Settings LoadSettings(void);
static BOOL GetSettingsPath(wchar_t *path, DWORD path_count);
static char *TrimAscii(char *text);
static BOOL EqualsIgnoreCaseAscii(const char *left, const char *right);
static BOOL ParseSecondsAscii(char *text, long *seconds);
static BOOL ParseBoolAscii(const char *text, BOOL *value);
static void GetVirtualDesktopBounds(AppState *state);
static BOOL CaptureDesktop(AppState *state);
static BYTE CurrentFadeAlpha(const AppState *state);
static double EaseInOutCubic(double value);
static void UpdateSaverFrame(AppState *state);
static void PaintSaver(HWND hwnd, AppState *state);
static void PaintBlackOverlay(HDC hdc, const RECT *client, BYTE alpha);
static void PaintPreview(HWND hwnd, AppState *state);
static void DismissSaver(HWND hwnd);
static void ReleaseCapturedDesktop(AppState *state);
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
    Settings settings;
    HWND hwnd;
    MSG message;

    settings = LoadSettings();

    ZeroMemory(&state, sizeof(state));
    state.instance = instance;
    state.mode = MODE_SAVER;
    state.fade_in_duration_ms = settings.fade_in_duration_ms;
    state.fade_out_duration_ms = settings.fade_out_duration_ms;
    state.lock_workstation = settings.lock_workstation;
    state.ignore_initial_mouse_move = TRUE;
    GetCursorPos(&state.last_mouse);
    GetVirtualDesktopBounds(&state);
    HideCursor(&state);
    CaptureDesktop(&state);

    /*
     * Capture the desktop before showing the saver window. The window itself is
     * fully opaque; each frame redraws the captured desktop and a black overlay.
     */
    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
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
        ReleaseCapturedDesktop(&state);
        ShowCursorIfHidden(&state);
        MessageBoxW(NULL, L"Could not create the screensaver window.", L"Dim Screensaver C", MB_OK | MB_ICONERROR);
        return 1;
    }

    state.hwnd = hwnd;
    state.start_tick = GetTickCount64();

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ShowCursorIfHidden(&state);
    ReleaseCapturedDesktop(&state);
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
    window_class.hbrBackground = NULL;

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

static Settings LoadSettings(void)
{
    Settings settings;
    wchar_t settings_path[MAX_PATH];
    char buffer[SETTINGS_MAX_BYTES + 1];
    DWORD bytes_read = 0;
    HANDLE file;
    char *cursor;

    settings.fade_in_duration_ms = DEFAULT_FADE_IN_DURATION_MS;
    settings.fade_out_duration_ms = DEFAULT_FADE_OUT_DURATION_MS;
    settings.lock_workstation = DEFAULT_LOCK_WORKSTATION;

    if (!GetSettingsPath(settings_path, MAX_PATH)) {
        return settings;
    }

    file = CreateFileW(
        settings_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (file == INVALID_HANDLE_VALUE) {
        return settings;
    }

    if (!ReadFile(file, buffer, SETTINGS_MAX_BYTES, &bytes_read, NULL)) {
        CloseHandle(file);
        return settings;
    }

    CloseHandle(file);
    buffer[bytes_read] = '\0';

    cursor = buffer;
    if (bytes_read >= 3 &&
        (unsigned char)buffer[0] == 0xEF &&
        (unsigned char)buffer[1] == 0xBB &&
        (unsigned char)buffer[2] == 0xBF) {
        cursor += 3;
    }

    while (*cursor != '\0') {
        char *line = cursor;
        char *separator;
        char *key;
        char *value_text;
        long seconds;
        BOOL bool_value;

        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }

        if (*cursor == '\n') {
            *cursor = '\0';
            cursor++;
        }

        key = TrimAscii(line);
        if (*key == '\0' || *key == '#' || *key == ';') {
            continue;
        }

        separator = strchr(key, '=');
        if (separator == NULL) {
            continue;
        }

        *separator = '\0';
        key = TrimAscii(key);
        value_text = TrimAscii(separator + 1);

        if (EqualsIgnoreCaseAscii(key, "FadeInSeconds") ||
            EqualsIgnoreCaseAscii(key, "LockDelaySeconds")) {
            if (ParseSecondsAscii(value_text, &seconds)) {
                settings.fade_in_duration_ms = (double)seconds * 1000.0;
            }
            continue;
        }

        if (EqualsIgnoreCaseAscii(key, "FadeOutSeconds")) {
            if (ParseSecondsAscii(value_text, &seconds)) {
                settings.fade_out_duration_ms = (double)seconds * 1000.0;
            }
            continue;
        }

        if (EqualsIgnoreCaseAscii(key, "LockWorkstation")) {
            if (ParseBoolAscii(value_text, &bool_value)) {
                settings.lock_workstation = bool_value;
            }
            continue;
        }
    }

    return settings;
}

static BOOL GetSettingsPath(wchar_t *path, DWORD path_count)
{
    DWORD length;
    wchar_t *last_separator;
    wchar_t *extension;
    size_t base_length;

    length = GetModuleFileNameW(NULL, path, path_count);
    if (length == 0 || length >= path_count) {
        return FALSE;
    }

    last_separator = wcsrchr(path, L'\\');
    extension = wcsrchr(path, L'.');
    if (extension == NULL || (last_separator != NULL && extension < last_separator)) {
        extension = path + length;
    }

    base_length = (size_t)(extension - path);
    if (base_length + 4 >= path_count) {
        return FALSE;
    }

    extension[0] = L'.';
    extension[1] = L'i';
    extension[2] = L'n';
    extension[3] = L'i';
    extension[4] = L'\0';
    return TRUE;
}

static char *TrimAscii(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }

    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }

    *end = '\0';
    return text;
}

static BOOL EqualsIgnoreCaseAscii(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        char left_char = *left;
        char right_char = *right;

        if (left_char >= 'A' && left_char <= 'Z') {
            left_char = (char)(left_char - 'A' + 'a');
        }

        if (right_char >= 'A' && right_char <= 'Z') {
            right_char = (char)(right_char - 'A' + 'a');
        }

        if (left_char != right_char) {
            return FALSE;
        }

        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static BOOL ParseSecondsAscii(char *text, long *seconds)
{
    char *parsed_end;
    long value;

    value = strtol(text, &parsed_end, 10);
    if (parsed_end == text) {
        return FALSE;
    }

    parsed_end = TrimAscii(parsed_end);
    if (*parsed_end != '\0') {
        return FALSE;
    }

    if (value < MIN_SETTING_SECONDS) {
        value = MIN_SETTING_SECONDS;
    } else if (value > MAX_SETTING_SECONDS) {
        value = MAX_SETTING_SECONDS;
    }

    *seconds = value;
    return TRUE;
}

static BOOL ParseBoolAscii(const char *text, BOOL *value)
{
    if (EqualsIgnoreCaseAscii(text, "true") ||
        EqualsIgnoreCaseAscii(text, "yes") ||
        EqualsIgnoreCaseAscii(text, "on") ||
        EqualsIgnoreCaseAscii(text, "1")) {
        *value = TRUE;
        return TRUE;
    }

    if (EqualsIgnoreCaseAscii(text, "false") ||
        EqualsIgnoreCaseAscii(text, "no") ||
        EqualsIgnoreCaseAscii(text, "off") ||
        EqualsIgnoreCaseAscii(text, "0")) {
        *value = FALSE;
        return TRUE;
    }

    return FALSE;
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
        SetTimer(hwnd, TIMER_ID, state->mode == MODE_PREVIEW ? 33 : OPACITY_TIMER_INTERVAL_MS, NULL);
        return 0;

    case WM_TIMER:
        if (state != NULL && state->mode == MODE_SAVER) {
            /* Fade-in and fade-out both share the same 15 FPS redraw tick. */
            UpdateSaverFrame(state);
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

            /*
             * Ignore the first synthetic mouse move and require a tiny threshold,
             * otherwise some mice wake the saver immediately due to sensor jitter.
             */
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

static BOOL CaptureDesktop(AppState *state)
{
    HDC screen_dc;
    HDC capture_dc;
    HBITMAP bitmap;
    HGDIOBJ old_bitmap;
    BOOL copied;

    screen_dc = GetDC(NULL);
    if (screen_dc == NULL) {
        return FALSE;
    }

    capture_dc = CreateCompatibleDC(screen_dc);
    if (capture_dc == NULL) {
        ReleaseDC(NULL, screen_dc);
        return FALSE;
    }

    bitmap = CreateCompatibleBitmap(screen_dc, state->virtual_width, state->virtual_height);
    if (bitmap == NULL) {
        DeleteDC(capture_dc);
        ReleaseDC(NULL, screen_dc);
        return FALSE;
    }

    old_bitmap = SelectObject(capture_dc, bitmap);
    copied = BitBlt(
        capture_dc,
        0,
        0,
        state->virtual_width,
        state->virtual_height,
        screen_dc,
        state->virtual_x,
        state->virtual_y,
        SRCCOPY | CAPTUREBLT);

    SelectObject(capture_dc, old_bitmap);
    DeleteDC(capture_dc);
    ReleaseDC(NULL, screen_dc);

    if (!copied) {
        DeleteObject(bitmap);
        return FALSE;
    }

    state->desktop_bitmap = bitmap;
    return TRUE;
}

static BYTE CurrentFadeAlpha(const AppState *state)
{
    ULONGLONG elapsed;
    double progress;
    double eased;
    int alpha;

    elapsed = GetTickCount64() - (state->dismissing ? state->dismiss_start_tick : state->start_tick);
    progress = (double)elapsed / (state->dismissing ? state->fade_out_duration_ms : state->fade_in_duration_ms);
    if (progress < 0.0) {
        progress = 0.0;
    } else if (progress > 1.0) {
        progress = 1.0;
    }

    eased = EaseInOutCubic(progress);
    /* During dismissal, fade down from the current alpha instead of jumping. */
    if (state->dismissing) {
        alpha = (int)(state->dismiss_start_alpha * (1.0 - eased) + 0.5);
    } else {
        alpha = (int)(255.0 * FINAL_DARKNESS * eased + 0.5);
    }

    if (alpha < 0) {
        return 0;
    }
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

static void UpdateSaverFrame(AppState *state)
{
    if (state->hwnd != NULL) {
        BYTE alpha = CurrentFadeAlpha(state);
        InvalidateRect(state->hwnd, NULL, FALSE);

        if (state->dismissing && alpha == 0) {
            DestroyWindow(state->hwnd);
        } else if (!state->dismissing && !state->lock_requested && GetTickCount64() - state->start_tick >= (ULONGLONG)state->fade_in_duration_ms) {
            state->lock_requested = TRUE;
            KillTimer(state->hwnd, TIMER_ID);
            if (state->lock_workstation) {
                LockWorkStation();
                DestroyWindow(state->hwnd);
            }
        }
    }
}

static void PaintSaver(HWND hwnd, AppState *state)
{
    PAINTSTRUCT paint;
    HDC hdc;
    HDC bitmap_dc;
    RECT client;
    HGDIOBJ old_bitmap;
    BYTE alpha;

    hdc = BeginPaint(hwnd, &paint);
    GetClientRect(hwnd, &client);

    if (state->desktop_bitmap != NULL) {
        bitmap_dc = CreateCompatibleDC(hdc);
        if (bitmap_dc != NULL) {
            old_bitmap = SelectObject(bitmap_dc, state->desktop_bitmap);
            BitBlt(
                hdc,
                0,
                0,
                client.right - client.left,
                client.bottom - client.top,
                bitmap_dc,
                0,
                0,
                SRCCOPY);
            SelectObject(bitmap_dc, old_bitmap);
            DeleteDC(bitmap_dc);
        }
    } else {
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &client, brush);
        DeleteObject(brush);
    }

    alpha = CurrentFadeAlpha(state);
    PaintBlackOverlay(hdc, &client, alpha);

    EndPaint(hwnd, &paint);
}

static void PaintBlackOverlay(HDC hdc, const RECT *client, BYTE alpha)
{
    HDC overlay_dc;
    HBITMAP overlay_bitmap;
    HGDIOBJ old_bitmap;
    RECT source_rect;
    HBRUSH black_brush;
    BLENDFUNCTION blend;

    if (alpha == 0) {
        return;
    }

    overlay_dc = CreateCompatibleDC(hdc);
    if (overlay_dc == NULL) {
        return;
    }

    overlay_bitmap = CreateCompatibleBitmap(hdc, 1, 1);
    if (overlay_bitmap == NULL) {
        DeleteDC(overlay_dc);
        return;
    }

    old_bitmap = SelectObject(overlay_dc, overlay_bitmap);
    source_rect.left = 0;
    source_rect.top = 0;
    source_rect.right = 1;
    source_rect.bottom = 1;
    black_brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(overlay_dc, &source_rect, black_brush);
    DeleteObject(black_brush);

    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = 0;

    AlphaBlend(
        hdc,
        client->left,
        client->top,
        client->right - client->left,
        client->bottom - client->top,
        overlay_dc,
        0,
        0,
        1,
        1,
        blend);

    SelectObject(overlay_dc, old_bitmap);
    DeleteObject(overlay_bitmap);
    DeleteDC(overlay_dc);
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

    /* Start a configured fade-out from whatever darkness is currently visible. */
    state->dismiss_start_alpha = CurrentFadeAlpha(state);
    state->dismiss_start_tick = GetTickCount64();
    state->dismissing = TRUE;
    SetTimer(hwnd, TIMER_ID, OPACITY_TIMER_INTERVAL_MS, NULL);
    UpdateSaverFrame(state);
}

static void ReleaseCapturedDesktop(AppState *state)
{
    if (state->desktop_bitmap != NULL) {
        DeleteObject(state->desktop_bitmap);
        state->desktop_bitmap = NULL;
    }
}

static void HideCursor(AppState *state)
{
    /*
     * ShowCursor keeps an internal display counter. Drive it below zero so the
     * cursor stays hidden until ShowCursorIfHidden restores it on exit.
     */
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
        L"Dim Screensaver captures the desktop, dims that captured image, then optionally locks Windows.\n\nEdit the .ini file next to the .scr file to change FadeInSeconds, FadeOutSeconds, and LockWorkstation.\n\nIn Windows Screen Saver Settings, leave \"On resume, display logon screen\" turned off so the saver can capture the visible desktop before locking.",
        L"Dim Screensaver C",
        MB_OK | MB_ICONINFORMATION);
}
