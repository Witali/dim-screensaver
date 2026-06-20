using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace DimScreensaver
{
    internal static class Program
    {
        private const int FadeDurationMs = 10000;
        private const float FinalDarkness = 0.94f;

        [STAThread]
        private static void Main(string[] args)
        {
            // Screen bounds and screenshots must be read in physical pixels.
            // Without this, Windows can DPI-virtualize the process and make the
            // saver look as if the monitor resolution changed.
            EnableDpiAwareness();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            // Windows launches .scr files with small command-line switches:
            // /s runs the saver, /p embeds a preview into Screen Saver Settings,
            // and /c opens configuration.
            ScreenSaverCommand command = ScreenSaverCommand.Parse(args);
            switch (command.Kind)
            {
                case ScreenSaverCommandKind.Preview:
                    Application.Run(new PreviewForm(command.PreviewParentHandle));
                    break;
                case ScreenSaverCommandKind.Configure:
                    MessageBox.Show(
                        "Dim Screensaver плавно затемняет текущий экран за 10 секунд.\n\n" +
                        "Чтобы установить его, соберите проект и скопируйте DimScreensaver.scr в папку Windows.",
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

        private static void RunScreensaver()
        {
            using (ScreensaverContext context = new ScreensaverContext())
            {
                Application.Run(context);
            }
        }

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

        private sealed class ScreensaverContext : ApplicationContext
        {
            private readonly List<DimForm> forms = new List<DimForm>();
            private int openForms;

            public ScreensaverContext()
            {
                // Capture before showing any saver windows. Once the forms are up,
                // the desktop below them is no longer visible to CopyFromScreen.
                List<ScreenCapture> captures = CaptureScreens();
                openForms = captures.Count;

                foreach (ScreenCapture capture in captures)
                {
                    DimForm form = new DimForm(capture.Bitmap, capture.Bounds, FadeDurationMs, FinalDarkness);
                    form.FormClosed += HandleFormClosed;
                    form.ExitRequested += CloseAll;
                    forms.Add(form);
                    form.Show();
                }
            }

            protected override void Dispose(bool disposing)
            {
                if (disposing)
                {
                    foreach (DimForm form in forms)
                    {
                        form.FormClosed -= HandleFormClosed;
                        form.ExitRequested -= CloseAll;
                        form.Dispose();
                    }
                }

                base.Dispose(disposing);
            }

            private static List<ScreenCapture> CaptureScreens()
            {
                List<ScreenCapture> captures = new List<ScreenCapture>();

                foreach (Screen screen in Screen.AllScreens)
                {
                    // Each monitor gets its own bitmap and its own borderless form.
                    // This avoids stitching screens together and handles negative
                    // coordinates used by monitors positioned left/above primary.
                    Rectangle bounds = screen.Bounds;
                    Bitmap bitmap = new Bitmap(bounds.Width, bounds.Height);

                    using (Graphics graphics = Graphics.FromImage(bitmap))
                    {
                        graphics.CopyFromScreen(bounds.Location, Point.Empty, bounds.Size, CopyPixelOperation.SourceCopy);
                    }

                    captures.Add(new ScreenCapture(bitmap, bounds));
                }

                return captures;
            }

            private void CloseAll(object sender, EventArgs e)
            {
                // Any input on any monitor should dismiss the entire screensaver,
                // not only the window that received the event.
                DimForm[] snapshot = forms.ToArray();
                foreach (DimForm form in snapshot)
                {
                    if (!form.IsDisposed)
                    {
                        form.Close();
                    }
                }
            }

            private void HandleFormClosed(object sender, FormClosedEventArgs e)
            {
                openForms--;
                if (openForms <= 0)
                {
                    ExitThread();
                }
            }
        }

        private sealed class ScreenCapture
        {
            // Stores one frozen desktop image together with the monitor rectangle
            // where that image should be displayed.
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
            private readonly Stopwatch stopwatch = Stopwatch.StartNew();
            private readonly Timer timer = new Timer { Interval = 16 };
            private readonly int fadeDurationMs;
            private readonly float finalDarkness;
            private Point lastMousePosition;
            private bool ignoreInitialMouseMove = true;

            public event EventHandler ExitRequested;

            public DimForm(Bitmap screenCapture, Rectangle bounds, int fadeDurationMs, float finalDarkness)
            {
                this.screenCapture = screenCapture;
                this.fadeDurationMs = fadeDurationMs;
                this.finalDarkness = finalDarkness;

                AutoScaleMode = AutoScaleMode.None;
                BackColor = Color.Black;
                Cursor.Hide();
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

                timer.Tick += HandleTimerTick;
                timer.Start();
            }

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

            protected override void OnShown(EventArgs e)
            {
                base.OnShown(e);
                lastMousePosition = Cursor.Position;
                Activate();
            }

            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);

                // Draw the captured desktop first, then fade a black layer over it.
                // The bitmap is already monitor-sized, so no smoothing is needed.
                e.Graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
                e.Graphics.PixelOffsetMode = PixelOffsetMode.Half;
                e.Graphics.DrawImage(screenCapture, ClientRectangle);

                float progress = Clamp(stopwatch.ElapsedMilliseconds / (float)fadeDurationMs, 0f, 1f);
                float eased = EaseInOutCubic(progress);
                int darkness = (int)Math.Round(255 * finalDarkness * eased);

                using (SolidBrush dimBrush = new SolidBrush(Color.FromArgb(darkness, Color.Black)))
                {
                    e.Graphics.FillRectangle(dimBrush, ClientRectangle);
                }
            }

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

            protected override void OnMouseDown(MouseEventArgs e)
            {
                base.OnMouseDown(e);
                RequestExit();
            }

            protected override void OnKeyDown(KeyEventArgs e)
            {
                base.OnKeyDown(e);
                RequestExit();
            }

            protected override void OnFormClosed(FormClosedEventArgs e)
            {
                timer.Stop();
                timer.Tick -= HandleTimerTick;
                timer.Dispose();
                screenCapture.Dispose();
                Cursor.Show();
                base.OnFormClosed(e);
            }

            private void HandleTimerTick(object sender, EventArgs e)
            {
                Invalidate();
            }

            private void RequestExit()
            {
                EventHandler handler = ExitRequested;
                if (handler != null)
                {
                    handler(this, EventArgs.Empty);
                }
            }

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

            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);

                // Preview mode cannot capture the real desktop reliably inside the
                // Settings dialog, so it shows a lightweight animated dim preview.
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

            protected override void OnFormClosed(FormClosedEventArgs e)
            {
                timer.Stop();
                timer.Tick -= HandleTimerTick;
                timer.Dispose();
                base.OnFormClosed(e);
            }

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
            public ScreenSaverCommand(ScreenSaverCommandKind kind, IntPtr previewParentHandle)
            {
                Kind = kind;
                PreviewParentHandle = previewParentHandle;
            }

            public ScreenSaverCommandKind Kind { get; private set; }
            public IntPtr PreviewParentHandle { get; private set; }

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

        [DllImport("user32.dll")]
        private static extern IntPtr SetParent(IntPtr childHandle, IntPtr parentHandle);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetProcessDPIAware();

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetProcessDpiAwarenessContext(IntPtr dpiContext);

        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)]
        private static extern IntPtr SetWindowLongPtr(IntPtr windowHandle, int index, IntPtr newLong);

        [DllImport("user32.dll")]
        private static extern bool GetClientRect(IntPtr windowHandle, out NativeRect rect);

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
