# Mouse Debounce Filter

A lightweight, zero-dependency Windows utility written in C that blocks accidental double-clicks caused by failing mouse switches. It uses a low-level Windows mouse hook (`WH_MOUSE_LL`) with 0% idle CPU overhead and lives in the system tray.

## How to Use

1. Run **`debounce.exe`** (it will request Administrator privileges so the mouse filter works across all applications and games).
2. A mouse icon will appear in your Windows System Tray:
   - **Click the tray icon** to view blocked click stats, enable/disable the filter, or toggle **Run on Startup (Admin)**.
3. To customize the debounce delay (default is `45ms` for Left, Right, Middle, Mouse4, and Mouse5), click **Edit Config (`config.ini`)** in the tray menu, save your changes, and click **Reload Config**. Set any button to `0` to disable filtering for that button.

## How to Compile From Source

If you prefer to compile the executable yourself instead of using the pre-built `debounce.exe`, everything needed is already included in the source folder (including a portable copy of the Tiny C Compiler):

1. Open the source folder containing `debounce.c`, `compile.bat`, and the `tcc` directory.
2. Double-click **`compile.bat`** (or run `"tcc\tcc.exe" debounce.c -luser32 -Wl,-subsystem=windows -o debounce.exe`).
3. A fresh `debounce.exe` will be generated immediately.
