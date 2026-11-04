# Pinnacle PCTV Hybrid Pro Stick 330e - Ghid Tehnic Digitizare VHS & AV

## 1. Prezentare Hardware

Dispozitivul conectat este un tuner hibrid USB 2.0 din generația clasică Pinnacle / PCTV Systems, recunoscut ca unul dintre cele mai robuste echipamente pentru conversia semnalelor video analogice SD instabile (cum sunt casetele VHS/S-VHS uzate).

### Identificatori USB
* **Vendor ID (VID):** `0x2304` (Pinnacle Systems, Inc. / PCTV Systems)
* **Product ID (PID):** `0x0226` (PCTV 330e / Pinnacle Hybrid Pro 330e)
* **Serial Number:** `071101161601`
* **Driver Linux:** Modul `em28xx` (`EM2882_BOARD_PINNACLE_HYBRID_PRO_330E`)

---

## 2. Arhitectura Cipurilor Interne

| Componentă | Cip / Producător | Rol și Caracteristici Tehnice |
| :--- | :--- | :--- |
| **USB 2.0 Bridge** | **eMPIA EM2882 / EM2883** | Bridge USB 2.0 High-Speed (480 Mbps). Gestionează stream-ul izocron/bulk video și ADC-ul audio stereo (AC97 / I2S Line-In). |
| **Video Decoder (AV)** | **Texas Instruments TVP5150AM1** | Decodor video NTSC/PAL/SECAM broadcast-quality. Deține un **PLL adaptiv de sincronizare orizontală extrem de stabil**, ideal pentru corecția erorilor mecanice de bandă VHS fără Time Base Corrector (TBC) extern dedicat. |
| **Demodulator Digital** | **Micronas DRX-D (DRX3975D)** | Demodulator digital **exclusiv DVB-T** (COFDM). Nu suportă DVB-C (cablu) și nici DVB-T2. |
| **RF Tuner** | **Xceive XC3028** (XC3028ACQ) | Tuner hibrid analogic/digital cu sinteză PLL și descărcare de microcod firmware (`xc3028.fw`). |

---

## 3. De ce este TVP5150 ideal pentru Casete VHS

Spre deosebire de adaptoarele ieftine de captură USB comerciale (EasyCap cu cipurile UTV007, AMT630 sau macro-decodoare ieftine):
1. **Rezistență la Timebase Jitter & Drop-out-uri:** Casetele VHS suferă de variații mari de viteză ale tamburului cu capete video (skew, flagging, jitter). TVP5150 nu dă ecran albastru („Blue Screen”) și nu întrerupe stream-ul de cadre la căderi de semnal.
2. **Filtru pieptene adaptiv 4H (4-Line Adaptive Comb Filter):** Separă precis componenta de luminanță ($Y$) de cea de crominanță ($C$), eliminând efectul de „curcubeu” sau moiré pe texturile fine din înregistrările VHS PAL.
3. **Control Automat al Câștigului (AGC) și Clamping:** Ajustează automat nivelul de negru și albul la semnale atenuate sau casete înregistrate în mod LP (Long Play).

---

## 4. Soluții de Conectare la VCR în lipsa Mufei Originale Breakout

Pinnacle 330e a venit din fabrică cu o mufă laterală proprietară compactă cu 8-10 pini care se ramifica în RCA Galben (Composite), Mini-DIN (S-Video) și Jack/RCA Audio.

### Opțiunea A: Captură prin Modulatorul RF al Videocasetofonului (Fără modificări hardware)
Orice VCR casnic dispune de un modulator RF intern (UHF Canal 36 / 591.25 MHz):
1. Conectați un cablu coaxial TV normal între mufa **RF OUT / AERIAL OUT** a videocasetofonului și mufa de antenă a stick-ului Pinnacle 330e.
2. În softul de captură (DirectShow sau V4L2), selectați intrarea **Tuner TV Analogic** pe norma **PAL B/G**.
3. Tunerul Xceive XC3028 demodulează canalul și livrează semnalul video baseband direct în pinul `TVP5150_COMPOSITE0`.

### Opțiunea B: Lipire directă a unui cablu RCA Composite + Audio pe PCB
Dacă desfaceți carcasa din plastic (clipsată pe margine), pe placa electronică:
* **Composite Video In:** Este legat la intrarea analogică **AIP1B** a cipului TI TVP5150AM1 (prin condensator SMD $0.1\,\mu\text{F}$ serie și rezistență de sarcină $75\,\Omega$).
* **Luma Y (pentru S-Video):** Pinul **AIP1A** al cipului TVP5150.
* **Chroma C (pentru S-Video):** Pinul **AIP1B** al cipului TVP5150.
* **Audio Stereo In:** Pinii de intrare linie analogică (Line-In L / Line-In R) ai cipului eMPIA EM2882.
* **GND (Masă comună):** Ecranajul metalic al mufei USB sau al conectorului de antenă.

---

## 5. Captură Software

### A. Windows (DirectShow / VirtualDub2 / OBS)
* Necesită driverul x64 `5.2010.0901.0` (PCTV Systems TVCenter 6.4 Package).
* Odată instalat driverul WDM/DirectShow, dispozitivul este recunoscut nativ în **VirtualDub2** sau **OBS Studio** ca:
  * Video: `PCTV 330e Video Capture`
  * Audio: `PCTV 330e Audio Capture` (sau conexiune Line-In)
* **Recomandare setări captură VHS:**
  * Rezoluție: `720x576` (PAL 4:3 interlaced, 25 fps / 50 fields/sec)
  * Format culoare: `YUY2` (evitați RGB24 pentru a nu face conversii inutile de spațiu de culoare)
  * Codec Lossless: `HuffYUV` sau `Lagarith` (fără artefacte de compresie)

### B. Linux (V4L2 + ALSA + FFmpeg)
Tunerul este suportat nativ de nucleul Linux (modulul `em28xx`). 
Firmware-ul tunerului se descarcă automat sau se plasează în `/lib/firmware/xc3028.fw`.

Comandă FFmpeg pentru arhivare **Lossless FFV1 + PCM Audio**:
```bash
ffmpeg -f v4l2 -standard PAL -video_size 720x576 -framerate 25 -i /dev/video0 \
       -f alsa -channels 2 -sample_rate 48000 -i hw:CARD=PCTV330e,DEV=0 \
       -c:v ffv1 -level 3 -c:a pcm_s16le \
       caseta_vhs_lossless.mkv
```
