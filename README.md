# Dim Screensaver

Windows screen saver experiments that dim the current desktop over 10 seconds.
Both implementations update opacity at about 15 frames per second.

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
The mouse cursor is hidden before the fade starts and restored on exit.

## Windows Setup

Configure Dim Screensaver as a normal Windows screen saver, and set its wait time earlier than the display power-off timeout. For example, start the screen saver after 4 minutes, then configure Windows power settings to turn the display off later.

That way the saver gently dims the desktop first, and Windows turns the physical display off only after the later power-management timeout.

![Windows Screen Saver Settings](assets/screen-saver-settings.png)
