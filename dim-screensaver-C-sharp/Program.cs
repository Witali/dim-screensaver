using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace DimScreensaver
{
    internal static class Program
    {
        private const int DefaultFadeInDurationMs = 10000;
        private const int DefaultFadeOutDurationMs = 1000;
        private const bool DefaultLockWorkstation = true;
        private const int FrameTimerIntervalMs = 67;
        private const float FinalDarkness = 0.94f;
        private const int MinSettingSeconds = 1;
        private const int MaxSettingSeconds = 3600;
        private const int SpiGetDesktopWallpaper = 0x0073;
        private const int UoiName = 2;
        private static string logPath;

        // Entry point for .scr execution; dispatches to preview, configuration, or saver mode.
        [STAThread]
        private static void Main(string[] args)
        {
            // Screen bounds must be read in physical pixels.
            // Without this, Windows can DPI-virtualize the process and make the
            // saver look as if the monitor resolution changed.
            EnableDpiAwareness();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Log("process start command_line=\"{0}\" executable=\"{1}\"", Environment.CommandLine, Application.ExecutablePath);

            // Windows launches .scr files with small command-line switches:
            // /s runs the saver, /p embeds a preview into Screen Saver Settings,
            // and /c opens configuration.
            ScreenSaverCommand command = ScreenSaverCommand.Parse(args);
            Log("parsed command kind={0} preview_parent=0x{1:X}", command.Kind, command.PreviewParentHandle.ToInt64());

            switch (command.Kind)
            {
                case ScreenSaverCommandKind.Preview:
                    Application.Run(new PreviewForm(command.PreviewParentHandle));
                    break;
                case ScreenSaverCommandKind.Configure:
                    MessageBox.Show(
                        "Dim Screensaver shows a configured image or the current Windows desktop wallpaper, dims that image, then optionally locks Windows.\n\n" +
                        "Edit the .ini file next to the .scr file to change FadeInSeconds, FadeOutSeconds, LockWorkstation, and BackgroundImagePath.\n\n" +
                        "In Windows Screen Saver Settings, leave \"On resume, display logon screen\" turned off because this saver performs its own delayed lock.",
                        "Dim Screensaver",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Information);
                    break;
                case ScreenSaverCommandKind.Run:
                default:
                    RunScreensaver();
                    break;
            }
        }

        // Starts the full-screen saver context with settings loaded from the .ini file.
        private static void RunScreensaver()
        {
            using (ScreensaverContext context = new ScreensaverContext(LoadSettings()))
            {
                Application.Run(context);
            }
        }

        // Appends one diagnostic log line, ignoring logging failures so the saver can keep running.
        private static void Log(string format, params object[] args)
        {
            try
            {
                string message = args.Length == 0
                    ? format
                    : string.Format(CultureInfo.InvariantCulture, format, args);
                string line = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture) + " " + message + Environment.NewLine;
                File.AppendAllText(GetLogPath(), line, Encoding.UTF8);
            }
            catch (Exception)
            {
            }
        }

        // Chooses a writable diagnostic log path beside the .scr or under LocalAppData.
        private static string GetLogPath()
        {
            if (logPath != null)
            {
                return logPath;
            }

            string siblingPath = Path.ChangeExtension(Application.ExecutablePath, ".log");
            try
            {
                using (new FileStream(siblingPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
                {
                }

                logPath = siblingPath;
                return logPath;
            }
            catch (Exception)
            {
            }

            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string directory = Path.Combine(localAppData, "DimScreensaver");
            Directory.CreateDirectory(directory);
            logPath = Path.Combine(directory, "DimScreensaver.log");
            return logPath;
        }

        // Reads fade, lock, and background-image settings from the .ini next to the .scr.
        private static ScreensaverSettings LoadSettings()
        {
            ScreensaverSettings settings = ScreensaverSettings.CreateDefault();
            string settingsPath = Path.ChangeExtension(Application.ExecutablePath, ".ini");
            Log("settings path=\"{0}\"", settingsPath);

            try
            {
                if (!File.Exists(settingsPath))
                {
                    Log("settings file missing, using defaults");
                    return settings;
                }

                foreach (string line in File.ReadAllLines(settingsPath))
                {
                    string trimmed = line.Trim();
                    int separator;
                    string key;
                    string valueText;
                    int seconds;
                    bool boolValue;

                    if (trimmed.Length == 0 || trimmed.StartsWith("#", StringComparison.Ordinal) || trimmed.StartsWith(";", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    separator = trimmed.IndexOf('=');
                    if (separator < 0)
                    {
                        continue;
                    }

                    key = trimmed.Substring(0, separator).Trim();
                    valueText = trimmed.Substring(separator + 1).Trim();

                    if (string.Equals(key, "FadeInSeconds", StringComparison.OrdinalIgnoreCase) ||
                        string.Equals(key, "LockDelaySeconds", StringComparison.OrdinalIgnoreCase))
                    {
                        if (TryParseSeconds(valueText, out seconds))
                        {
                            settings.FadeInDurationMs = seconds * 1000;
                        }

                        continue;
                    }

                    if (string.Equals(key, "FadeOutSeconds", StringComparison.OrdinalIgnoreCase))
                    {
                        if (TryParseSeconds(valueText, out seconds))
                        {
                            settings.FadeOutDurationMs = seconds * 1000;
                        }

                        continue;
                    }

                    if (string.Equals(key, "LockWorkstation", StringComparison.OrdinalIgnoreCase))
                    {
                        if (TryParseBoolean(valueText, out boolValue))
                        {
                            settings.LockWorkstation = boolValue;
                        }

                        continue;
                    }

                    if (string.Equals(key, "BackgroundImagePath", StringComparison.OrdinalIgnoreCase) ||
                        string.Equals(key, "ImagePath", StringComparison.OrdinalIgnoreCase))
                    {
                        settings.BackgroundImagePath = UnquotePath(valueText);
                        continue;
                    }
                }
            }
            catch (Exception)
            {
                Log("settings read failed, using defaults");
                return ScreensaverSettings.CreateDefault();
            }

            Log(
                "settings resolved fade_in_ms={0} fade_out_ms={1} lock_workstation={2} background_image_path=\"{3}\"",
                settings.FadeInDurationMs,
                settings.FadeOutDurationMs,
                settings.LockWorkstation,
                settings.BackgroundImagePath ?? string.Empty);
            return settings;
        }

        // Parses and clamps a seconds value from the settings file.
        private static bool TryParseSeconds(string text, out int seconds)
        {
            if (!int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out seconds))
            {
                return false;
            }

            seconds = Math.Max(MinSettingSeconds, Math.Min(MaxSettingSeconds, seconds));
            return true;
        }

        // Parses common true/false spellings from the settings file.
        private static bool TryParseBoolean(string text, out bool value)
        {
            string normalized = text.Trim();

            if (string.Equals(normalized, "true", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(normalized, "yes", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(normalized, "on", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(normalized, "1", StringComparison.OrdinalIgnoreCase))
            {
                value = true;
                return true;
            }

            if (string.Equals(normalized, "false", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(normalized, "no", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(normalized, "off", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(normalized, "0", StringComparison.OrdinalIgnoreCase))
            {
                value = false;
                return true;
            }

            value = false;
            return false;
        }

        // Removes optional single or double quotes from a configured path.
        private static string UnquotePath(string text)
        {
            string trimmed = text == null ? string.Empty : text.Trim();
            if (trimmed.Length >= 2 &&
                ((trimmed[0] == '"' && trimmed[trimmed.Length - 1] == '"') ||
                 (trimmed[0] == '\'' && trimmed[trimmed.Length - 1] == '\'')))
            {
                return trimmed.Substring(1, trimmed.Length - 2);
            }

            return trimmed;
        }

        // Records the current thread desktop name for diagnostics.
        private static void LogCurrentDesktopName()
        {
            IntPtr desktop = GetThreadDesktop(GetCurrentThreadId());
            if (desktop == IntPtr.Zero)
            {
                Log("thread desktop unavailable error={0}", Marshal.GetLastWin32Error());
                return;
            }

            StringBuilder name = new StringBuilder(256);
            int needed;
            if (GetUserObjectInformation(desktop, UoiName, name, name.Capacity * 2, out needed))
            {
                Log("thread desktop name=\"{0}\"", name);
            }
            else
            {
                Log("thread desktop name query failed error={0} needed={1}", Marshal.GetLastWin32Error(), needed);
            }
        }

        // Enables DPI awareness so screen bounds are read in physical pixels.
        private static void EnableDpiAwareness()
        {
            try
            {
                // PER_MONITOR_AWARE_V2 is available on modern Windows 10/11.
                if (SetProcessDpiAwarenessContext(new IntPtr(-4)))
                {
                    return;
                }
            }
            catch (EntryPointNotFoundException)
            {
            }
            catch (DllNotFoundException)
            {
            }

            try
            {
                // Older Windows versions still understand process-wide DPI awareness.
                SetProcessDPIAware();
            }
            catch (EntryPointNotFoundException)
            {
            }
            catch (DllNotFoundException)
            {
            }
        }

        private sealed class ScreensaverSettings
        {
            public int FadeInDurationMs { get; set; }

            public int FadeOutDurationMs { get; set; }

            public bool LockWorkstation { get; set; }

            public string BackgroundImagePath { get; set; }

            // Creates the default settings used when the .ini file is missing or invalid.
            public static ScreensaverSettings CreateDefault()
            {
                return new ScreensaverSettings
                {
                    FadeInDurationMs = DefaultFadeInDurationMs,
                    FadeOutDurationMs = DefaultFadeOutDurationMs,
                    LockWorkstation = DefaultLockWorkstation,
                    BackgroundImagePath = string.Empty,
                };
            }
        }

        private sealed class ScreensaverContext : ApplicationContext
        {
            private readonly List<DimForm> forms = new List<DimForm>();
            private readonly ScreensaverSettings settings;
            private int openForms;
            private bool cursorHidden;
            private bool locking;

            // Creates one saver form per monitor and wires shared exit and lock events.
            public ScreensaverContext(ScreensaverSettings settings)
            {
                this.settings = settings;
                HideCursor();
                Log("cursor hidden before background load");
                LogCurrentDesktopName();
                List<ScreenCapture> captures = CaptureScreens(this.settings.BackgroundImagePath);
                openForms = captures.Count;

                foreach (ScreenCapture capture in captures)
                {
                    // Each monitor gets its own captured bitmap and borderless
                    // window. This avoids stitching screens together and handles
                    // negative monitor coordinates.
                    DimForm form = new DimForm(capture.Bitmap, capture.Bounds, this.settings.FadeInDurationMs, this.settings.FadeOutDurationMs, FrameTimerIntervalMs, FinalDarkness, this.settings.LockWorkstation);
                    form.FormClosed += HandleFormClosed;
                    form.ExitRequested += CloseAll;
                    form.LockRequested += LockWorkstationAndClose;
                    forms.Add(form);
                    form.Show();
                }
            }

            // Releases saver forms and restores the cursor before the application exits.
            protected override void Dispose(bool disposing)
            {
                if (disposing)
                {
                    foreach (DimForm form in forms)
                    {
                        form.FormClosed -= HandleFormClosed;
                        form.ExitRequested -= CloseAll;
                        form.LockRequested -= LockWorkstationAndClose;
                        form.Dispose();
                    }
                }

                ShowCursorIfHidden();
                base.Dispose(disposing);
            }

            // Hides the cursor by driving the Win32 display counter below zero.
            private void HideCursor()
            {
                while (ShowCursor(false) >= 0)
                {
                }

                cursorHidden = true;
            }

            // Restores the Win32 cursor display counter if this context hid it.
            private void ShowCursorIfHidden()
            {
                if (!cursorHidden)
                {
                    return;
                }

                while (ShowCursor(true) < 0)
                {
                }

                cursorHidden = false;
            }

            // Builds one frozen background bitmap for each screen.
            private static List<ScreenCapture> CaptureScreens(string configuredBackgroundImagePath)
            {
                List<ScreenCapture> captures = new List<ScreenCapture>();
                Log("build wallpaper backgrounds begin count={0}", Screen.AllScreens.Length);

                foreach (Screen screen in Screen.AllScreens)
                {
                    Rectangle bounds = screen.Bounds;
                    Bitmap bitmap = CreateWallpaperBackground(bounds, configuredBackgroundImagePath);
                    bool copied = false;

                    if (bitmap == null)
                    {
                        Log(
                            "wallpaper background unavailable bounds=({0},{1},{2},{3}); attempting desktop capture fallback",
                            bounds.X,
                            bounds.Y,
                            bounds.Width,
                            bounds.Height);
                        bitmap = new Bitmap(bounds.Width, bounds.Height);

                        using (Graphics graphics = Graphics.FromImage(bitmap))
                        {
                            graphics.Clear(Color.Black);
                            try
                            {
                                graphics.CopyFromScreen(bounds.Location, Point.Empty, bounds.Size, CopyPixelOperation.SourceCopy);
                                copied = true;
                            }
                            catch (Exception ex)
                            {
                                Log(
                                    "CopyFromScreen failed bounds=({0},{1},{2},{3}) exception={4}: {5}",
                                    bounds.X,
                                    bounds.Y,
                                    bounds.Width,
                                    bounds.Height,
                                    ex.GetType().Name,
                                    ex.Message);
                            }
                        }
                    }

                    Log(
                        "screen background ready copied_from_screen={0} bounds=({1},{2},{3},{4}) primary={5} device=\"{6}\"",
                        copied,
                        bounds.X,
                        bounds.Y,
                        bounds.Width,
                        bounds.Height,
                        screen.Primary,
                        screen.DeviceName);
                    LogBitmapPixels(bitmap);
                    captures.Add(new ScreenCapture(bitmap, bounds));
                }

                return captures;
            }

            // Loads the configured image or wallpaper and crops it to one monitor's bounds.
            private static Bitmap CreateWallpaperBackground(Rectangle bounds, string configuredBackgroundImagePath)
            {
                string backgroundPath = GetBackgroundImagePath(configuredBackgroundImagePath);
                if (string.IsNullOrEmpty(backgroundPath))
                {
                    return null;
                }

                try
                {
                    using (Image wallpaper = Image.FromFile(backgroundPath))
                    {
                        Bitmap bitmap = new Bitmap(bounds.Width, bounds.Height);
                        using (Graphics graphics = Graphics.FromImage(bitmap))
                        {
                            Rectangle destination = GetCoverDestination(bounds.Size, wallpaper.Size);
                            graphics.Clear(Color.Black);
                            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
                            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
                            graphics.DrawImage(wallpaper, destination);
                        }

                        Log(
                            "wallpaper background ready source=({0},{1}) dest=({2},{3}) path=\"{4}\"",
                            wallpaper.Width,
                            wallpaper.Height,
                            bounds.Width,
                            bounds.Height,
                            backgroundPath);
                        return bitmap;
                    }
                }
                catch (Exception ex)
                {
                    Log(
                        "wallpaper background load failed path=\"{0}\" exception={1}: {2}",
                        backgroundPath,
                        ex.GetType().Name,
                        ex.Message);
                    return null;
                }
            }

            // Chooses the configured image path, falling back to the current wallpaper.
            private static string GetBackgroundImagePath(string configuredBackgroundImagePath)
            {
                if (!string.IsNullOrWhiteSpace(configuredBackgroundImagePath))
                {
                    string resolvedPath = ResolveConfiguredImagePath(configuredBackgroundImagePath);
                    if (File.Exists(resolvedPath))
                    {
                        Log("background image path from settings=\"{0}\"", resolvedPath);
                        return resolvedPath;
                    }

                    Log("configured background image is not readable=\"{0}\"", configuredBackgroundImagePath);
                }

                return GetCurrentWallpaperPath();
            }

            // Expands environment variables and resolves relative image paths beside the .scr.
            private static string ResolveConfiguredImagePath(string configuredBackgroundImagePath)
            {
                string expandedPath = Environment.ExpandEnvironmentVariables(configuredBackgroundImagePath);
                return Path.IsPathRooted(expandedPath)
                    ? expandedPath
                    : Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), expandedPath);
            }

            // Computes the cover-scaled destination rectangle for drawing an image.
            private static Rectangle GetCoverDestination(Size destinationSize, Size sourceSize)
            {
                double scaleX = destinationSize.Width / (double)sourceSize.Width;
                double scaleY = destinationSize.Height / (double)sourceSize.Height;
                double scale = Math.Max(scaleX, scaleY);
                int width = (int)Math.Round(sourceSize.Width * scale);
                int height = (int)Math.Round(sourceSize.Height * scale);
                return new Rectangle(
                    (destinationSize.Width - width) / 2,
                    (destinationSize.Height - height) / 2,
                    width,
                    height);
            }

            // Finds the current Windows wallpaper file, with a TranscodedWallpaper fallback.
            private static string GetCurrentWallpaperPath()
            {
                StringBuilder path = new StringBuilder(260);
                if (SystemParametersInfo(SpiGetDesktopWallpaper, path.Capacity, path, 0) && path.Length > 0)
                {
                    string systemPath = path.ToString();
                    if (File.Exists(systemPath))
                    {
                        Log("wallpaper path from SystemParametersInfo=\"{0}\"", systemPath);
                        return systemPath;
                    }

                    Log("wallpaper path from SystemParametersInfo is not readable=\"{0}\"", systemPath);
                }
                else
                {
                    Log("SystemParametersInfo(SPI_GETDESKWALLPAPER) returned no readable path error={0}", Marshal.GetLastWin32Error());
                }

                string transcodedPath = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "Microsoft",
                    "Windows",
                    "Themes",
                    "TranscodedWallpaper");
                if (File.Exists(transcodedPath))
                {
                    Log("wallpaper path from TranscodedWallpaper fallback=\"{0}\"", transcodedPath);
                    return transcodedPath;
                }

                Log("transcoded wallpaper fallback is not readable=\"{0}\"", transcodedPath);
                return null;
            }

            // Samples a few bitmap pixels for diagnostics without logging the whole image.
            private static void LogBitmapPixels(Bitmap bitmap)
            {
                Color topLeft = bitmap.GetPixel(0, 0);
                Color center = bitmap.GetPixel(bitmap.Width / 2, bitmap.Height / 2);
                Color bottomRight = bitmap.GetPixel(Math.Max(0, bitmap.Width - 1), Math.Max(0, bitmap.Height - 1));

                Log(
                    "capture pixels top_left=#{0:X2}{1:X2}{2:X2} center=#{3:X2}{4:X2}{5:X2} bottom_right=#{6:X2}{7:X2}{8:X2}",
                    topLeft.R,
                    topLeft.G,
                    topLeft.B,
                    center.R,
                    center.G,
                    center.B,
                    bottomRight.R,
                    bottomRight.G,
                    bottomRight.B);
            }

            // Starts fade-out on every open saver form after user input.
            private void CloseAll(object sender, EventArgs e)
            {
                // Any input on any monitor should dismiss the entire screensaver,
                // not only the window that received the event.
                DimForm[] snapshot = forms.ToArray();
                foreach (DimForm form in snapshot)
                {
                    if (!form.IsDisposed)
                    {
                        form.StartExitFade();
                    }
                }
            }

            // Counts closed saver forms and exits the application when the last one closes.
            private void HandleFormClosed(object sender, FormClosedEventArgs e)
            {
                openForms--;
                if (openForms <= 0)
                {
                    ExitThread();
                }
            }

            // Locks Windows once the fade-in timer completes, then closes all saver windows.
            private void LockWorkstationAndClose(object sender, EventArgs e)
            {
                if (!settings.LockWorkstation)
                {
                    return;
                }

                if (locking)
                {
                    return;
                }

                locking = true;
                LockWorkStation();

                foreach (DimForm form in forms.ToArray())
                {
                    if (!form.IsDisposed)
                    {
                        form.Close();
                    }
                }
            }
        }

        private sealed class ScreenCapture
        {
            // Stores a prepared bitmap with the screen bounds it belongs to.
            public ScreenCapture(Bitmap bitmap, Rectangle bounds)
            {
                Bitmap = bitmap;
                Bounds = bounds;
            }

            public Bitmap Bitmap { get; private set; }

            public Rectangle Bounds { get; private set; }
        }

        private sealed class DimForm : Form
        {
            private const int WakeMovePixels = 6;

            private readonly Bitmap screenCapture;
            private readonly Stopwatch stopwatch = new Stopwatch();
            private readonly Timer timer = new Timer();
            private readonly int fadeDurationMs;
            private readonly int exitFadeDurationMs;
            private readonly float finalDarkness;
            private readonly bool showLockCountdown;
            private Point lastMousePosition;
            private bool ignoreInitialMouseMove = true;
            private bool exiting;
            private long exitStartedAtMs;
            private int currentDarkness;
            private int exitStartDarkness;
            private bool lockRequested;

            public event EventHandler ExitRequested;
            public event EventHandler LockRequested;

            // Initializes a borderless topmost form that displays and dims one monitor bitmap.
            public DimForm(Bitmap screenCapture, Rectangle bounds, int fadeDurationMs, int exitFadeDurationMs, int frameTimerIntervalMs, float finalDarkness, bool showLockCountdown)
            {
                this.screenCapture = screenCapture;
                this.fadeDurationMs = fadeDurationMs;
                this.exitFadeDurationMs = exitFadeDurationMs;
                this.finalDarkness = finalDarkness;
                this.showLockCountdown = showLockCountdown;

                AutoScaleMode = AutoScaleMode.None;
                BackColor = Color.Black;
                DoubleBuffered = true;
                FormBorderStyle = FormBorderStyle.None;
                KeyPreview = true;
                ShowInTaskbar = false;
                StartPosition = FormStartPosition.Manual;
                // Use SetBounds after the border style is removed, so the client
                // area matches the monitor rectangle exactly.
                SetBounds(bounds.X, bounds.Y, bounds.Width, bounds.Height);
                TopMost = true;
                WindowState = FormWindowState.Normal;

                timer.Interval = frameTimerIntervalMs;
                timer.Tick += HandleTimerTick;
            }

            // Adds tool-window style flags so the saver surface stays out of Alt-Tab.
            protected override CreateParams CreateParams
            {
                get
                {
                    // Tool windows stay out of Alt-Tab and behave more like a
                    // system-owned screensaver surface.
                    const int wsExToolWindow = 0x00000080;

                    CreateParams parameters = base.CreateParams;
                    parameters.ExStyle |= wsExToolWindow;
                    return parameters;
                }
            }

            // Starts animation timing once the form is visible.
            protected override void OnShown(EventArgs e)
            {
                base.OnShown(e);
                lastMousePosition = Cursor.Position;
                stopwatch.Start();
                UpdateFrame();
                timer.Start();
                Activate();
            }

            // Paints the frozen background, the dim overlay, and the lock countdown.
            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);

                // Draw the prepared background first, then dim that frozen image.
                e.Graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
                e.Graphics.PixelOffsetMode = PixelOffsetMode.Half;
                e.Graphics.DrawImage(screenCapture, ClientRectangle);

                if (currentDarkness > 0)
                {
                    using (SolidBrush dimBrush = new SolidBrush(Color.FromArgb(currentDarkness, Color.Black)))
                    {
                        e.Graphics.FillRectangle(dimBrush, ClientRectangle);
                    }
                }

                PaintLockCountdown(e.Graphics);
            }

            // Dismisses the saver after meaningful mouse movement.
            protected override void OnMouseMove(MouseEventArgs e)
            {
                base.OnMouseMove(e);

                // Ignore the first mouse move that Windows often sends while the
                // form is being shown. After that, a small threshold prevents tiny
                // sensor jitter from immediately dismissing the saver.
                Point currentPosition = Cursor.Position;
                bool movedFarEnough =
                    Math.Abs(currentPosition.X - lastMousePosition.X) > WakeMovePixels ||
                    Math.Abs(currentPosition.Y - lastMousePosition.Y) > WakeMovePixels;
                lastMousePosition = currentPosition;

                if (ignoreInitialMouseMove)
                {
                    ignoreInitialMouseMove = false;
                    return;
                }

                if (movedFarEnough)
                {
                    RequestExit();
                }
            }

            // Dismisses the saver on mouse button input.
            protected override void OnMouseDown(MouseEventArgs e)
            {
                base.OnMouseDown(e);
                RequestExit();
            }

            // Dismisses the saver on keyboard input.
            protected override void OnKeyDown(KeyEventArgs e)
            {
                base.OnKeyDown(e);
                RequestExit();
            }

            // Stops animation resources and disposes the screen bitmap when the form closes.
            protected override void OnFormClosed(FormClosedEventArgs e)
            {
                timer.Stop();
                timer.Tick -= HandleTimerTick;
                timer.Dispose();
                screenCapture.Dispose();
                base.OnFormClosed(e);
            }

            // Draws the remaining seconds before lock in the lower-right corner.
            private void PaintLockCountdown(Graphics graphics)
            {
                if (!showLockCountdown || exiting || lockRequested)
                {
                    return;
                }

                long remainingMs = fadeDurationMs - stopwatch.ElapsedMilliseconds;
                int remainingSeconds = remainingMs <= 0
                    ? 0
                    : (int)Math.Ceiling(remainingMs / 1000d);
                int fontHeight = ClientSize.Height / 18;
                int margin = ClientSize.Height / 32;

                fontHeight = Math.Max(28, Math.Min(72, fontHeight));
                margin = Math.Max(24, Math.Min(72, margin));

                using (Font font = new Font("Segoe UI", fontHeight, FontStyle.Bold, GraphicsUnit.Pixel))
                using (StringFormat format = new StringFormat { Alignment = StringAlignment.Far, LineAlignment = StringAlignment.Far })
                using (SolidBrush shadowBrush = new SolidBrush(Color.Black))
                using (SolidBrush textBrush = new SolidBrush(Color.FromArgb(245, 245, 245)))
                {
                    string text = remainingSeconds.ToString(CultureInfo.InvariantCulture);
                    RectangleF textRect = new RectangleF(
                        margin,
                        margin,
                        Math.Max(1, ClientSize.Width - margin * 2),
                        Math.Max(1, ClientSize.Height - margin * 2));
                    RectangleF shadowRect = textRect;
                    shadowRect.Offset(2, 2);

                    graphics.DrawString(text, font, shadowBrush, shadowRect, format);
                    graphics.DrawString(text, font, textBrush, textRect, format);
                }
            }

            // Advances the fade animation on each timer tick.
            private void HandleTimerTick(object sender, EventArgs e)
            {
                UpdateFrame();
            }

            // Computes the current dim level and requests lock or close when a fade completes.
            private void UpdateFrame()
            {
                float progress;
                float eased;

                if (exiting)
                {
                    progress = Clamp((stopwatch.ElapsedMilliseconds - exitStartedAtMs) / (float)exitFadeDurationMs, 0f, 1f);
                    eased = EaseInOutCubic(progress);
                    currentDarkness = Math.Max(0, (int)Math.Round(exitStartDarkness * (1d - eased)));
                    Invalidate();

                    if (progress >= 1f)
                    {
                        Close();
                    }

                    return;
                }

                progress = Clamp(stopwatch.ElapsedMilliseconds / (float)fadeDurationMs, 0f, 1f);
                eased = EaseInOutCubic(progress);
                currentDarkness = Math.Min(255, (int)Math.Round(255 * finalDarkness * eased));
                Invalidate();

                if (progress >= 1f)
                {
                    timer.Stop();
                    RequestLock();
                }
            }

            // Begins the one-second fade-out from the current darkness.
            public void StartExitFade()
            {
                if (exiting)
                {
                    return;
                }

                if (!stopwatch.IsRunning)
                {
                    stopwatch.Start();
                }

                exiting = true;
                exitStartedAtMs = stopwatch.ElapsedMilliseconds;
                exitStartDarkness = currentDarkness;

                if (!timer.Enabled)
                {
                    timer.Start();
                }

                UpdateFrame();
            }

            // Raises the shared exit request or starts local fade-out if no handler is attached.
            private void RequestExit()
            {
                if (exiting)
                {
                    return;
                }

                EventHandler handler = ExitRequested;
                if (handler != null)
                {
                    handler(this, EventArgs.Empty);
                }
                else
                {
                    StartExitFade();
                }
            }

            // Raises the shared lock request once the fade-in delay has elapsed.
            private void RequestLock()
            {
                if (exiting || lockRequested)
                {
                    return;
                }

                lockRequested = true;
                EventHandler handler = LockRequested;
                if (handler != null)
                {
                    handler(this, EventArgs.Empty);
                }
                else
                {
                    Close();
                }
            }

            // Constrains a floating-point value to an inclusive range.
            private static float Clamp(float value, float min, float max)
            {
                if (value < min)
                {
                    return min;
                }

                if (value > max)
                {
                    return max;
                }

                return value;
            }

            // Smooths a 0..1 fade progress value with cubic easing.
            private static float EaseInOutCubic(float value)
            {
                // Starts and ends gently, so the screen does not abruptly jump
                // darker at the beginning or snap into black at the end.
                return value < 0.5f
                    ? 4f * value * value * value
                    : 1f - (float)Math.Pow(-2f * value + 2f, 3d) / 2f;
            }
        }

        private sealed class PreviewForm : Form
        {
            private readonly IntPtr parentHandle;
            private readonly Timer timer = new Timer { Interval = 33 };
            private float phase;

            // Initializes the lightweight preview surface hosted by Screen Saver Settings.
            public PreviewForm(IntPtr parentHandle)
            {
                this.parentHandle = parentHandle;

                AutoScaleMode = AutoScaleMode.None;
                BackColor = Color.Black;
                DoubleBuffered = true;
                FormBorderStyle = FormBorderStyle.None;
                ShowInTaskbar = false;
                StartPosition = FormStartPosition.Manual;

                timer.Tick += HandleTimerTick;
            }

            // Reparents the preview form into the Windows preview host and starts animation.
            protected override void OnLoad(EventArgs e)
            {
                base.OnLoad(e);

                if (parentHandle != IntPtr.Zero)
                {
                    // Screen Saver Settings passes a tiny child-window handle for
                    // preview mode. Reparent this form so Windows can host it there.
                    SetParent(Handle, parentHandle);
                    SetWindowLongPtr(Handle, GwlStyle, new IntPtr(WsChild | WsVisible));
                    Bounds = GetParentClientRectangle(parentHandle);
                }

                timer.Start();
            }

            // Paints the animated preview gradient and dim overlay.
            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);

                // Preview mode shows a lightweight animated dim preview inside the
                // small host window provided by Screen Saver Settings.
                using (LinearGradientBrush background = new LinearGradientBrush(
                    ClientRectangle,
                    Color.FromArgb(36, 48, 62),
                    Color.FromArgb(8, 10, 14),
                    LinearGradientMode.ForwardDiagonal))
                {
                    e.Graphics.FillRectangle(background, ClientRectangle);
                }

                int alpha = 72 + (int)(96 * (0.5f + Math.Sin(phase * Math.PI * 2.0) * 0.5f));
                using (SolidBrush dimBrush = new SolidBrush(Color.FromArgb(alpha, Color.Black)))
                {
                    e.Graphics.FillRectangle(dimBrush, ClientRectangle);
                }
            }

            // Stops and disposes preview animation resources.
            protected override void OnFormClosed(FormClosedEventArgs e)
            {
                timer.Stop();
                timer.Tick -= HandleTimerTick;
                timer.Dispose();
                base.OnFormClosed(e);
            }

            // Advances the preview animation phase on each timer tick.
            private void HandleTimerTick(object sender, EventArgs e)
            {
                phase = (phase + 0.012f) % 1f;
                Invalidate();
            }
        }

        private enum ScreenSaverCommandKind
        {
            Run,
            Preview,
            Configure,
        }

        private sealed class ScreenSaverCommand
        {
            // Stores the parsed screen saver command and optional preview parent handle.
            public ScreenSaverCommand(ScreenSaverCommandKind kind, IntPtr previewParentHandle)
            {
                Kind = kind;
                PreviewParentHandle = previewParentHandle;
            }

            public ScreenSaverCommandKind Kind { get; private set; }
            public IntPtr PreviewParentHandle { get; private set; }

            // Parses Windows screen saver switches such as /s, /p, and /c.
            public static ScreenSaverCommand Parse(string[] args)
            {
                if (args.Length == 0)
                {
                    // Double-clicking a .scr normally opens configuration.
                    return new ScreenSaverCommand(ScreenSaverCommandKind.Configure, IntPtr.Zero);
                }

                string first = args[0].Trim().ToLowerInvariant();
                if (first.StartsWith("/p", StringComparison.Ordinal) || first.StartsWith("-p", StringComparison.Ordinal))
                {
                    // Windows may pass preview as "/p 12345" or "/p:12345".
                    string handleText = args.Length > 1 ? args[1] : first.Substring(2).Trim(':');
                    return new ScreenSaverCommand(ScreenSaverCommandKind.Preview, ParseHandle(handleText));
                }

                if (first.StartsWith("/c", StringComparison.Ordinal) || first.StartsWith("-c", StringComparison.Ordinal))
                {
                    return new ScreenSaverCommand(ScreenSaverCommandKind.Configure, IntPtr.Zero);
                }

                return new ScreenSaverCommand(ScreenSaverCommandKind.Run, IntPtr.Zero);
            }

            // Converts preview parent handle text to an IntPtr.
            private static IntPtr ParseHandle(string text)
            {
                long handle;
                return long.TryParse(text, out handle)
                    ? new IntPtr(handle)
                    : IntPtr.Zero;
            }
        }

        private const int GwlStyle = -16;
        private const int WsChild = 0x40000000;
        private const int WsVisible = 0x10000000;

        // Reparents the preview form into the host window supplied by Screen Saver Settings.
        [DllImport("user32.dll")]
        private static extern IntPtr SetParent(IntPtr childHandle, IntPtr parentHandle);

        // Adjusts the Win32 cursor display counter.
        [DllImport("user32.dll")]
        private static extern int ShowCursor(bool show);

        // Locks the current Windows workstation.
        [DllImport("user32.dll")]
        private static extern bool LockWorkStation();

        // Returns the id of the thread running the saver.
        [DllImport("kernel32.dll")]
        private static extern int GetCurrentThreadId();

        // Returns the desktop object associated with a thread.
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr GetThreadDesktop(int threadId);

        // Reads metadata such as the desktop name from a Win32 user object.
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool GetUserObjectInformation(IntPtr objectHandle, int index, StringBuilder information, int length, out int needed);

        // Reads system parameters, including the configured desktop wallpaper path.
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool SystemParametersInfo(int action, int parameter, StringBuilder value, int winIni);

        // Enables legacy process-wide DPI awareness on older Windows versions.
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetProcessDPIAware();

        // Enables modern DPI awareness when the Windows build supports it.
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetProcessDpiAwarenessContext(IntPtr dpiContext);

        // Updates preview-window style flags after reparenting.
        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)]
        private static extern IntPtr SetWindowLongPtr(IntPtr windowHandle, int index, IntPtr newLong);

        // Reads the client rectangle of the preview host window.
        [DllImport("user32.dll")]
        private static extern bool GetClientRect(IntPtr windowHandle, out NativeRect rect);

        // Reads the preview host client rectangle, falling back to a small default size.
        private static Rectangle GetParentClientRectangle(IntPtr parentHandle)
        {
            NativeRect rect;
            return GetClientRect(parentHandle, out rect)
                ? new Rectangle(0, 0, rect.Right - rect.Left, rect.Bottom - rect.Top)
                : new Rectangle(0, 0, 160, 100);
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeRect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }
    }
}
