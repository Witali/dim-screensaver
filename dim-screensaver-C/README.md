# Dim Screensaver C

Native Win32 implementation of Dim Screensaver.

It opens a borderless topmost black window over all monitors. The window starts fully transparent, then fades toward opacity over a configurable delay.
Opacity is updated at about 15 frames per second.

## Build

Install the Visual Studio C/C++ workload, then run:

```powershell
.\build.ps1
```

The screen saver is produced at:

```text
publish\DimScreensaver.scr
publish\DimScreensaver.ini
```

## Settings

Keep `DimScreensaver.ini` next to `DimScreensaver.scr`:

```ini
FadeInSeconds=10
FadeOutSeconds=1
LockWorkstation=true
```

Use `FadeInSeconds=10`, `20`, or `60` for 10 seconds, 20 seconds, or 1 minute before optional locking. `FadeOutSeconds` controls the fade-out after input. Set `LockWorkstation=false` to keep the screen dimmed without locking Windows. If the file is missing or a value cannot be read, the defaults are 10 seconds fade-in, 1 second fade-out, and `LockWorkstation=true`.

The older `LockDelaySeconds` key is still accepted as an alias for `FadeInSeconds`.

## Try It

```powershell
.\publish\DimScreensaver.scr /s
```

Move the mouse, click, or press any key to fade it out over 1 second and close it.
The mouse cursor is hidden before the fade starts and restored on exit.
If there is no input during the configured dimming period, the saver calls Windows Lock Workstation after the fade completes.
