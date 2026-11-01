#!/bin/bash
set -e

echo "=== Instalare Driver Reparat Xbox One TV Tuner (QAM-256 High-Bitrate Fix) ==="

if [ "$EUID" -ne 0 ]; then
    echo "Te rog ruleaza acest script ca root (sudo ./install.sh)"
    exit 1
fi

KVER=$(uname -r)
KBUILD="/lib/modules/${KVER}/build"

if [ ! -d "$KBUILD" ]; then
    echo "Instalare linux-headers pentru ${KVER}..."
    apt-get update && apt-get install -y "linux-headers-${KVER}" build-essential
fi

echo "[1/4] Compilare modul kernel mn88472.ko..."
make -C "$KBUILD" M="$(pwd)" clean
make -C "$KBUILD" M="$(pwd)" modules

echo "[2/4] Instalare modul mn88472.ko in /lib/modules/${KVER}/updates/..."
mkdir -p "/lib/modules/${KVER}/updates"
cp -v mn88472.ko "/lib/modules/${KVER}/updates/mn88472.ko"
depmod -a "$KVER"

echo "[3/4] Configurare parametri USB URB buffer & RC polling in /etc/modprobe.d/xbox-tuner-qam256.conf..."
cp -v xbox-tuner-qam256.conf /etc/modprobe.d/xbox-tuner-qam256.conf

echo "[4/4] Reincarcare module kernel..."
modprobe -r dvb_usb_dib0700 tda18250 mn88472 2>/dev/null || true
modprobe mn88472
modprobe dvb_usb_dib0700

echo "=== GATA! Driverul a fost instalat si activat cu succes! ==="
dmesg | grep -E "mn88472|dib0700|Xbox" | tail -n 10
