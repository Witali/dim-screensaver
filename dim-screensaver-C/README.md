# Dim Screensaver C

Native Win32 implementation of Dim Screensaver.

It opens a borderless topmost black window over all monitors. The window starts fully transparent, then fades toward opacity over 10 seconds.
Opacity is updated at about 15 frames per second.

## Build

Install the Visual Studio C/C++ workload, then run:

```powershell
.\build.ps1
```

The screen saver is produced at:

```text
publish\DimScreensaverC.scr
```

## Try It

```powershell
.\publish\DimScreensaverC.scr /s
```

Move the mouse, click, or press any key to fade it out over 1 second and close it.
The mouse cursor is hidden before the fade starts and restored on exit.
