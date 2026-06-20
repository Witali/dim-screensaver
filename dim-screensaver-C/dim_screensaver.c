#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <shellapi.h>
#include <shlobj.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
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
#define ENABLE_DIAGNOSTIC_LOGGING 0

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
    void *desktop_bits;
    HBITMAP dimmed_bitmap;
    void *dimmed_bits;
    int bitmap_width;
    int bitmap_height;
    int bitmap_stride;
    BYTE rendered_alpha;
    BOOL dimmed_bitmap_valid;
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

typedef struct CaptureThreadContext {
    AppState *state;
    BOOL success;
} CaptureThreadContext;

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
static void LogMessage(const wchar_t *format, ...);
#if ENABLE_DIAGNOSTIC_LOGGING
static HANDLE OpenLogFile(void);
#endif
static BOOL GetModuleSiblingPath(const wchar_t *extension, wchar_t *path, DWORD path_count);
#if ENABLE_DIAGNOSTIC_LOGGING
static BOOL GetLocalAppDataLogPath(wchar_t *path, DWORD path_count);
#endif
static BOOL GetSettingsPath(wchar_t *path, DWORD path_count);
static char *TrimAscii(char *text);
static BOOL EqualsIgnoreCaseAscii(const char *left, const char *right);
static BOOL ParseSecondsAscii(char *text, long *seconds);
static BOOL ParseBoolAscii(const char *text, BOOL *value);
static BOOL GetCurrentDesktopName(wchar_t *name, DWORD name_count);
static void LogCurrentDesktopName(void);
static void LogCapturedPixels(HDC capture_dc, int width, int height);
static BOOL CapturedImageLooksLikeBlankSaverDesktop(HDC capture_dc, int width, int height);
static void GetVirtualDesktopBounds(AppState *state);
static BOOL CaptureDesktop(AppState *state);
static BOOL CaptureDefaultDesktop(AppState *state);
static DWORD WINAPI CaptureDefaultDesktopThreadProc(void *parameter);
static BOOL LoadWallpaperBackground(AppState *state);
static BOOL GetCurrentWallpaperPath(wchar_t *path, DWORD path_count);
static BOOL GetTranscodedWallpaperPath(wchar_t *path, DWORD path_count);
static BOOL LoadImageFileAsBitmap(const wchar_t *path, HBITMAP *bitmap, int *width, int *height);
static BOOL CreateWallpaperCanvas(AppState *state, HBITMAP source_bitmap, int source_width, int source_height);
static HBITMAP CreateTopDownDib(int width, int height, void **bits);
static void StoreBackgroundBitmap(AppState *state, HBITMAP bitmap, void *bits, int width, int height);
static BOOL RenderDimmedFrame(AppState *state, BYTE alpha);
static BOOL EnsureDimmedBitmap(AppState *state);
static BYTE CurrentFadeAlpha(const AppState *state);
static double EaseInOutCubic(double value);
static void UpdateSaverFrame(AppState *state);
static void PaintSaver(HWND hwnd, AppState *state);
static void PaintLockCountdown(HDC hdc, const RECT *client, const AppState *state);
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
    LogMessage(L"process start command_line=\"%ls\"", GetCommandLineW());

    command = ParseCommandLine();
    LogMessage(
        L"parsed command mode=%d configure=%d preview_parent=0x%p",
        command.mode,
        command.configure,
        command.preview_parent);

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
    wchar_t desktop_name[256];
    BOOL captured = FALSE;

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
    LogCurrentDesktopName();
    LogMessage(
        L"run saver virtual_bounds x=%d y=%d width=%d height=%d initial_mouse=(%ld,%ld)",
        state.virtual_x,
        state.virtual_y,
        state.virtual_width,
        state.virtual_height,
        state.last_mouse.x,
        state.last_mouse.y);

    HideCursor(&state);
    LogMessage(L"cursor hidden before background load");

    LogMessage(L"loading wallpaper as primary dimming background");
    captured = LoadWallpaperBackground(&state);

    if (!captured) {
        LogMessage(L"wallpaper background unavailable, attempting desktop capture fallback");

        if (GetCurrentDesktopName(desktop_name, (DWORD)(sizeof(desktop_name) / sizeof(desktop_name[0]))) &&
            _wcsicmp(desktop_name, L"Default") != 0) {
            LogMessage(L"current desktop is not Default; attempting worker capture from WinSta0\\Default");
            captured = CaptureDefaultDesktop(&state);
        }
    }

    if (!captured) {
        LogMessage(L"capture current desktop begin");
        captured = CaptureDesktop(&state);
    }

    if (!captured) {
        LogMessage(L"wallpaper and desktop capture unavailable, saver will draw black fallback");
    }

    /*
     * Build the background image before showing the saver window. The window is
     * fully opaque; each frame redraws that image and a black overlay.
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

static void LogMessage(const wchar_t *format, ...)
{
#if ENABLE_DIAGNOSTIC_LOGGING
    HANDLE file;
    SYSTEMTIME now;
    wchar_t message[2048];
    wchar_t line[2300];
    char bytes[8192];
    va_list args;
    int wide_length;
    int byte_length;
    DWORD written;

    file = OpenLogFile();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    va_start(args, format);
    vswprintf_s(message, sizeof(message) / sizeof(message[0]), format, args);
    va_end(args);

    GetLocalTime(&now);
    wide_length = swprintf_s(
        line,
        sizeof(line) / sizeof(line[0]),
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u %ls\r\n",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        message);

    if (wide_length > 0) {
        byte_length = WideCharToMultiByte(
            CP_UTF8,
            0,
            line,
            wide_length,
            bytes,
            sizeof(bytes),
            NULL,
            NULL);

        if (byte_length > 0) {
            WriteFile(file, bytes, (DWORD)byte_length, &written, NULL);
        }
    }

    CloseHandle(file);
#else
    (void)format;
#endif
}

#if ENABLE_DIAGNOSTIC_LOGGING
static HANDLE OpenLogFile(void)
{
    HANDLE file;
    wchar_t path[MAX_PATH];

    if (GetModuleSiblingPath(L".log", path, MAX_PATH)) {
        file = CreateFileW(
            path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }
    }

    if (GetLocalAppDataLogPath(path, MAX_PATH)) {
        file = CreateFileW(
            path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }
    }

    return INVALID_HANDLE_VALUE;
}
#endif

static BOOL GetModuleSiblingPath(const wchar_t *extension, wchar_t *path, DWORD path_count)
{
    DWORD length;
    wchar_t *last_separator;
    wchar_t *existing_extension;
    size_t base_length;
    size_t extension_length;

    length = GetModuleFileNameW(NULL, path, path_count);
    if (length == 0 || length >= path_count) {
        return FALSE;
    }

    last_separator = wcsrchr(path, L'\\');
    existing_extension = wcsrchr(path, L'.');
    if (existing_extension == NULL || (last_separator != NULL && existing_extension < last_separator)) {
        existing_extension = path + length;
    }

    base_length = (size_t)(existing_extension - path);
    extension_length = wcslen(extension);
    if (base_length + extension_length >= path_count) {
        return FALSE;
    }

    wcscpy_s(existing_extension, path_count - base_length, extension);
    return TRUE;
}

#if ENABLE_DIAGNOSTIC_LOGGING
static BOOL GetLocalAppDataLogPath(wchar_t *path, DWORD path_count)
{
    wchar_t directory[MAX_PATH];
    DWORD length;

    length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return FALSE;
    }

    if (swprintf_s(path, path_count, L"%ls\\DimScreensaver", directory) < 0) {
        return FALSE;
    }

    CreateDirectoryW(path, NULL);

    if (swprintf_s(path, path_count, L"%ls\\DimScreensaver\\DimScreensaver.log", directory) < 0) {
        return FALSE;
    }

    return TRUE;
}
#endif

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
        LogMessage(L"settings path unavailable, using defaults");
        return settings;
    }

    LogMessage(L"settings path=\"%ls\"", settings_path);
    file = CreateFileW(
        settings_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (file == INVALID_HANDLE_VALUE) {
        LogMessage(L"settings file open failed error=%lu, using defaults", GetLastError());
        return settings;
    }

    if (!ReadFile(file, buffer, SETTINGS_MAX_BYTES, &bytes_read, NULL)) {
        LogMessage(L"settings file read failed error=%lu, using defaults", GetLastError());
        CloseHandle(file);
        return settings;
    }

    CloseHandle(file);
    buffer[bytes_read] = '\0';
    LogMessage(L"settings file read bytes=%lu", bytes_read);

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

    LogMessage(
        L"settings resolved fade_in_ms=%.0f fade_out_ms=%.0f lock_workstation=%d",
        settings.fade_in_duration_ms,
        settings.fade_out_duration_ms,
        settings.lock_workstation);

    return settings;
}

static BOOL GetSettingsPath(wchar_t *path, DWORD path_count)
{
    return GetModuleSiblingPath(L".ini", path, path_count);
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

static BOOL GetCurrentDesktopName(wchar_t *name, DWORD name_count)
{
    HDESK desktop;
    DWORD needed = 0;

    desktop = GetThreadDesktop(GetCurrentThreadId());
    if (desktop == NULL) {
        return FALSE;
    }

    return GetUserObjectInformationW(desktop, UOI_NAME, name, name_count * sizeof(wchar_t), &needed);
}

static void LogCurrentDesktopName(void)
{
    wchar_t name[256];
    DWORD needed = 0;
    HDESK desktop;

    if (GetCurrentDesktopName(name, (DWORD)(sizeof(name) / sizeof(name[0])))) {
        LogMessage(L"thread desktop name=\"%ls\"", name);
    } else {
        desktop = GetThreadDesktop(GetCurrentThreadId());
        if (desktop != NULL) {
            GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed);
        }

        LogMessage(L"thread desktop name query failed error=%lu needed=%lu", GetLastError(), needed);
    }
}

static void LogCapturedPixels(HDC capture_dc, int width, int height)
{
    COLORREF top_left;
    COLORREF center;
    COLORREF bottom_right;

    top_left = GetPixel(capture_dc, 0, 0);
    center = GetPixel(capture_dc, width / 2, height / 2);
    bottom_right = GetPixel(capture_dc, width > 0 ? width - 1 : 0, height > 0 ? height - 1 : 0);

    LogMessage(
        L"capture pixels top_left=%08lX center=%08lX bottom_right=%08lX",
        (unsigned long)top_left,
        (unsigned long)center,
        (unsigned long)bottom_right);
}

static BOOL CapturedImageLooksLikeBlankSaverDesktop(HDC capture_dc, int width, int height)
{
    POINT points[5];
    COLORREF first;
    int first_red;
    int first_green;
    int first_blue;

    if (width <= 0 || height <= 0) {
        return FALSE;
    }

    points[0].x = 0;
    points[0].y = 0;
    points[1].x = width / 2;
    points[1].y = height / 2;
    points[2].x = width - 1;
    points[2].y = height - 1;
    points[3].x = width / 4;
    points[3].y = height / 4;
    points[4].x = (width * 3) / 4;
    points[4].y = (height * 3) / 4;

    first = GetPixel(capture_dc, points[0].x, points[0].y);
    if (first == CLR_INVALID) {
        return FALSE;
    }

    first_red = GetRValue(first);
    first_green = GetGValue(first);
    first_blue = GetBValue(first);

    for (int index = 0; index < 5; index++) {
        COLORREF color = GetPixel(capture_dc, points[index].x, points[index].y);
        int red;
        int green;
        int blue;

        if (color == CLR_INVALID) {
            return FALSE;
        }

        red = GetRValue(color);
        green = GetGValue(color);
        blue = GetBValue(color);

        if (red > 48 || green > 48 || blue > 48) {
            return FALSE;
        }

        if (abs(red - first_red) > 4 || abs(green - first_green) > 4 || abs(blue - first_blue) > 4) {
            return FALSE;
        }
    }

    return TRUE;
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
    void *bits = NULL;
    HGDIOBJ old_bitmap;
    BOOL copied;
    DWORD bitblt_error;

    screen_dc = GetDC(NULL);
    if (screen_dc == NULL) {
        LogMessage(L"CaptureDesktop GetDC(NULL) failed error=%lu", GetLastError());
        return FALSE;
    }

    capture_dc = CreateCompatibleDC(screen_dc);
    if (capture_dc == NULL) {
        LogMessage(L"CaptureDesktop CreateCompatibleDC failed error=%lu", GetLastError());
        ReleaseDC(NULL, screen_dc);
        return FALSE;
    }

    bitmap = CreateTopDownDib(state->virtual_width, state->virtual_height, &bits);
    if (bitmap == NULL) {
        LogMessage(
            L"CaptureDesktop CreateTopDownDib failed width=%d height=%d error=%lu",
            state->virtual_width,
            state->virtual_height,
            GetLastError());
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
    bitblt_error = copied ? 0 : GetLastError();

    if (copied) {
        LogMessage(
            L"CaptureDesktop BitBlt succeeded width=%d height=%d source=(%d,%d)",
            state->virtual_width,
            state->virtual_height,
            state->virtual_x,
            state->virtual_y);
        LogCapturedPixels(capture_dc, state->virtual_width, state->virtual_height);
        if (CapturedImageLooksLikeBlankSaverDesktop(capture_dc, state->virtual_width, state->virtual_height)) {
            LogMessage(L"CaptureDesktop image looks like the blank Screen-saver desktop; discarding capture");
            copied = FALSE;
        }
    } else {
        LogMessage(L"CaptureDesktop BitBlt failed error=%lu", bitblt_error);
    }

    SelectObject(capture_dc, old_bitmap);
    DeleteDC(capture_dc);
    ReleaseDC(NULL, screen_dc);

    if (!copied) {
        DeleteObject(bitmap);
        return FALSE;
    }

    StoreBackgroundBitmap(state, bitmap, bits, state->virtual_width, state->virtual_height);
    return TRUE;
}

static BOOL CaptureDefaultDesktop(AppState *state)
{
    CaptureThreadContext context;
    HANDLE thread;

    context.state = state;
    context.success = FALSE;

    thread = CreateThread(NULL, 0, CaptureDefaultDesktopThreadProc, &context, 0, NULL);
    if (thread == NULL) {
        LogMessage(L"Default desktop capture worker CreateThread failed error=%lu", GetLastError());
        return FALSE;
    }

    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    LogMessage(L"Default desktop capture worker finished success=%d", context.success);
    return context.success;
}

static DWORD WINAPI CaptureDefaultDesktopThreadProc(void *parameter)
{
    CaptureThreadContext *context = (CaptureThreadContext *)parameter;
    HDESK default_desktop;
    DWORD desired_access;

    desired_access =
        DESKTOP_READOBJECTS |
        DESKTOP_WRITEOBJECTS |
        DESKTOP_ENUMERATE |
        DESKTOP_CREATEWINDOW |
        DESKTOP_SWITCHDESKTOP;

    default_desktop = OpenDesktopW(L"Default", 0, FALSE, desired_access);
    if (default_desktop == NULL) {
        LogMessage(L"OpenDesktop(Default) failed error=%lu", GetLastError());
        return 0;
    }

    if (!SetThreadDesktop(default_desktop)) {
        LogMessage(L"SetThreadDesktop(Default) failed error=%lu", GetLastError());
        CloseDesktop(default_desktop);
        return 0;
    }

    LogMessage(L"worker switched to Default desktop");
    LogCurrentDesktopName();
    context->success = CaptureDesktop(context->state);
    CloseDesktop(default_desktop);
    return 0;
}

static BOOL LoadWallpaperBackground(AppState *state)
{
    wchar_t wallpaper_path[MAX_PATH];
    HBITMAP wallpaper_bitmap = NULL;
    int wallpaper_width = 0;
    int wallpaper_height = 0;
    BOOL loaded;

    if (!GetCurrentWallpaperPath(wallpaper_path, (DWORD)(sizeof(wallpaper_path) / sizeof(wallpaper_path[0])))) {
        return FALSE;
    }

    loaded = LoadImageFileAsBitmap(wallpaper_path, &wallpaper_bitmap, &wallpaper_width, &wallpaper_height);
    if (!loaded) {
        return FALSE;
    }

    loaded = CreateWallpaperCanvas(state, wallpaper_bitmap, wallpaper_width, wallpaper_height);
    DeleteObject(wallpaper_bitmap);

    return loaded;
}

static BOOL GetCurrentWallpaperPath(wchar_t *path, DWORD path_count)
{
    DWORD attributes;

    if (path == NULL || path_count == 0) {
        return FALSE;
    }

    path[0] = L'\0';
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, path_count, path, 0) && path[0] != L'\0') {
        attributes = GetFileAttributesW(path);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            LogMessage(L"wallpaper path from SystemParametersInfo=\"%ls\"", path);
            return TRUE;
        }

        LogMessage(L"wallpaper path from SystemParametersInfo is not readable=\"%ls\"", path);
    } else {
        LogMessage(L"SystemParametersInfo(SPI_GETDESKWALLPAPER) returned no readable path error=%lu", GetLastError());
    }

    if (!GetTranscodedWallpaperPath(path, path_count)) {
        return FALSE;
    }

    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        LogMessage(L"transcoded wallpaper fallback is not readable=\"%ls\"", path);
        return FALSE;
    }

    LogMessage(L"wallpaper path from TranscodedWallpaper fallback=\"%ls\"", path);
    return TRUE;
}

static BOOL GetTranscodedWallpaperPath(wchar_t *path, DWORD path_count)
{
    wchar_t app_data[MAX_PATH];
    HRESULT hr;
    int written;

    hr = SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, app_data);
    if (FAILED(hr)) {
        LogMessage(L"SHGetFolderPath(CSIDL_APPDATA) failed hr=0x%08lX", (unsigned long)hr);
        return FALSE;
    }

    written = swprintf(
        path,
        path_count,
        L"%ls\\Microsoft\\Windows\\Themes\\TranscodedWallpaper",
        app_data);

    return written > 0 && (DWORD)written < path_count;
}

static BOOL LoadImageFileAsBitmap(const wchar_t *path, HBITMAP *bitmap, int *width, int *height)
{
    HRESULT hr;
    BOOL com_initialized = FALSE;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT source_width = 0;
    UINT source_height = 0;
    UINT stride;
    UINT image_size;
    BITMAPINFO bitmap_info;
    void *bits = NULL;
    HBITMAP decoded_bitmap = NULL;

    *bitmap = NULL;
    *width = 0;
    *height = 0;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        com_initialized = TRUE;
    } else if (hr != RPC_E_CHANGED_MODE) {
        LogMessage(L"CoInitializeEx for wallpaper decode failed hr=0x%08lX", (unsigned long)hr);
        return FALSE;
    }

    hr = CoCreateInstance(
        &CLSID_WICImagingFactory,
        NULL,
        CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory,
        (void **)&factory);
    if (FAILED(hr)) {
        LogMessage(L"CoCreateInstance(WICImagingFactory) failed hr=0x%08lX", (unsigned long)hr);
        goto cleanup;
    }

    hr = factory->lpVtbl->CreateDecoderFromFilename(
        factory,
        path,
        NULL,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(hr)) {
        LogMessage(L"WIC CreateDecoderFromFilename failed path=\"%ls\" hr=0x%08lX", path, (unsigned long)hr);
        goto cleanup;
    }

    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) {
        LogMessage(L"WIC GetFrame failed hr=0x%08lX", (unsigned long)hr);
        goto cleanup;
    }

    hr = frame->lpVtbl->GetSize(frame, &source_width, &source_height);
    if (FAILED(hr) || source_width == 0 || source_height == 0) {
        LogMessage(L"WIC GetSize failed hr=0x%08lX width=%u height=%u", (unsigned long)hr, source_width, source_height);
        goto cleanup;
    }

    if (source_width > (UINT_MAX / 4) || source_height > (UINT_MAX / (source_width * 4))) {
        LogMessage(L"wallpaper image is too large width=%u height=%u", source_width, source_height);
        goto cleanup;
    }

    hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) {
        LogMessage(L"WIC CreateFormatConverter failed hr=0x%08lX", (unsigned long)hr);
        goto cleanup;
    }

    hr = converter->lpVtbl->Initialize(
        converter,
        (IWICBitmapSource *)frame,
        &GUID_WICPixelFormat32bppBGR,
        WICBitmapDitherTypeNone,
        NULL,
        0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        LogMessage(L"WIC format conversion failed hr=0x%08lX", (unsigned long)hr);
        goto cleanup;
    }

    stride = source_width * 4;
    image_size = stride * source_height;
    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
    bitmap_info.bmiHeader.biWidth = (LONG)source_width;
    bitmap_info.bmiHeader.biHeight = -(LONG)source_height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    decoded_bitmap = CreateDIBSection(NULL, &bitmap_info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (decoded_bitmap == NULL || bits == NULL) {
        LogMessage(L"CreateDIBSection for wallpaper failed error=%lu", GetLastError());
        goto cleanup;
    }

    hr = converter->lpVtbl->CopyPixels(converter, NULL, stride, image_size, (BYTE *)bits);
    if (FAILED(hr)) {
        LogMessage(L"WIC CopyPixels failed hr=0x%08lX", (unsigned long)hr);
        DeleteObject(decoded_bitmap);
        decoded_bitmap = NULL;
        goto cleanup;
    }

    *bitmap = decoded_bitmap;
    *width = (int)source_width;
    *height = (int)source_height;
    decoded_bitmap = NULL;
    LogMessage(L"wallpaper decoded width=%d height=%d path=\"%ls\"", *width, *height, path);

cleanup:
    if (decoded_bitmap != NULL) {
        DeleteObject(decoded_bitmap);
    }
    if (converter != NULL) {
        converter->lpVtbl->Release(converter);
    }
    if (frame != NULL) {
        frame->lpVtbl->Release(frame);
    }
    if (decoder != NULL) {
        decoder->lpVtbl->Release(decoder);
    }
    if (factory != NULL) {
        factory->lpVtbl->Release(factory);
    }
    if (com_initialized) {
        CoUninitialize();
    }

    return *bitmap != NULL;
}

static BOOL CreateWallpaperCanvas(AppState *state, HBITMAP source_bitmap, int source_width, int source_height)
{
    HDC screen_dc;
    HDC source_dc;
    HDC target_dc;
    HBITMAP target_bitmap;
    void *target_bits = NULL;
    HGDIOBJ old_source_bitmap;
    HGDIOBJ old_target_bitmap;
    RECT target_rect;
    HBRUSH black_brush;
    double scale_x;
    double scale_y;
    double scale;
    int draw_width;
    int draw_height;
    int draw_x;
    int draw_y;
    BOOL painted;

    if (source_bitmap == NULL || source_width <= 0 || source_height <= 0) {
        return FALSE;
    }

    screen_dc = GetDC(NULL);
    if (screen_dc == NULL) {
        LogMessage(L"wallpaper background GetDC(NULL) failed error=%lu", GetLastError());
        return FALSE;
    }

    source_dc = CreateCompatibleDC(screen_dc);
    target_dc = CreateCompatibleDC(screen_dc);
    target_bitmap = CreateTopDownDib(state->virtual_width, state->virtual_height, &target_bits);
    if (source_dc == NULL || target_dc == NULL || target_bitmap == NULL) {
        LogMessage(L"wallpaper background DC/bitmap creation failed error=%lu", GetLastError());
        if (target_bitmap != NULL) {
            DeleteObject(target_bitmap);
        }
        if (target_dc != NULL) {
            DeleteDC(target_dc);
        }
        if (source_dc != NULL) {
            DeleteDC(source_dc);
        }
        ReleaseDC(NULL, screen_dc);
        return FALSE;
    }

    old_source_bitmap = SelectObject(source_dc, source_bitmap);
    old_target_bitmap = SelectObject(target_dc, target_bitmap);

    target_rect.left = 0;
    target_rect.top = 0;
    target_rect.right = state->virtual_width;
    target_rect.bottom = state->virtual_height;
    black_brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(target_dc, &target_rect, black_brush);
    DeleteObject(black_brush);

    scale_x = (double)state->virtual_width / (double)source_width;
    scale_y = (double)state->virtual_height / (double)source_height;
    scale = scale_x > scale_y ? scale_x : scale_y;
    draw_width = (int)((double)source_width * scale + 0.5);
    draw_height = (int)((double)source_height * scale + 0.5);
    draw_x = (state->virtual_width - draw_width) / 2;
    draw_y = (state->virtual_height - draw_height) / 2;

    SetStretchBltMode(target_dc, HALFTONE);
    SetBrushOrgEx(target_dc, 0, 0, NULL);
    painted = StretchBlt(
        target_dc,
        draw_x,
        draw_y,
        draw_width,
        draw_height,
        source_dc,
        0,
        0,
        source_width,
        source_height,
        SRCCOPY);

    SelectObject(source_dc, old_source_bitmap);
    SelectObject(target_dc, old_target_bitmap);
    DeleteDC(source_dc);
    DeleteDC(target_dc);
    ReleaseDC(NULL, screen_dc);

    if (!painted) {
        LogMessage(L"wallpaper background StretchBlt failed error=%lu", GetLastError());
        DeleteObject(target_bitmap);
        return FALSE;
    }

    StoreBackgroundBitmap(state, target_bitmap, target_bits, state->virtual_width, state->virtual_height);
    LogMessage(
        L"wallpaper background ready source=%dx%d dest=%dx%d draw=(%d,%d,%d,%d)",
        source_width,
        source_height,
        state->virtual_width,
        state->virtual_height,
        draw_x,
        draw_y,
        draw_width,
        draw_height);
    return TRUE;
}

static HBITMAP CreateTopDownDib(int width, int height, void **bits)
{
    BITMAPINFO bitmap_info;

    if (bits == NULL || width <= 0 || height <= 0) {
        return NULL;
    }

    *bits = NULL;
    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    return CreateDIBSection(NULL, &bitmap_info, DIB_RGB_COLORS, bits, NULL, 0);
}

static void StoreBackgroundBitmap(AppState *state, HBITMAP bitmap, void *bits, int width, int height)
{
    ReleaseCapturedDesktop(state);
    state->desktop_bitmap = bitmap;
    state->desktop_bits = bits;
    state->bitmap_width = width;
    state->bitmap_height = height;
    state->bitmap_stride = width * 4;
    state->dimmed_bitmap_valid = FALSE;
}

static BOOL RenderDimmedFrame(AppState *state, BYTE alpha)
{
    const DWORD *source;
    DWORD *target;
    size_t pixel_count;
    unsigned int factor;

    if (state->desktop_bitmap == NULL || state->desktop_bits == NULL) {
        return FALSE;
    }

    if (!EnsureDimmedBitmap(state)) {
        return FALSE;
    }

    if (state->dimmed_bitmap_valid && state->rendered_alpha == alpha) {
        return TRUE;
    }

    source = (const DWORD *)state->desktop_bits;
    target = (DWORD *)state->dimmed_bits;
    pixel_count = (size_t)state->bitmap_width * (size_t)state->bitmap_height;
    factor = 255U - (unsigned int)alpha;

    for (size_t index = 0; index < pixel_count; index++) {
        DWORD pixel = source[index];
        unsigned int blue = pixel & 0xFFU;
        unsigned int green = (pixel >> 8) & 0xFFU;
        unsigned int red = (pixel >> 16) & 0xFFU;

        blue = (blue * factor + 127U) / 255U;
        green = (green * factor + 127U) / 255U;
        red = (red * factor + 127U) / 255U;

        target[index] = (pixel & 0xFF000000U) | (red << 16) | (green << 8) | blue;
    }

    state->rendered_alpha = alpha;
    state->dimmed_bitmap_valid = TRUE;
    return TRUE;
}

static BOOL EnsureDimmedBitmap(AppState *state)
{
    if (state->dimmed_bitmap != NULL &&
        state->dimmed_bits != NULL &&
        state->bitmap_width > 0 &&
        state->bitmap_height > 0) {
        return TRUE;
    }

    if (state->dimmed_bitmap != NULL) {
        DeleteObject(state->dimmed_bitmap);
        state->dimmed_bitmap = NULL;
        state->dimmed_bits = NULL;
    }

    state->dimmed_bitmap = CreateTopDownDib(state->bitmap_width, state->bitmap_height, &state->dimmed_bits);
    if (state->dimmed_bitmap == NULL || state->dimmed_bits == NULL) {
        LogMessage(
            L"CreateTopDownDib for dimmed frame failed width=%d height=%d error=%lu",
            state->bitmap_width,
            state->bitmap_height,
            GetLastError());
        return FALSE;
    }

    state->dimmed_bitmap_valid = FALSE;
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
    HBITMAP frame_bitmap;

    hdc = BeginPaint(hwnd, &paint);
    GetClientRect(hwnd, &client);

    if (state->desktop_bitmap != NULL) {
        bitmap_dc = CreateCompatibleDC(hdc);
        if (bitmap_dc != NULL) {
            alpha = CurrentFadeAlpha(state);
            frame_bitmap = RenderDimmedFrame(state, alpha) ? state->dimmed_bitmap : state->desktop_bitmap;
            old_bitmap = SelectObject(bitmap_dc, frame_bitmap);
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

    PaintLockCountdown(hdc, &client, state);

    EndPaint(hwnd, &paint);
}

static void PaintLockCountdown(HDC hdc, const RECT *client, const AppState *state)
{
    ULONGLONG elapsed;
    double remaining_ms;
    int remaining_seconds;
    int width;
    int height;
    int font_height;
    int margin;
    wchar_t text[32];
    HFONT font;
    HFONT old_font;
    RECT text_rect;
    RECT shadow_rect;
    int old_bk_mode;
    COLORREF old_text_color;

    if (!state->lock_workstation || state->dismissing || state->lock_requested) {
        return;
    }

    elapsed = GetTickCount64() - state->start_tick;
    remaining_ms = state->fade_in_duration_ms - (double)elapsed;
    if (remaining_ms <= 0.0) {
        remaining_seconds = 0;
    } else {
        remaining_seconds = (int)ceil(remaining_ms / 1000.0);
    }

    width = client->right - client->left;
    height = client->bottom - client->top;
    if (width <= 0 || height <= 0) {
        return;
    }

    font_height = height / 18;
    if (font_height < 28) {
        font_height = 28;
    } else if (font_height > 72) {
        font_height = 72;
    }

    margin = height / 32;
    if (margin < 24) {
        margin = 24;
    } else if (margin > 72) {
        margin = 72;
    }

    swprintf_s(text, sizeof(text) / sizeof(text[0]), L"%d", remaining_seconds);
    font = CreateFontW(
        -font_height,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    if (font == NULL) {
        return;
    }

    old_font = (HFONT)SelectObject(hdc, font);
    old_bk_mode = SetBkMode(hdc, TRANSPARENT);
    old_text_color = SetTextColor(hdc, RGB(245, 245, 245));

    text_rect.left = client->left + margin;
    text_rect.top = client->top + margin;
    text_rect.right = client->right - margin;
    text_rect.bottom = client->bottom - margin;

    shadow_rect = text_rect;
    OffsetRect(&shadow_rect, 2, 2);
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextW(hdc, text, -1, &shadow_rect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(hdc, RGB(245, 245, 245));
    DrawTextW(hdc, text, -1, &text_rect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(hdc, old_text_color);
    SetBkMode(hdc, old_bk_mode);
    SelectObject(hdc, old_font);
    DeleteObject(font);
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
    if (state->dimmed_bitmap != NULL) {
        DeleteObject(state->dimmed_bitmap);
        state->dimmed_bitmap = NULL;
        state->dimmed_bits = NULL;
    }

    if (state->desktop_bitmap != NULL) {
        DeleteObject(state->desktop_bitmap);
        state->desktop_bitmap = NULL;
    }

    state->desktop_bits = NULL;
    state->bitmap_width = 0;
    state->bitmap_height = 0;
    state->bitmap_stride = 0;
    state->rendered_alpha = 0;
    state->dimmed_bitmap_valid = FALSE;
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
        L"Dim Screensaver shows the current Windows desktop wallpaper, dims that image, then optionally locks Windows.\n\nEdit the .ini file next to the .scr file to change FadeInSeconds, FadeOutSeconds, and LockWorkstation.\n\nIn Windows Screen Saver Settings, leave \"On resume, display logon screen\" turned off because this saver performs its own delayed lock.",
        L"Dim Screensaver C",
        MB_OK | MB_ICONINFORMATION);
}
