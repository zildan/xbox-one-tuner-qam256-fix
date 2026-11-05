# Fushicai USBTV007 (EasyCap DC60) - Ghid Tehnic Digitizor Video & Audio

Documentație tehnică și specificații de operare pentru adaptorul de captură USB **EasyCap cu cip Fushicai UTV007**.

---

## 1. Identificare Hardware

* **Nume Dispozitiv (PnP Windows):** `usbtv007`
* **USB Vendor ID (VID):** `0x1B71` (Fushicai Audio-Video Grabber)
* **USB Product ID (PID):** `0x3002`
* **Număr Serial:** `300000000002`
* **Driver Kernel Linux:** Modulul oficial **`usbtv`** (`drivers/media/usb/usbtv/`)
* **Arhitectură Chipset:** **Fushicai UTV007 / HTV007 SoC** – circuit integrat all-in-one low-cost ce include:
  * Decodor video analogic NTSC/PAL/SECAM (ADC 8-bit, 27 MHz).
  * ADC audio stereo integrat (eșantionare 48.000 Hz, 16-bit).
  * Controller USB 2.0 High-Speed (480 Mbps).

---

## 2. Conectori Hardware (Gata de Utilizare fără Modificări)

Spre deosebire de tunerele cu mufe proprietare, adaptorul are cablurile pigtail integrate direct din fabrică:
1. **RCA Galben (Mamă):** Intrare Video Composite (CVBS) 75 $\Omega$.
2. **RCA Alb (Mamă):** Intrare Audio Analogic Canal Stânga (Line-In L).
3. **RCA Roșu (Mamă):** Intrare Audio Analogic Canal Dreapta (Line-In R).
4. **Mini-DIN 4-pini (Mamă):** Intrare S-Video (Separare Luma $Y$ + Chroma $C$).

---

## 3. Comparație Tehnică pentru Digitizare VHS: UTV007 vs Pinnacle 330e

| Criteriu | **EasyCap UTV007 (`1B71:3002`)** | **Pinnacle PCTV 330e (`2304:0226`)** |
| :--- | :--- | :--- |
| **Cip Decodor Video** | **Fushicai UTV007** (All-in-one comercial) | **Texas Instruments TVP5150AM1** (Broadcast Grade) |
| **Conectivitate Fizică** | **Avantaj major:** Are deja cablurile RCA gata | Necesită cablu dedicat sau lipire pigtail |
| **Toleranță la Jitter VHS** | **Medie:** La casete foarte uzate sau întreruperi de bandă, poate suferi de desincronizare A/V sau drop de cadre. | **Excepțională:** PLL hardware Line-Locked dedicat pentru compensarea deformărilor de sincronizare mecanică. |
| **Filtru Separare Culori** | Filtru pieptene simplu 2D (poate produce ușor zgomot cromatic) | **Filtru pieptene adaptiv pe 4 linii (4H)** |
| **Suport Drivere** | Recunoscut automat pe Linux (`usbtv`), DirectShow pe Windows | Recunoscut nativ pe Linux (`em28xx`), DirectShow pe Windows |

---

## 4. Utilizare sub Linux (V4L2 + ALSA)

Modulul `usbtv` este inclus nativ în nucleul Linux (Ubuntu, Debian, Proxmox, Raspberry Pi OS).

### Verificare la conectare:
```bash
dmesg | grep -i usbtv
lsusb -d 1b71:3002
v4l2-ctl --list-devices
```

### Comandă FFmpeg pentru Captură Directă Lossless (PAL 720x576):
```bash
ffmpeg -f v4l2 -standard PAL -video_size 720x576 -framerate 25 -i /dev/video0 \
       -f alsa -channels 2 -sample_rate 48000 -i hw:CARD=usbtv007,DEV=0 \
       -c:v ffv1 -level 3 -c:a pcm_s16le \
       easycap_vhs_capture.mkv
```

---

## 5. Utilizare sub Windows (DirectShow / OBS / VirtualDub2)

* Instalați driverul OEM Fushicai DirectShow Video Grabber (sau `usbtv007 x64 installer`).
* Dispozitivul apare în **OBS Studio** sau **VirtualDub2** ca:
  * Video: `OEM Device` sau `USBTV007 Video`
  * Audio: `OEM Device` sau `USBTV007 Audio`
* Selectați norma **PAL-B/G** și rezoluția **720x576 @ 25 fps** (spațiu de culoare `YUY2`).
