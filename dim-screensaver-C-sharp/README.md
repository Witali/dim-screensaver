# Dim Screensaver

Windows screen saver that loads a configured image, or the current Windows desktop wallpaper by default, opens borderless topmost windows over each monitor, then draws that image with an increasingly dark black layer.
The dimmed frame is redrawn at about 15 frames per second.

## Build

Run the build script:

```powershell
.\build.ps1
```

The screen saver is produced at:

```text
publish\DimScreensaver.scr
publish\DimScreensaver.ini
```

The script uses the C# compiler included with .NET Framework on Windows, so it does not require the .NET SDK.

## Settings

Keep `DimScreensaver.ini` next to `DimScreensaver.scr`:

```ini
FadeInSeconds=10
FadeOutSeconds=1
LockWorkstation=true
BackgroundImagePath=
```

Use `FadeInSeconds=10`, `20`, or `60` for 10 seconds, 20 seconds, or 1 minute before optional locking. `FadeOutSeconds` controls the fade-out after input. Set `LockWorkstation=false` to keep the screen dimmed without locking Windows. Set `BackgroundImagePath` to an absolute image path, or to a relative path next to `DimScreensaver.scr`; leave it empty to use the current Windows wallpaper. If the file is missing or a value cannot be read, the defaults are 10 seconds fade-in, 1 second fade-out, `LockWorkstation=true`, and no custom image.

The older `LockDelaySeconds` key is still accepted as an alias for `FadeInSeconds`.

## Diagnostics

The saver writes `DimScreensaver.log` next to the `.scr` file, or falls back to `%LOCALAPPDATA%\DimScreensaver\DimScreensaver.log` if that folder is not writable.

## Try It

Run the saver directly:

```powershell
.\publish\DimScreensaver.scr /s
```

Move the mouse, click, or press any key to fade it out over 1 second and close it.
The mouse cursor is hidden before the fade starts and restored on exit.
If there is no input during the configured dimming period, the saver calls Windows Lock Workstation after the fade completes.

## Install

Copy `publish\DimScreensaver.scr` and `publish\DimScreensaver.ini` to `C:\Windows\System32`, then select **DimScreensaver** in Windows Screen Saver Settings.

Leave the Windows **On resume, display logon screen** checkbox turned off. This saver performs its own delayed lock with `LockWorkstation=true`.

You can also right-click the `.scr` file and choose **Install**.
