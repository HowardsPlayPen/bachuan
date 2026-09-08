# Building on Windows 11

This project builds on Windows using the **MSYS2 UCRT64** toolchain (GCC /
MinGW). That is the only supported Windows toolchain — the code is written for
the MinGW-GCC family (`getopt_long`, GCC warning flags, POSIX-ish idioms) and is
**not** an MSVC / Visual Studio build. Everything you need — compiler, CMake, and
every C-library dependency — comes from the one MSYS2 package manager, so there
is no Visual Studio, Windows SDK, vcpkg, or Conan to install.

> The same source tree still builds on Linux unchanged. All Windows-specific
> code lives behind `#ifdef _WIN32` or in the `src/utils/net_compat.*` shim.

---

## What you need (the minimum)

| Tool | Where it comes from |
|------|---------------------|
| GCC / g++ / binutils | `mingw-w64-ucrt-x86_64-toolchain` |
| CMake | `mingw-w64-ucrt-x86_64-cmake` |
| Ninja | `mingw-w64-ucrt-x86_64-ninja` |
| pkg-config | `mingw-w64-ucrt-x86_64-pkgconf` |
| GTK3 + Cairo | `mingw-w64-ucrt-x86_64-gtk3` (pulls in Cairo/Pango/GLib) |
| FFmpeg | `mingw-w64-ucrt-x86_64-ffmpeg` |
| OpenSSL | `mingw-w64-ucrt-x86_64-openssl` |
| libxml2 | `mingw-w64-ucrt-x86_64-libxml2` |
| libjpeg-turbo | `mingw-w64-ucrt-x86_64-libjpeg-turbo` |
| Winsock (`ws2_32`) | part of MinGW — **no install**, CMake links it automatically |

**Not needed:** Visual Studio / MSVC, a standalone Windows SDK, vcpkg, or Conan.

Disk footprint: MSYS2 plus this package set is roughly **3–4 GB**.

---

## Step 1 — Install MSYS2

1. Download the installer from <https://www.msys2.org> and run it (accept the
   defaults; it installs to `C:\msys64` by default).
2. From the Start menu open the shell named **"MSYS2 UCRT64"**.

   > This matters: MSYS2 ships several shells (MSYS, MINGW64, UCRT64, CLANG64).
   > Use the **UCRT64** one. You can confirm you're in the right shell with:
   > ```bash
   > echo "$MSYSTEM"   # should print: UCRT64
   > ```

## Step 2 — Update the base system

```bash
pacman -Syu
```

If it tells you to close the shell, close it, reopen **MSYS2 UCRT64**, and run
`pacman -Syu` once more to finish.

## Step 3 — Install the toolchain and dependencies

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-gtk3 \
  mingw-w64-ucrt-x86_64-cairo \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-libxml2 \
  mingw-w64-ucrt-x86_64-libjpeg-turbo
```

(When the `toolchain` group asks which members to install, just press Enter to
take them all.)

## Step 4 — Get the source and build

From the **MSYS2 UCRT64** shell:

```bash
# clone (or cd into an existing checkout)
git clone https://github.com/HowardsPlayPen/bachuan.git
cd bachuan

cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/dashboard.exe` (multi-camera dashboard) and
`build/baichuan.exe` (single-camera viewer).

## Step 5 — Run it

The quickest way to run — **from the same UCRT64 shell**, where all the GTK DLLs
and assets are already on `PATH`:

```bash
./build/dashboard.exe --help
./build/dashboard.exe --config config.json
```

If `dashboard.exe --help` prints the usage text (including the documentation
URL) and, with a config, a window opens and renders the camera grid, the build
is good.

---

## Producing a standalone bundle (run on any Windows PC)

The above runs inside the UCRT64 shell. To run on a **clean Windows 10/11
machine with nothing installed**, you must ship the DLLs plus the GTK runtime
assets (pixbuf loaders, GSettings schemas, an icon theme). The script
`scripts/bundle_windows.sh` does all of that.

From the **MSYS2 UCRT64** shell, after building:

```bash
./scripts/bundle_windows.sh
```

It creates `dist/baichuan-win64/` (and a `.zip` if the `zip` package is
installed) containing the executables and every runtime dependency. Copy that
folder to the target machine and run `dashboard.exe` directly — no MSYS2
required there.

What the script bundles:

- The `dashboard.exe` / `baichuan.exe` binaries.
- Every dependent DLL under the UCRT64 prefix, resolved transitively with `ldd`
  (system DLLs like `kernel32`/`ws2_32` are intentionally left out — they exist
  on every Windows machine).
- gdk-pixbuf image loaders + a `loaders.cache` with bundle-relative paths.
- Compiled GSettings schemas (GTK aborts at startup without these).
- The Adwaita + hicolor icon themes and a `gtk-3.0/settings.ini`.

If `zip` is not installed and you want the `.zip`:

```bash
pacman -S --needed zip
```

---

## Windows notes / differences from Linux

- **Control interface is TCP-only on Windows.** The Unix-domain-socket control
  path is compiled out (`#ifndef _WIN32`). Configure a TCP control port instead
  of a `control.unix` path, e.g. test it with:
  ```bash
  echo '{"list":true}' | nc <host> 9100
  ```
- **Default config path** on Windows is `%APPDATA%\baichuan\config.json`
  (falling back to `%USERPROFILE%\.config\baichuan\config.json`). You can always
  pass `--config <path>` explicitly.
- **Shutdown:** Ctrl+C in the console exits cleanly. Closing the window also
  exits cleanly.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `echo "$MSYSTEM"` is not `UCRT64` | You opened the wrong shell — open **MSYS2 UCRT64**. |
| `cmake: command not found` | You skipped Step 3, or you're in the MSYS/MINGW64 shell instead of UCRT64. |
| `Package 'gtk+-3.0' not found` (pkg-config) | `mingw-w64-ucrt-x86_64-gtk3` didn't install — re-run Step 3. |
| Bundled `dashboard.exe` shows no icons / exits immediately on the target | The GTK assets weren't bundled — re-run `scripts/bundle_windows.sh` from the UCRT64 shell (it needs `gdk-pixbuf-query-loaders` and `glib-compile-schemas` on PATH). |
| `WSAPoll`/socket link errors | Ensure you configured fresh with CMake ≥ 3.16 from this tree — the `if(WIN32)` block links `ws2_32` and sets `_WIN32_WINNT=0x0A00`. |
