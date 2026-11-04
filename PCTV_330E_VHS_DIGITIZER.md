# Pinnacle PCTV Hybrid Pro Stick 330e - Ghid Tehnic Digitizare VHS & AV

## 1. Prezentare Hardware

Dispozitivul conectat este un tuner hibrid USB 2.0 din generația clasică Pinnacle / PCTV Systems, recunoscut ca unul dintre cele mai robuste echipamente pentru conversia semnalelor video analogice SD instabile (cum sunt casetele VHS/S-VHS uzate).

### Identificatori USB
* **Vendor ID (VID):** `0x2304` (Pinnacle Systems, Inc. / PCTV Systems)
* **Product ID (PID):** `0x0226` (PCTV 330e / Pinnacle Hybrid Pro 330e)
* **Serial Number:** `071101161601`
* **Driver Linux:** Modul `em28xx` (`EM2882_BOARD_PINNACLE_HYBRID_PRO_330E`)
* **Repository dedicat Gitea:** `https://git.gbc.xyz/zil/pctv-330e-vhs-digitizer`

---

## 2. Arhitectura Cipurilor Interne

| Componentă | Cip / Producător | Rol și Caracteristici Tehnice |
| :--- | :--- | :--- |
| **USB 2.0 Bridge** | **eMPIA EM2882 / EM2883** | Bridge USB 2.0 High-Speed (480 Mbps). Gestionează stream-ul izocron/bulk video și ADC-ul audio stereo (AC97 / I2S Line-In). |
| **Video Decoder (AV)** | **Texas Instruments TVP5150AM1** | Decodor video NTSC/PAL/SECAM broadcast-quality. Deține un **PLL adaptiv de sincronizare orizontală extrem de stabil**, ideal pentru corecția erorilor mecanice de bandă VHS fără Time Base Corrector (TBC) extern dedicat. |
| **Demodulator Digital** | **Micronas DRX-D (DRX3975D)** | Demodulator digital **exclusiv DVB-T** (COFDM). Nu suportă DVB-C (cablu) și nici DVB-T2. |
| **RF Tuner** | **Xceive XC3028** (XC3028ACQ) | Tuner hibrid analogic/digital cu sinteză PLL și descărcare de microcod firmware (`xc3028.fw`). |

---

## 3. PCTV 330e vs PCTV 340e (De ce 330e este mult superior pentru VHS)

Pe modelul mai nou 340e (`2304:023d`), producătorul a tăiat costurile și a trecut la platforma DiBcom DiB0700 + XC4000, eliminând decodorul video dedicat TI TVP5150. Din acest motiv, conform LinuxTV, pe 340e captura video analogică nu funcționează sub Linux (*„analog TV is out of reach / stick does not work with v4l”*).

Modelul **330e** pe care îl deținem are suport nativ analogic complet în nucleul Linux prin `em28xx` și un decodor broadcast TI de top.

---

## 4. Soluție Hardware pentru Lipsa Mufei Originale: Modding Pigtail RCA

Conectorul lateral este un mini-header dreptunghiular ultra-slim proprietar. În locul unui cablu original greu de găsit și predispus la contact imperfect, se realizează un pigtail de 15 cm cu 3 mufe RCA mamă:
* **Video Composite (RCA Galben):** Pinul 2 (`AIP1B`) al cipului TI TVP5150 (printr-un condensator SMD de cuplare de $0.1\,\mu\text{F}$ serie și rezistență de terminare $75\,\Omega$).
* **Audio Left (RCA Alb):** Intrarea stânga a ADC-ului audio din eMPIA EM2882.
* **Audio Right (RCA Roșu):** Intrarea dreapta a ADC-ului audio din eMPIA EM2882.
* **GND (Masă):** Ecranajul metalic general al tunerului/mufei USB.

---

## 5. Telecomandă IR & Protocol Philips RC-5

Tunerul dispune de receptor IR integrat configurat pe protocolul industrial **Philips RC-5** la **36 kHz**:
* **Adresă Dispozitiv:** `0x07` (Decimal `7`)
* **Comenzi principale:** POWER (`0x39`), RECORD (`0x36`), PLAY (`0x30`), STOP (`0x3C`), VOL+/- (`0x03`/`0x09`), CH+/- (`0x06`/`0x0C`).
* Se poate clona sau programa în orice telecomandă universală, aplicație pe smartphone cu infraroșu sau Flipper Zero.

---

## 6. Captură Software

### A. Windows (DirectShow / VirtualDub2 / OBS)
* Necesită driverul x64 `5.2010.0901.0` (PCTV Systems TVCenter 6.4 Package).
* Setări recomandate: PAL B/G 720x576 interlaced, 25 fps, spațiu de culoare `YUY2`, compresie lossless `HuffYUV` sau `Lagarith`.

### B. Linux (V4L2 + ALSA + FFmpeg)
Comandă FFmpeg pentru arhivare **Lossless FFV1 + PCM Audio**:
```bash
ffmpeg -f v4l2 -standard PAL -video_size 720x576 -framerate 25 -i /dev/video0 \
       -f alsa -channels 2 -sample_rate 48000 -i hw:CARD=PCTV330e,DEV=0 \
       -c:v ffv1 -level 3 -c:a pcm_s16le \
       caseta_vhs_lossless.mkv
```
