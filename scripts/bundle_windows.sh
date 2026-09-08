#!/usr/bin/env bash
#
# bundle_windows.sh — assemble a standalone Windows distribution of the
# baichuan / dashboard executables plus every DLL and GTK runtime asset they
# need, so the result runs on a clean Windows 10/11 machine with nothing
# installed (no MSYS2 required on the target).
#
# RUN THIS FROM THE MSYS2 *UCRT64* SHELL, after a successful build:
#     cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build -j
#     ./scripts/bundle_windows.sh
#
# Result: dist/baichuan-win64/  (and a zip alongside it, if `zip` is present).
# Copy that folder to the target machine and run dashboard.exe / baichuan.exe.
#
# Usage:
#   ./scripts/bundle_windows.sh [BUILD_DIR] [OUT_DIR]
#     BUILD_DIR   directory containing the built .exe files   (default: build)
#     OUT_DIR     staging directory for the bundle            (default: dist/baichuan-win64)

set -euo pipefail

# --- Locate ourselves / the project root ------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
OUT_DIR="${2:-${PROJECT_ROOT}/dist/baichuan-win64}"

# --- Sanity checks ----------------------------------------------------------
if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    echo "WARNING: MSYSTEM is '${MSYSTEM:-<unset>}', expected 'UCRT64'." >&2
    echo "         Run this from the 'MSYS2 UCRT64' shell so the correct" >&2
    echo "         DLLs and GTK assets are picked up." >&2
fi

# The UCRT64 prefix that everything is copied from. `ldd` reports DLL paths
# under this prefix; anything outside it is a system DLL we must NOT ship.
UCRT_PREFIX="${MINGW_PREFIX:-/ucrt64}"

for tool in ldd gdk-pixbuf-query-loaders glib-compile-schemas; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool '$tool' not found on PATH." >&2
        echo "       Install the matching MSYS2 packages (see BUILD.md)." >&2
        exit 1
    fi
done

EXES=(dashboard.exe baichuan.exe)
found_any=0
for exe in "${EXES[@]}"; do
    [[ -f "${BUILD_DIR}/${exe}" ]] && found_any=1
done
if [[ "$found_any" -eq 0 ]]; then
    echo "ERROR: no built executables found in '${BUILD_DIR}'." >&2
    echo "       Build first:  cmake --build build -j" >&2
    exit 1
fi

echo ">> Project root : ${PROJECT_ROOT}"
echo ">> Build dir    : ${BUILD_DIR}"
echo ">> Output dir   : ${OUT_DIR}"
echo ">> UCRT prefix  : ${UCRT_PREFIX}"

# --- Fresh staging directory ------------------------------------------------
rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

# --- 1. Copy the executables ------------------------------------------------
echo ">> Copying executables..."
for exe in "${EXES[@]}"; do
    if [[ -f "${BUILD_DIR}/${exe}" ]]; then
        cp -v "${BUILD_DIR}/${exe}" "${OUT_DIR}/"
    fi
done

# --- 2. Copy dependent DLLs (recursively via ldd) ---------------------------
# ldd resolves the full transitive DLL graph. We keep only DLLs living under
# the UCRT64 prefix; the rest are Windows system DLLs (kernel32, ws2_32, ...)
# that already exist on every target machine.
echo ">> Resolving and copying dependent DLLs..."
copied_dlls=0
for exe in "${EXES[@]}"; do
    [[ -f "${OUT_DIR}/${exe}" ]] || continue
    # ldd lines look like:  libfoo.dll => /ucrt64/bin/libfoo.dll (0x...)
    while read -r dll_path; do
        [[ -n "$dll_path" ]] || continue
        dest="${OUT_DIR}/$(basename "$dll_path")"
        if [[ ! -f "$dest" ]]; then
            cp "$dll_path" "$dest"
            copied_dlls=$((copied_dlls + 1))
        fi
    done < <(ldd "${OUT_DIR}/${exe}" \
                | grep -iF "${UCRT_PREFIX}" \
                | awk '{print $3}' \
                | grep -iE '\.dll$' || true)
done
echo "   copied ${copied_dlls} DLL(s)"

# --- 3. gdk-pixbuf loaders (needed to render any icon / image) --------------
# Ship the loader DLLs and a loaders.cache with paths RELATIVE to the bundle
# so it resolves correctly on the target machine.
echo ">> Bundling gdk-pixbuf loaders..."
PIXBUF_MODDIR_SRC="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)"
if [[ -n "${PIXBUF_MODDIR_SRC}" && -d "${PIXBUF_MODDIR_SRC}" ]]; then
    # Mirror the standard layout: lib/gdk-pixbuf-2.0/2.10.0/loaders/
    PIXBUF_REL="lib/gdk-pixbuf-2.0/2.10.0"
    mkdir -p "${OUT_DIR}/${PIXBUF_REL}/loaders"
    cp "${PIXBUF_MODDIR_SRC}"/*.dll "${OUT_DIR}/${PIXBUF_REL}/loaders/" 2>/dev/null || true
    # Regenerate the cache, rewriting absolute loader paths to relative ones.
    gdk-pixbuf-query-loaders "${OUT_DIR}/${PIXBUF_REL}/loaders/"*.dll \
        | sed "s|${OUT_DIR}/||g" \
        > "${OUT_DIR}/${PIXBUF_REL}/loaders.cache"
    echo "   loaders.cache written (${PIXBUF_REL}/loaders.cache)"
else
    echo "   WARNING: gdk-pixbuf module dir not found; icons may not render." >&2
fi

# --- 4. GSettings schemas (GTK aborts at startup without these) -------------
echo ">> Compiling GSettings schemas..."
SCHEMA_SRC="${UCRT_PREFIX}/share/glib-2.0/schemas"
if [[ -d "${SCHEMA_SRC}" ]]; then
    mkdir -p "${OUT_DIR}/share/glib-2.0/schemas"
    cp "${SCHEMA_SRC}"/*.xml           "${OUT_DIR}/share/glib-2.0/schemas/" 2>/dev/null || true
    cp "${SCHEMA_SRC}"/gschema.dtd     "${OUT_DIR}/share/glib-2.0/schemas/" 2>/dev/null || true
    glib-compile-schemas "${OUT_DIR}/share/glib-2.0/schemas"
    echo "   gschemas.compiled written"
else
    echo "   WARNING: schema source dir not found: ${SCHEMA_SRC}" >&2
fi

# --- 5. Icon themes (Adwaita + hicolor) -------------------------------------
echo ">> Copying icon themes..."
mkdir -p "${OUT_DIR}/share/icons"
for theme in Adwaita hicolor; do
    if [[ -d "${UCRT_PREFIX}/share/icons/${theme}" ]]; then
        cp -r "${UCRT_PREFIX}/share/icons/${theme}" "${OUT_DIR}/share/icons/"
    fi
done

# --- 6. GTK settings so it uses the bundled theme ---------------------------
mkdir -p "${OUT_DIR}/share/gtk-3.0"
cat > "${OUT_DIR}/share/gtk-3.0/settings.ini" <<'INI'
[Settings]
gtk-theme-name=Adwaita
gtk-icon-theme-name=Adwaita
gtk-font-name=Segoe UI 10
INI

# --- 7. Bundled config template ---------------------------------------------
if [[ -f "${PROJECT_ROOT}/config.json" ]]; then
    cp "${PROJECT_ROOT}/config.json" "${OUT_DIR}/config.example.json"
fi

# --- Summary + optional zip -------------------------------------------------
echo
echo ">> Bundle assembled at: ${OUT_DIR}"
du -sh "${OUT_DIR}" 2>/dev/null || true

if command -v zip >/dev/null 2>&1; then
    ZIP_PATH="${OUT_DIR}.zip"
    echo ">> Creating zip: ${ZIP_PATH}"
    ( cd "$(dirname "${OUT_DIR}")" && rm -f "$(basename "${ZIP_PATH}")" \
        && zip -qr "$(basename "${ZIP_PATH}")" "$(basename "${OUT_DIR}")" )
    echo ">> Zip ready: ${ZIP_PATH}"
else
    echo ">> (install the 'zip' package to also produce a .zip)"
fi

echo ">> Done. Copy the folder (or zip) to the target and run dashboard.exe."
