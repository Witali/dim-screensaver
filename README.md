# Dim Screensaver

Windows screen saver experiments that smoothly dim the current desktop over 10 seconds.
Both implementations quantize opacity into 16 transparency levels.

Implementations:

- [`dim-screensaver-C`](dim-screensaver-C): native Win32 C version using a transparent layered black window.
- [`dim-screensaver-C-sharp`](dim-screensaver-C-sharp): C# / Windows Forms version using transparent black overlay windows.

## Build

```powershell
cd .\dim-screensaver-C-sharp
.\build.ps1
```

Or build the native C version:

```powershell
cd .\dim-screensaver-C
.\build.ps1
```

The C build is produced at:

```text
dim-screensaver-C\publish\DimScreensaverC.scr
```

The C# build is produced at:

```text
dim-screensaver-C-sharp\publish\DimScreensaver.scr
```

## Try It

```powershell
.\dim-screensaver-C\publish\DimScreensaverC.scr /s
```

Move the mouse, click, or press any key to fade it out over 1 second and close it.
