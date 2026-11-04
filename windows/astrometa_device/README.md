# Astrometa DVB-T2/C USB Stick - AltDVB 2.5 Device Driver (`Dev_Astrometa_DVB.int`)

Acest modul reprezintă implementarea de producție nativă WinUSB a driverului AltDVB 2.5 (32-bit `__stdcall` DLL) pentru tunerul USB **Astrometa DVB-T2/C**, validat complet pe DVB-C (DIGI România, 64-QAM și 256-QAM, SR 6900) cu **0 packet drops** pe peste 1.000.000 pachete MPEG-TS consecutive.

---

## 1. Identificare Hardware & Revizii Astrometa

Pe piață există cel puțin trei generații de tunere vândute sub brandul Astrometa (carcasă neagră cu LED albastru/roșu și mufă IEC):

| Versiune / Generație | Demodulator Secundar | Tuner RF | Standarde Suportate | Identificare Demodulator | Comportament DVB-C |
|---|---|---|---|---|---|
| **Revizia 1 (clasică)** | Panasonic **MN88472** | Rafael Micro **R828D** | DVB-T, DVB-T2, DVB-C | I2C `0x18`/`0x1A`, Chip ID `0x02` la Bank 0 `0xFF` | Suportă DVB-C pe Linux, dar driverul vechi de Windows Astrometa adesea nu oferea firmware DVB-C. TS Mod: Parallel (`0x08 = 0x00`). |
| **Revizia 2 (ACEST DRIVER)** | Panasonic **MN88473** | Rafael Micro **R828D** | DVB-T, DVB-T2, DVB-C | I2C `0x1C` (scrie `0x38`, citește `0x39`), Chip ID `0x03` la Bank 2 `0xFF` | **Complet funcțional cu acest driver**. Mod TS: **Serial** (`Bank 2 0x08 = 0x1D`), clock 25 MHz, IF 5.07 MHz. |
| **Revizia 3 ("Fără DVB-C" / T2-only)** | Sony **CXD2837ER** sau **Fără demod extern** (doar RTL2832U) | Rafael Micro R828D sau Fitipower FC0012/13 | DVB-T, DVB-T2 (sau doar DVB-T/DAB/FM) | Sony I2C `0x6C`/`0x6D` sau niciun răspuns pe I2C slave | Lipsă hardware DVB-C (filtre RF cablu nepopulate) sau demodulatorul Sony nu suportă/nu are registre DVB-C configurate în firmware-ul vendor. |

### Detalii Stick Curent (Revizia 2):
* **USB VID:PID:** `15F4:0131` (Composite Device)
* **Interfață USB Streaming:** `MI_00` (Endpoint `0x81` Bulk IN, max packet 512 bytes)
* **Driver USB:** Microsoft WinUSB (`winusb.sys`) asociat pe `MI_00` prin Zadig.
* **Bridge USB & I2C Master:** Realtek **RTL2832P**
* **Demodulator Principal:** Panasonic **MN88473** (firmware microcode: 2271 bytes, Bank 0 `0xF6`)
* **Silicon Tuner:** Rafael Micro **R828D** (adresă I2C: `0x3A` / 8-bit `0x74`, XTAL: 16.0 MHz)

---

## 2. Arhitectură & Rutare Slave TS (RTL2832P)

Pentru a primi date MPEG-TS de la demodulatorul extern Panasonic MN88473, bridge-ul Realtek RTL2832P trebuie configurat în modul **Slave TS Bypass** către USB Endpoint A (EPA / EP 0x81):

### Secvența Critică de Registre RTL2832:
1. **Control Demodulator & DMA EPA (`SYS_DEMOD_CTL` / Reg `0x3000`):**
   * Trebuie scris mascat: `val = (val & 0x0F) | 0xE0;` (scriere `0xE0, 0xE0`).
   * **CRITIC:** Bitul 6 (`0x40`) menține deschisă poarta DMA către EPA. Dacă se scrie `0xA0` (fără bitul `0x40`), Endpoint-ul `0x81` se blochează complet în `WAIT_TIMEOUT` / WinUSB Error 121 (`ERROR_SEM_TIMEOUT`).
   * Reg `0x300B` (`SYS_DEMOD_CTL1`): bitul 4 (`0x10`) trebuie șters pentru a scoate demodulatorul din suspend.
2. **I2C Repeater & Soft Reset (Page 1, Reg `0x01` / `0x101`):**
   * Valoare: `0x18` (bit 2 `DVBT_SOFT_RST` = 0, bit 3 `DVBT_IIC_REPEAT` = 1, bit 4 = 1).
   * **ATENȚIE:** Nu scrieți `0x00` în `0x101`! Închiderea repeater-ului I2C împiedică comunicarea cu tunerul R828D și prăbușește semnalul RF (AGC scade la 0).
3. **Muxare Slave TS:**
   * Reg `0x10C`: `\x5F\xFF` (2 bytes).
   * Reg `0x021` (`DVBT_PIP_ON`): bit 3 setat (`val = (val & ~0xC0) | 0x08`).
   * Reg `0x0BC`: `\x18`.
   * Reg `0x192`: `\x7F\xF7\xFF` (3 bytes, rutează magistrala TS externă în EPA FIFO).
4. **Deblocare FIFO & Reset Stalls:**
   * Reg `0x2000` (`USB_SYSCTL`): `|= 0x09` (Activează EPA Full Packet Mode & DMA).
   * Reg `0x2158` (`EPA_MAX_PKT`): `512` bytes (`{0x00, 0x02, 0x00, 0x00}`).
   * Reg `0x2148`: `\x00\x00` (șterge starea de EPA stall).

### Registre RTL2832 Interzise la Suprascriere:
* **NU suprascrieți `Page 0, Reg 0x06, 0x07, 0x0D, 0x61`!**
  Spre deosebire de driverele generice DVB-T, pe Astrometa aceste registre controlează pinii GPIO și semnalul de ceas de referință distribuit către R828D. Modificarea lor oprește oscilatorul tunerului și distruge AGC-ul.

---

## 3. Demodulator Panasonic MN88473

### A. Încărcare Firmware:
* Firmware-ul de 2271 octeți (`mn88473_fw.h`) este scris pe Bank 0, registrul `0xF6` în blocuri de 16 octeți.
* Verificare la pornire: dacă Bank 0 reg `0xF5` are bitul 0 curățat (`0x20`), firmware-ul rulează deja.

### B. Configurare DVB-C (Annex A):
* **Selectare Mod:** Bank 2 reg `0x03 = 0x04` (DVB-C).
* **Frecvență IF (5.07 MHz pe ceas demodulator de 25 MHz):**
  $$\text{uitmp} = \frac{5070000 \times 2^{24} + 12500000}{25000000} = \text{0x33EAB3}$$
  * Bank 2 reg `0x10 = 0x33`
  * Bank 2 reg `0x11 = 0xEA`
  * Bank 2 reg `0x12 = 0xB3`
* **Filtru Digital:** Bank 1 reg `0x10`: `\x10\xAB\x0D\xAE\x1D\x9D` (6 bytes).
* **Configurare Ieșire TS (Serial Mode):**
  * Bank 2 reg `0x08 = 0x1D` (Serial TS Mode).
  * Bank 2 reg `0x09 = 0x08` (TS Clock polarity și enable).

> [!CAUTION]
> **Diferență Majoră față de Xbox One Tuner (MN88472):**
> Pe tunerul Xbox One (care folosește MN88472 în mod Parallel TS), scrierea `Bank 0, Reg 0xD9 = 0xE3` era fixul critic pentru ceasul variabil.
> **Pe MN88473 în modul Serial TS, scrierea `0xD9 = 0xE3` OPREȘTE COMPLET ieșirea de clock serial TS!** Registrul `0xD9` NU trebuie modificat pe MN88473.

---

## 4. Pipeline de Streaming USB & Resincronizare TS

### A. Dimensiune Buffer (`BULK_STAGESIZE` = 24.064 octeți):
* Fiecare buffer are exact **24.064 octeți** ($128 \times 188\text{ bytes} = 47 \times 512\text{ USB packets}$).
* Această dimensiune corespunde parametrului hardware `BULK_STAGESIZE` din registry-ul driverului vendor Astrometa și previne fragmentarea asincronă pe endpoint-ul High-Speed.
* Alocare memorie aliniată la pagină de 4096 octeți (`_aligned_malloc(24064, 4096)`).

### B. Ring Buffer cu 16 Buffer-e Overlapped:
* Thread-ul de streaming rulează la `THREAD_PRIORITY_HIGHEST`.
* 16 cereri `WinUsb_ReadPipe` asincrone sunt menținute simultan în coada USB a kernelului Windows. Imediat ce un buffer se completează, este reînarmat la coadă.

### C. Algoritm de Sincronizare Robustă (2-Sync Hunter + Single Corrupt Bypass):
Fluxul de date de la ieșirea serială a MN88473 prin RTL2832 poate conține ocazional un octet de padding (`0xFF`) sau un pachet incomplet la schimbarea de canal:
1. **Achiziție Sincronizare:** Verifică simultan doi octeți de sync `0x47` separați de 188 de octeți (`chunk[k] == 0x47 && chunk[k+188] == 0x47`).
2. **Toleranță la Pachete Izolate Corupte:** Dacă pachetul curent nu are `0x47`, dar pachetul imediat următor (`+188`) are `0x47`, pachetul corupt este sărit fără a reseta starea de sync și fără a arunca restul de 23.000 octeți din buffer.
3. **Re-hunt Intern Rapid:** Dacă sincronizarea este pierdută în interiorul buffer-ului, se caută o nouă pereche `0x47` în cadrul aceluiași buffer, prevenind artefactele video de macroblock.

---

## 5. Ghid de Compilare & Instalare

### Compilare Cross pe Linux NUC (`root@192.168.5.106`):
```bash
# Copiere fișiere sursă pe NUC
scp -i C:\Users\luisa\.ssh\id_ed25519 main_altdvb.c astrometa_device.h altdvb_interface.h rtl2832_core.c mn88473_core.c mn88473_fw.h r820t_core.c r820t.c r820t.h usb_winusb.c Dev_Astrometa_DVB.def root@192.168.5.106:/tmp/astrometa_build/

# Compilare DLL 32-bit (i686-w64-mingw32-gcc)
ssh -n -i C:\Users\luisa\.ssh\id_ed25519 root@192.168.5.106 "cd /tmp/astrometa_build && i686-w64-mingw32-gcc -shared -s -O2 -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup -o Dev_Astrometa_DVB.int Dev_Astrometa_DVB.def main_altdvb.c rtl2832_core.c mn88473_core.c r820t_core.c usb_winusb.c -lwinusb -lsetupapi"

# Deploy direct în AltDVB 2.5
scp -i C:\Users\luisa\.ssh\id_ed25519 root@192.168.5.106:/tmp/astrometa_build/Dev_Astrometa_DVB.int "D:\temporare\AltDVB.v25_0\Devices\Dev_Astrometa_DVB.int"
```

---

## 6. Notă Pregătitoare pentru Investigarea Celuilalt Stick Astrometa

Pentru a doua versiune de Astrometa menționată (care aparent nu are DVB-C):
1. **Test Conectare & Verificare USB ID:**
   * La conectare, verificăm ID-ul hardware în Device Manager sau PowerShell (`Get-PnpDevice` / `usbview`).
   * Dacă are același `15F4:0131` sau `15F4:0130`, identificarea se face direct prin I2C probing pe magistrala RTL2832.
2. **Posibile Configurații Hardware:**
   * **Dacă demodulatorul este Sony CXD2837ER:** Adresă I2C `0x6C` / `0x6D`. Acest cip poate fi T/T2-only dacă circuitele analogice de cablu nu sunt cablate pe PCB, sau poate necesita setul complet de tabele de registre Sony.
   * **Dacă demodulatorul este Panasonic MN88472:** Adresă I2C `0x18` (răspunde cu `0x02` la Bank 0 `0xFF`). Necesită firmware-ul microcode pentru MN88472 și mod Parallel TS (`Bank 2 0x08 = 0x00`).
   * **Dacă este un stick RTL2832U pur:** Nu are niciun cip secundar pe I2C (doar RTL2832 + tuner R820T/FC0012). În acest caz, suportul hardware este exclusiv DVB-T / DAB / FM, fără capabilitate fizică de DVB-C sau DVB-T2.
