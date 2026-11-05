# Unified DVB-C USB Tuner Driver for AltDVB 2.5 (`Dev_DVB_Cable_Unified.int`)

Acest modul reunifică într-un **singur driver nativ WinUSB de producție** (`.int` DLL) suportul complet pentru ambele tunere de cablu DVB-C:
1. **Microsoft Xbox One Digital TV Tuner** (`045E:02D5` — DiBcom DiB0700 + Panasonic MN88472 + NXP TDA18250BHN).
2. **Astrometa DVB-T2/C USB Stick** (`15F4:0131` — Realtek RTL2832P + Panasonic MN88473 + Rafael Micro R828D).

În loc de 4 fișiere separate în AltDVB (`Dev_XboxOne_QAM256.int/.cfg` și `Dev_Astrometa_DVB.int/.cfg`), acum este necesar **un singur driver și o singură configurare** (`Dev_DVB_Cable_Unified.int` + `Dev_DVB_Cable_Unified.cfg`).

---

## 1. Arhitectură & Detecție Automată

La inițializarea de către AltDVB (`DEVICE_DESCRIPTION` și `OPEN_DEVICE`):
* Driverul raportează către AltDVB două sloturi hardware disponibile:
  * **Slot 0:** `Xbox One DVB-C Tuner (DiB0700/MN88472/TDA18250)` (`ID=XBOX`)
  * **Slot 1:** `Astrometa DVB-T2/C Stick (RTL2832P/MN88473/R828D)` (`ID=ASTROMETA`)
* **Detecție dinamică:** Indiferent de selecție sau pe modul automat, driverul scanează magistrala USB prin WinUSB și leagă pipeline-ul corespunzător dispozitivului conectat fizic:
  * Dacă găsește `045E:02D5`: rutează comenzile prin puntea DiB0700, încarcă firmware-ul `dvb-usb-dib0700-1.20.fw` și `dvb-demod-mn88472-02.fw`, inițializează tunerul TDA18250 și ascultă pe Endpoint-ul Bulk `0x82`.
  * Dacă găsește `15F4:0131`: configurează puntea RTL2832 în Slave TS (`SYS_DEMOD_CTL = 0xE0`), încarcă microcodul MN88473 (2271 octeți), inițializează tunerul R828D (5.07 MHz IF) și ascultă pe Endpoint-ul Bulk `0x81`.

---

## 2. Watchdog Automat de Re-achiziție Purtătoare (Hot-Plug Cablu Coaxial)

Driverul integrează un mecanism de autorecuperare în caz de pierdere a semnalului (deconectare cablu coaxial, fluctuații de rețea CATV sau căderi de semnal):
* **Interogare adaptivă continuă:** Monitorizează permanent starea demodulatorului la 400 ms în mod locked și la 150 ms în căutare.
* **Restart automat al automatului de stări (FSM Pulse):** La decuplarea semnalului RF, demodulatoarele Panasonic MN88472 / MN88473 primesc impuls automat pe registrul Bank 2 `0xF8 = 0x9F` pentru scanare continuă de purtătoare.
* **Recuperare instantanee (< 1 secundă):** La reintroducerea cablului coaxial, demodulatorul agață purtătoarea instantaneu, bucla de streaming resetează alinierea pe octetul `0x47`, iar redarea video/audio revine automat în AltDVB **fără a fi nevoie de dublu-click pe canal sau restart de aplicație**.

---

## 3. Pipeline Comun de Streaming de Mare Viteză (0-Drop QAM-256)

Ambele tunere beneficiază de pipeline-ul asincron de producție:
* **Dimensiune buffer (`BULK_STAGESIZE`):** Exact 24.064 octeți ($128 \times 188\text{ octeți} = 47 \times 512\text{ pachete USB}$).
* **Ring buffer:** 16 buffere asincrone suprapuse (`OVERLAPPED`), alocate paginat la 4096 octeți (`_aligned_malloc`).
* **Prioritate thread:** `THREAD_PRIORITY_HIGHEST`.
* **Algoritm de sincronizare dual-sync (2-sync hunter):** Verificare `0x47` la offset și offset+188, cu bypass automat al octeților izolați de padding (`0xFF`) fără pierderea sincronizării.
* **Protecție x87 FPU:** Curățare automată a registrelor FPU Delphi (`fnclex`, `_clear87()`) la fiecare apel de interfață pentru a preveni crash-urile din AltDVB.

---

## 4. Ghid de Compilare

Cross-compilare de pe Linux (sau Linux NUC):
```bash
make
```
Sau manual:
```bash
i686-w64-mingw32-gcc -Wall -Wextra -O2 -s -shared -static-libgcc \
  -Wl,--kill-at -Wl,--enable-stdcall-fixup \
  -o Dev_DVB_Cable_Unified.int Dev_DVB_Cable_Unified.def \
  main_altdvb.c usb_winusb.c dib0700_core.c mn88472_core.c tda18250_core.c \
  rtl2832_core.c mn88473_core.c r820t_core.c -lwinusb -lsetupapi
```

---

## 5. Instalare în AltDVB 2.5

1. Copiați în folderul `AltDVB\Devices\`:
   * `Dev_DVB_Cable_Unified.int`
   * `Dev_DVB_Cable_Unified.cfg`
2. Pentru tunerul Xbox One, asigurați-vă că fișierele de firmware se află în `AltDVB\Devices\`:
   * `dvb-usb-dib0700-1.20.fw`
   * `dvb-demod-mn88472-02.fw`
3. În AltDVB, selectați dispozitivul dorit din lista de *Devices*.
