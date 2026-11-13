#!/bin/bash
#
# PS5 Hackintosh — Descargar OpenCore + kexts + drivers
#
# Descarga todos los componentes necesarios de OpenCore para el Hackintosh.
# Los kexts y drivers se colocan en el directorio de build.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD="$PROJECT_DIR/build"
OC_DIR="$BUILD/opencore"

mkdir -p "$OC_DIR/EFI/OC/"{ACPI,Drivers,Kexts,Tools}
mkdir -p "$OC_DIR/EFI/BOOT"
mkdir -p "$BUILD/downloads"

DL="$BUILD/downloads"
source "$SCRIPT_DIR/ziputil.sh"

echo "╔══════════════════════════════════════════════════╗"
echo "║  PS5 Hackintosh — Descargar OpenCore + Kexts    ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# ── Función para descargar release de GitHub ───────────
# gh_latest REPO PATTERN OUT CHECK...: every CHECK (kext:NAME or an entry regex) must pass (see tools/ziputil.sh); a cached zip is checked too
gh_latest() {
    local repo="$1"
    local pattern="$2"
    local out="$3"
    shift 3

    if [ -f "$out" ] && zip_ok "$out" "$@"; then
        echo "  [✓] $(basename "$out") ya descargado (verificado)"
        return 0
    fi

    echo "  [↓] Descargando $(basename "$out") de $repo..."
    local url
    url=$(curl -sL "https://api.github.com/repos/$repo/releases/latest" \
        | grep -o "\"browser_download_url\": *\"[^\"]*${pattern}[^\"]*\"" \
        | head -1 \
        | sed 's/"browser_download_url": *"//;s/"//')

    if [ -z "$url" ]; then
        echo "  [!] No se pudo encontrar $pattern en $repo"
        return 1
    fi

    fetch_zip "$url" "$out" "$@"
}

# ── OpenCore ───────────────────────────────────────────
echo "OpenCore bootloader:"
OC_FILES=("^X64/EFI/BOOT/BOOTx64\.efi$" "^X64/EFI/OC/OpenCore\.efi$" "/OpenRuntime\.efi$" "/OpenHfsPlus\.efi$")
gh_latest "acidanthera/OpenCorePkg" "RELEASE.zip" "$DL/OpenCore-RELEASE.zip" "${OC_FILES[@]}"

if [ -f "$DL/OpenCore-RELEASE.zip" ]; then
    OC_TMP=$(mktemp -d)
    unzip -qo "$DL/OpenCore-RELEASE.zip" -d "$OC_TMP"

    # Bootloader
    cp "$OC_TMP/X64/EFI/BOOT/BOOTx64.efi" "$OC_DIR/EFI/BOOT/" 2>/dev/null || true
    cp "$OC_TMP/X64/EFI/OC/OpenCore.efi" "$OC_DIR/EFI/OC/" 2>/dev/null || true

    # Drivers
    for drv in OpenRuntime.efi OpenHfsPlus.efi ResetNvramEntry.efi OpenUsbKbDxe.efi; do
        find "$OC_TMP" -name "$drv" -exec cp {} "$OC_DIR/EFI/OC/Drivers/" \; 2>/dev/null
    done

    rm -rf "$OC_TMP"
    echo "  [✓] OpenCore extraído"
fi

# Build DEBUG (OC_DEBUG=1: clean_run.sh and ktty_run.sh use it, it logs the memory map and the ExitBootServices steps)
gh_latest "acidanthera/OpenCorePkg" "DEBUG.zip" "$DL/OpenCore-DEBUG.zip" "${OC_FILES[@]}"
if [ -f "$DL/OpenCore-DEBUG.zip" ]; then
    rm -rf "$BUILD/opencore-debug" && mkdir -p "$BUILD/opencore-debug"
    unzip -qo "$DL/OpenCore-DEBUG.zip" -d "$BUILD/opencore-debug"
    echo "  [✓] OpenCore DEBUG extraído"
fi

# ── Kexts ──────────────────────────────────────────────
echo ""
echo "Kexts:"

declare -A KEXTS=(
    ["acidanthera/Lilu"]="RELEASE.zip"
    ["acidanthera/VirtualSMC"]="RELEASE.zip"
    ["acidanthera/WhateverGreen"]="RELEASE.zip"
    ["acidanthera/AppleALC"]="RELEASE.zip"
    ["acidanthera/NVMeFix"]="RELEASE.zip"
    ["acidanthera/RestrictEvents"]="RELEASE.zip"
)

for repo in "${!KEXTS[@]}"; do
    name=$(basename "$repo")
    pattern="${KEXTS[$repo]}"
    dl_file="$DL/${name}.zip"

    expect=("kext:$name")                                                  # a complete kext: Info.plist and its executable
    if [ "$name" = VirtualSMC ]; then expect+=("kext:SMCProcessor"); fi      # make_baremetal_efi.py copies SMCProcessor from this zip
    gh_latest "$repo" "$pattern" "$dl_file" "${expect[@]}"

    if [ -f "$dl_file" ]; then
        KEXT_TMP=$(mktemp -d)
        unzip -qo "$dl_file" -d "$KEXT_TMP"

        # Copiar .kext encontrados
        find "$KEXT_TMP" -maxdepth 3 -name "*.kext" -exec cp -R {} "$OC_DIR/EFI/OC/Kexts/" \; 2>/dev/null
        rm -rf "$KEXT_TMP"
    fi
done

# SMCAMDProcessor's release holds three zips, the first being AMD.Power.Gadget.app.zip (no kext): a plain "zip" pattern took that one, so
# neither kext was ever copied. Fetch the two kext zips by name. (The bare-metal EFI, make_baremetal_efi.py, does not load either of them.)
for asset in SMCAMDProcessor.kext.zip AMDRyzenCPUPowerManagement.kext.zip; do
    gh_latest "trulyspinach/SMCAMDProcessor" "$asset" "$DL/$asset" "kext:${asset%.kext.zip}"
    KEXT_TMP=$(mktemp -d)
    unzip -qo "$DL/$asset" -d "$KEXT_TMP"
    find "$KEXT_TMP" -maxdepth 3 -name "*.kext" -exec cp -R {} "$OC_DIR/EFI/OC/Kexts/" \; 2>/dev/null
    rm -rf "$KEXT_TMP"
done
echo ""

# ── Copiar config.plist ────────────────────────────────
cp "$PROJECT_DIR/opencore/config.plist" "$OC_DIR/EFI/OC/config.plist"

# ── ACPI ───────────────────────────────────────────────
if ls "$BUILD/acpi/"*.aml 1>/dev/null 2>&1; then
    cp "$BUILD/acpi/"*.aml "$OC_DIR/EFI/OC/ACPI/"
fi

# ── Resumen ────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════"
echo "Contenido de OpenCore:"
echo ""
echo "EFI/BOOT/:"
ls "$OC_DIR/EFI/BOOT/" 2>/dev/null | sed 's/^/  /'
echo ""
echo "EFI/OC/Drivers/:"
ls "$OC_DIR/EFI/OC/Drivers/" 2>/dev/null | sed 's/^/  /'
echo ""
echo "EFI/OC/Kexts/:"
ls "$OC_DIR/EFI/OC/Kexts/" 2>/dev/null | sed 's/^/  /'
echo ""
echo "Todo listo en: $OC_DIR/"
echo ""
echo "Para crear la imagen OpenCore:"
echo "  ./tools/build_opencore_img.sh $OC_DIR OpenCore.img"
echo "═══════════════════════════════════════════════════"
