# Dim Screensaver

Windows screen saver that opens transparent black overlay windows over all monitors, then smoothly fades them toward opacity over 10 seconds.
Opacity is updated at about 15 frames per second.

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

Move the mouse, click, or press any key to fade it out over 1 second and close it.
The mouse cursor is hidden before the fade starts and restored on exit.

## Install

Copy `publish\DimScreensaver.scr` to `C:\Windows\System32`, then select **DimScreensaver** in Windows Screen Saver Settings.

You can also right-click the `.scr` file and choose **Install**.
