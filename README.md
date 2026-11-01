# Microsoft Xbox One Digital TV Tuner (`045e:02d5`) — DVB-C QAM-256 High-Bitrate Fix (~50.87 Mbps / DIGI)

**[RO]** Driver Linux reparat (`mn88472` + `dvb_usb_dib0700`) pentru tunerul USB **Microsoft Xbox One Digital TV Tuner (`045e:02d5`)**, care elimină complet pierderile de pachete (`Continuity Counter Errors`, macroblocking, artefacte video) pe transponderele **DVB-C QAM-256 cu bitrate mare (47 – 50.87 Mbps, ex: DIGI / RCS-RDS SR 6900)**.

**[EN]** Patched Linux kernel driver (`mn88472` + `dvb_usb_dib0700` tuning) for the **Microsoft Xbox One Digital TV Tuner (`045e:02d5`)**, completely fixing MPEG-TS packet drops (`Continuity Counter Errors` / corrupted recordings) on full-capacity **DVB-C QAM-256 transponders (47 – 50.87 Mbps, SR 6900)**.

---

## 🇷🇴 1. De ce nu mergea pe DIGI QAM-256 cu driverul original (și cum am rezolvat)

Pe forumuri (inclusiv pe **sateliti.info**) s-a observat de ani de zile că tunerul USB de Xbox One (`045e:02d5`) merge bine pe DVB-T2 sau pe frecvențe QAM-256 cu bitrate mic (`< 36 Mbps`), dar **pe cablu DIGI (unde transponderele QAM-256 au 47 – 49.5 Mbps date video utile din 50.87 Mbps total) rupe imaginea și adună constant `Continuity Counter Errors`**.

### Cauza tehnică demonstrată prin măsurători la nivel de microsecundă:
1. **Hardware-ul RF (`NXP TDA18250BHN`) și demodulatorul (`Panasonic MN88472`) NU au nicio problemă:**
   - Măsurat live pe cel mai încărcat transponder DIGI (**`530 MHz` — `49.48 Mbps` payload video/audio util din `50.87 Mbps`, adică 97% plin**), demodulatorul are `CNR = 33.7–34.4 dB`, `0` erori Reed-Solomon (`BlkErr = 0`) și `TEI = 0`. Fără trafic I2C parazit, transferă milioane de pachete TS cu **0 erori**.
2. **„Smoking Gun” — Ștrangularea FIFO-ului din cipul USB `DiB0700` la fiecare interogare `FE_READ_STATUS`:**
   - Fiecare pachet pierdut (`CC_err`, exact 1 pachet TS de 188 bytes) pică **în milisecunda exactă în care aplicația (Tvheadend / Plex / Enigma2 / Kaffeine) apelează `FE_READ_STATUS`** (de obicei o dată pe secundă pentru a afișa SNR-ul și starea semnalului).
   - În driverul original [`mn88472.c`](file:///d:/Proiecte%20AI/DRIVER%20Tunner%20XBOX/src_orig/mn88472.c), fiecare apel `mn88472_read_status()` execută necondiționat **5 tranzacții I2C (10 pachete sincrone USB Control pe Endpoint 0)** către adresele I2C `0x18`, `0x1a` și `0x1c`.
   - Cipul bridge USB **DiB0700** are înăuntru un microcontroler **8051 single-threaded** vechi. Cât timp acest 8051 face bit-banging I2C la 100 kHz pentru cele 5 citiri de status/SNR, **îngheață pentru câteva sute de microsecunde golirea FIFO-ului de Transport Stream**!
   - Pe un transponder cu bitrate mic (`35 Mbps`), ~30% din pachete sunt goale (`NULL PID 0x1FFF`), deci înghețul nu lovește pachete video. Pe DIGI la `49.48 Mbps` payload util (`530 MHz`), sosește câte un pachet video la fiecare **29,5 microsecunde** — astfel încât înghețul I2C dă peste cap FIFO-ul și aruncă pe jos câte 1 pachet video la fiecare interogare!
3. **Ceasul TS și regiștrii DVB-C din `mn88472.c` + bufferele URB din `dvb_usb_dib0700`:**
   - Driverul stock setează `FIXED_TS_CLOCK` (`0xe1` în reg `0xd9`), `0x48` în reg `0xd6` și omite regiștrii filtrului de bandă Panasonic pentru DVB-C (`0x13..0x19`).
   - Driverul `dvb_usb_dib0700` alocă implicit un buffer USB minuscul (`nb_packet_buffer_size=21`, doar 3.5 KB/URB) și lasă activ polling-ul I2C pentru telecomandă IR.

### Ce face acest Fix:
1. **Smart I2C Status/CNR Caching în [`mn88472_read_status()`](file:///d:/Proiecte%20AI/DRIVER%20Tunner%20XBOX/src_patched/mn88472.c):** La acordarea pe un canal nou (`mn88472_set_frontend`), driverul citește normal pe I2C până când obține `FE_HAS_LOCK`, puterea semnalului și `CNR`-ul real (`~34 dB`). Odată încuiat pe frecvență, răspunde instantaneu aplicației (Tvheadend / Plex / Enigma2) din cache, fără să mai blocheze microcontrolerul 8051 din DiB0700 în timpul streamului de 50 Mbps (controlabil dinamic prin parametrul `/sys/module/mn88472/parameters/stat_poll_interval`).
2. **Activează `VARIABLE_TS_CLOCK` (`0xe3` în reg `0xd9`), `reg 0xd6 = 0x46` și filtrele de bandă Panasonic DVB-C (`0x13..0x19`)** în [`mn88472_set_frontend()`](file:///d:/Proiecte%20AI/DRIVER%20Tunner%20XBOX/src_patched/mn88472.c).
3. **Mărește bufferele USB Bulk URB (`nb_packet_buffer_size=512`) și oprește RC polling-ul I2C (`disable_rc_polling=1`)** pe `dvb_usb_dib0700`.

---

## 📊 2. Rezultate Măsurate Live pe Rețeaua DIGI (QAM-256, SR 6900)

Testat cu polling `FE_READ_STATUS` + `FE_GET_PROPERTY` activ la fiecare secundă (`poll_fe=1`):

| Frecvență DIGI | Modulație | Bitrate Total | Payload Video/Audio Util | CNR | `CC_errors` (Driver Stock) | `CC_errors` (Driver Reparat) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **530 MHz** | QAM-256 | `50.87 Mbps` | **`49.34 – 49.48 Mbps` (97%)** | `33.7 dB` | `1 – 2 erori / 10s` | **`0 erori` (0 TEI, 0 BlkErr)** |
| **506 MHz** | QAM-256 | `50.87 Mbps` | **`48.50 – 48.86 Mbps` (96%)** | `33.7 dB` | `1 – 2 erori / 10s` | **`0 erori` (0 TEI, 0 BlkErr)** |
| **538 MHz** | QAM-256 | `50.87 Mbps` | **`47.67 – 47.93 Mbps` (94%)** | `33.5 dB` | `1 – 2 erori / 10s` | **`0 erori` (0 TEI, 0 BlkErr)** |

---

## 🛠️ 3. Instalare Rapidă (Ubuntu / Debian / Proxmox / Mint / Raspberry Pi OS)

Compatibil cu orice kernel Linux de la **4.15 până la 6.12+**:

```bash
git clone https://github.com/zildan/xbox-one-tuner-qam256-fix.git
cd xbox-one-tuner-qam256-fix
chmod +x install.sh
sudo ./install.sh
```

### Pentru dezvoltatorii de imagini Enigma2 (OpenATV / OpenPLi) / LibreELEC / CoreELEC:
Aplicați direct patch-ul [`xbox-one-tuner-qam256-fix.patch`](file:///d:/Proiecte%20AI/DRIVER%20Tunner%20XBOX/xbox-one-tuner-qam256-fix.patch) peste `drivers/media/dvb-frontends/mn88472.c` și `mn88472_priv.h`, și adăugați opțiunile de modul din [`xbox-tuner-qam256.conf`](file:///d:/Proiecte%20AI/DRIVER%20Tunner%20XBOX/src_patched/xbox-tuner-qam256.conf) (`options dvb_usb_dib0700 nb_packet_buffer_size=512 disable_rc_polling=1`).

---

## 🇬🇧 4. English Summary (For Upstream `linux-media` & International Users)

### Root Cause
On the **Microsoft Xbox One Digital TV Tuner (`045e:02d5`)**, the Panasonic `MN88472` demodulator and NXP `TDA18250BHN` tuner are bridged via a **DiBcom DiB0700** USB controller (`fw 1.20`).
Every call to `mn88472_read_status()` performs **5 I2C transactions (10 synchronous USB Endpoint 0 control transfers)** across I2C addresses `0x18`, `0x1a`, and `0x1c`. Because the DiB0700 runs a single-threaded 8051 microcontroller, servicing 10 synchronous EP0 I2C requests stalls the hardware MPEG-TS slave FIFO for several hundred microseconds.
While unnoticeable on DVB-T2 or lower-payload DVB-C muxes (where NULL packets `PID 0x1FFF` absorb the timing jitter), on a 97%-full **DVB-C QAM-256** mux (`50.87 Mbps` total / `49.48 Mbps` active video payload, 1 TS packet every `29.5 µs`), every `FE_READ_STATUS` poll overflows the DiB0700 TS FIFO and drops 1 MPEG-TS packet (`Continuity Counter Error`).

### Solution
1. Cache lock status and initial CNR/strength in `mn88472_read_status()` once `FE_HAS_LOCK` and valid CNR are established (resetting on `mn88472_set_frontend()` and `mn88472_sleep()`, and configurable via module parameter `stat_poll_interval`).
2. Configure `VARIABLE_TS_CLOCK` (`0xe3` in `reg 0xd9`), `0x46` in `reg 0xd6`, and load the 8 MHz DVB-C bandwidth filter registers (`0x13..0x19`) on `SYS_DVBC_ANNEX_A`.
3. Increase `dvb_usb_dib0700` URB buffer size (`nb_packet_buffer_size=512`) and disable IR polling (`disable_rc_polling=1`).
