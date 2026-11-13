#!/bin/bash
#
# PS5 Hackintosh — Instalar macOS en QEMU (en tu Mac)
#
# Lanza QEMU con OpenCore + recovery para instalar macOS
# en la imagen de disco. Esto se ejecuta en tu Mac, NO en PS5.
#
# Uso: ./tools/install_macos_qemu.sh [version]      (default monterey; the recovery made by create_macos_img.sh for that version)
#   NO_RECOVERY=1 boots the installed disk without a recovery image

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD="$PROJECT_DIR/build"

MACOS_IMG="$BUILD/macOS.img"
VERSION="${1:-monterey}"
case "$VERSION" in
    monterey|ventura|sonoma|sequoia) ;;
    *) echo "Error: versión '$VERSION' no soportada. Opciones: monterey, ventura, sonoma, sequoia"; exit 1;;
esac
RECOVERY_IMG="$BUILD/macOS-recovery-$VERSION.img"
# checked before anything is downloaded or launched: without it QEMU would start with no installer and no word about why
if [ "${NO_RECOVERY:-0}" != 1 ] && [ ! -f "$RECOVERY_IMG" ]; then
    echo "Error: no hay recovery de $VERSION ($RECOVERY_IMG). Ejecuta primero: ./tools/create_macos_img.sh $VERSION"
    echo "  (NO_RECOVERY=1 arranca el disco ya instalado sin recovery)"
    exit 1
fi
OC_QCOW2="$BUILD/OpenCore-OSX-KVM.qcow2"
OVMF_CODE="$BUILD/OVMF_CODE.fd"
OVMF_VARS="$BUILD/OVMF_VARS.fd"
OVMF_VARS_TMP="/tmp/ps5-hackintosh-OVMF_VARS.fd"

OC_URL="https://github.com/kholia/OSX-KVM/raw/master/OpenCore/OpenCore.qcow2"

echo "╔══════════════════════════════════════════════╗"
echo "║  PS5 Hackintosh — Instalar macOS (QEMU)     ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

# ── Descargar OpenCore para QEMU si no existe ─────────
if [ ! -f "$OC_QCOW2" ]; then
    echo "  Descargando OpenCore (OSX-KVM) para QEMU..."
    # -f + .part: an HTTP error page must not be kept as the qcow2 (the next run would take it as downloaded)
    curl -fL -o "$OC_QCOW2.part" "$OC_URL" || { rm -f "$OC_QCOW2.part"; echo "Error: no se pudo descargar $OC_URL"; exit 1; }
    mv "$OC_QCOW2.part" "$OC_QCOW2"
    echo "  OpenCore descargado: $(du -h "$OC_QCOW2" | cut -f1)"
fi

# ── Verificar artefactos ──────────────────────────────
MISSING=""
[ ! -f "$OVMF_CODE" ]   && MISSING="$MISSING OVMF_CODE.fd"
[ ! -f "$OVMF_VARS" ]   && MISSING="$MISSING OVMF_VARS.fd"
[ ! -f "$MACOS_IMG" ]    && MISSING="$MISSING macOS.img"

if [ -n "$MISSING" ]; then
    echo "Faltan:$MISSING"
    echo "Ejecuta primero: ./tools/fetch_binaries.sh && ./tools/create_macos_img.sh"
    exit 1
fi

# Copia temporal de OVMF_VARS (QEMU escribe en él)
cp "$OVMF_VARS" "$OVMF_VARS_TMP"

# ── Determinar aceleración ────────────────────────────
ACCEL=""
if [ "$(uname -m)" = "x86_64" ] && qemu-system-x86_64 -accel help 2>&1 | grep -q hvf; then
    ACCEL="-accel hvf -cpu Penryn,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,vendor=GenuineIntel"
    echo "  Aceleración: HVF (Intel Mac — rápido)"
else
    ACCEL="-accel tcg,thread=multi -cpu Penryn,+ssse3,+sse4.2,+popcnt,+xsave,+xsaveopt,vendor=GenuineIntel"
    echo "  Aceleración: TCG (emulación x86 — la instalación será lenta)"
    echo "  Nota: en PS5 usará KVM nativo, esto es solo para preparar la imagen"
fi

echo "  macOS.img:    $MACOS_IMG"
echo "  OpenCore:     $OC_QCOW2"
echo ""

# ── Construir argumentos QEMU ─────────────────────────
QEMU_ARGS=(
    -machine q35
    $ACCEL
    -smp 4,cores=4,threads=1
    -m 8G

    # Controlador SATA (AHCI)
    -device ich9-ahci,id=sata

    # UEFI firmware
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_TMP"

    # OpenCore (imagen pre-construida de OSX-KVM)
    -drive "id=OpenCore,if=none,snapshot=on,format=qcow2,file=$OC_QCOW2"
    -device ide-hd,bus=sata.0,drive=OpenCore,bootindex=1

    # Disco macOS
    -drive "id=MacHDD,if=none,format=qcow2,file=$MACOS_IMG"
    -device ide-hd,bus=sata.1,drive=MacHDD

    # Display
    -device virtio-vga
    -display default,show-cursor=on

    # Red (SSH en puerto 2222)
    -device virtio-net-pci,netdev=net0
    -netdev user,id=net0,hostfwd=tcp::2222-:22

    # USB
    -device qemu-xhci
    -device usb-kbd
    -device usb-tablet
)

# Añadir recovery (siempre, salvo NO_RECOVERY=1; su existencia se comprobó arriba)
if [ "${NO_RECOVERY:-0}" != 1 ]; then
    QEMU_ARGS+=(
        -drive "id=Recovery,if=none,format=raw,file=$RECOVERY_IMG"
        -device ide-hd,bus=sata.2,drive=Recovery
    )
    echo "  Recovery:     $RECOVERY_IMG"
fi

echo ""
echo "  Lanzando QEMU..."
echo "  (La ventana de QEMU se abrirá con OpenCore)"
echo ""
echo "  Instrucciones de instalación:"
echo "    1. En OpenCore, selecciona 'macOS Base System'"
echo "    2. Abre Disk Utility, formatea el disco de 64GB como APFS"
echo "    3. Sal de Disk Utility e instala macOS"
echo "    4. La instalación reiniciará varias veces"
echo "    5. Cuando termine, apaga la VM (Cmd+Q en QEMU)"
echo ""
echo "  SSH disponible en: ssh localhost -p 2222"
echo "  Ctrl+Alt+G para liberar el ratón"
echo ""

qemu-system-x86_64 "${QEMU_ARGS[@]}"

echo ""
echo "═══════════════════════════════════════════════"
echo "Instalación completada."
echo ""
echo "  macOS.img listo: $(du -h "$MACOS_IMG" | cut -f1)"
echo ""
echo "  Siguiente paso: copiar todo al USB"
echo "    ./tools/create_usb.sh /dev/diskN"
echo "═══════════════════════════════════════════════"
