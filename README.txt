=====================================================
Retro TV Minimize & Thanos Snap Close (Standalone App)
=====================================================

Zero dependencies on Windhawk! This is your own standalone personal utility.

FILES IN THIS FOLDER:
- RetroThanosApp.exe     : The lightweight background launcher app (90 KB).
- RetroThanosEngine.dll  : The animation engine and system hook (88 KB).
- build.bat              : One-click recompiler script.
- RetroThanosApp.cpp     : C++ source for the launcher and system tray.
- RetroThanosEngine.cpp  : C++ source for CRT TV and Thanos Snap animations.

-----------------------------------------------------
HOW TO USE:
-----------------------------------------------------
1. Double-click "RetroThanosApp.exe" to launch it.
   - It runs quietly in the background with zero terminal windows.
   - A notification bubble will confirm it is active.

2. Animations:
   - Minimize any window (—) -> Watch the Retro CRT TV power-down effect!
   - Close any window (X)    -> Watch the Thanos Snap particle disintegration!
   - Restore & Open windows  -> Native Windows performance remains untouched.

-----------------------------------------------------
WHERE TO FIND THE SYSTEM TRAY CONTROLS:
-----------------------------------------------------
Look at the bottom-right corner of your screen (near the clock, volume, and Wi-Fi):

1. Click the little upward arrow ( ^ ) next to your clock.
   Windows places background apps inside this hidden icons menu by default.
   You will see the app icon there (looks like a small application window).

   TIP: You can click and drag that icon directly onto your main taskbar so it
   always stays visible next to your clock!

2. Right-click (or left-click) the icon to open the controls menu:
   - "Enabled"             : Click to pause or resume animations anytime.
   - "Animation Speed"     : Choose between:
                             * Cinematic Slow (1000 ms)
                             * Normal (750 ms) [Default]
                             * Snappy Fast (450 ms)
   - "Particle Size"       : Choose between:
                             * Fine Dust (2 px)
                             * Standard (4 px) [Default]
                             * Chunky Blocks (6 px)
   - "Exit"                : Cleanly unhooks everything and shuts down the app.

-----------------------------------------------------
HOW TO START AUTOMATICALLY WITH WINDOWS:
-----------------------------------------------------
1. Press `Win + R` on your keyboard.
2. Type `shell:startup` and press Enter.
3. Right-click "RetroThanosApp.exe" in this folder -> "Show more options" -> "Create shortcut".
4. Move or paste that shortcut into the Startup folder that opened!

Now your custom animations will automatically run every time your PC turns on.
