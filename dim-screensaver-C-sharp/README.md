# Dim Screensaver

Windows screen saver that captures the current desktop, then smoothly darkens that frozen image over 10 seconds.

## Build

Run the build script:

```powershell
.\build.ps1
```

The screen saver is produced at:

```text
publish\DimScreensaver.scr
```

The script uses the C# compiler included with .NET Framework on Windows, so it does not require the .NET SDK.

## Try It

Run the saver directly:

```powershell
.\publish\DimScreensaver.scr /s
```

Move the mouse, click, or press any key to close it.

## Install

Copy `publish\DimScreensaver.scr` to `C:\Windows\System32`, then select **DimScreensaver** in Windows Screen Saver Settings.

You can also right-click the `.scr` file and choose **Install**.
