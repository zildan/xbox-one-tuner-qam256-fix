# Dev_XboxOne_QAM256 — Direct Device Module for AltDVB (Windows)

> ⚠️ **AVERTISMENT / DISCLAIMER (STATUS: TEST / EXPERIMENTAL)**
> 
> Acest modul de Windows pentru AltDVB (`Dev_XboxOne_QAM256.int`) este în stadiu de **TEST / EXPERIMENTAL (BETA)** și **NU este considerat încă 100% stabil**.
> - Imaginea este mult mai stabilă față de soluțiile anterioare, însă pe anumite configurații sau multiplexuri dense mai pot apărea ocazional artefacte.
> - **Fiecare utilizator testează pe propria răspundere! (Use and test at your own risk!)**
> - Proiectul este oferit „așa cum este” (*as is*), fără garanții de stabilitate absolută. Feedback-ul și testele comunității sunt binevenite!

---

## 📌 Recomandări importante pentru redare în AltDVB
1. **Renderer Video:** În AltDVB, la **Settings -> DirectShow Config -> Main -> Video -> Renderer**, selectați **Default Video Renderer** sau **Video Mixing Renderer 9 (VMR-9)**.  
   *(Pe Windows 10/11, „Enhanced Video Renderer / EVR” poate produce ecran negru fără imagine).*
2. **Decodoare DirectShow (32-bit):** AltDVB este o aplicație x86 (32-bit), deci necesită codecuri DirectShow pe 32-bit (ex: **LAV Filters x86** sau **Microsoft DTV-DVD Video Decoder**).

---

## 🚀 Cum se instalează și se folosește

### 1. Asocierea Tunerului cu WinUSB (folosind Zadig)
Deoarece tunerul nu are driver oficial de la Microsoft pentru Windows:
1. Conectează tunerul de Xbox One la un port USB al PC-ului.
2. Descarcă utilitarul gratuit [Zadig](https://zadig.akeo.ie/).
3. Deschide Zadig:
   - Mergi în meniu la **Options** -> bifează **List All Devices**.
   - Din lista derulantă selectează **Xbox One Digital TV Tuner** (verifică să aibă USB ID `045E 02D5`).
   - În câmpul din dreapta săgeții verzi alege **WinUSB** (versiunea implicită Microsoft).
   - Apasă pe butonul **Replace Driver** (sau **Install Driver**).

---

### 2. Copierea fișierelor în AltDVB
1. Copiază fișierele din acest folder direct în directorul **`Devices\`** al instalării tale de AltDVB:
   - `Dev_XboxOne_QAM256.int`
   - `Dev_XboxOne_QAM256.cfg`
2. Pune cele două fișiere de firmware în folderul `Devices\` (sau în rădăcina AltDVB):
   - `dvb-usb-dib0700-1.20.fw`
   - `dvb-demod-mn88472-02.fw`

---

### 3. Utilizarea în AltDVB
1. Pornește **AltDVB**.
2. La pornire (în fereastra *Select Device Interface*), selectează **`Dev_XboxOne_QAM256`**.
3. Tunerul se va inițializa automat prin WinUSB:
   - Ceasul intern va fi setat la **72 MHz**
   - Magistrala I2C va rula la **1000 kHz** (mod rapid)
   - Tranzacțiile I2C se execută **atomic** (fără blocaj de FIFO)
   - Demodulatorul Panasonic MN88472 este setat pe **`VARIABLE_TS_CLOCK`**
4. Mergi la Scanare / Vizionare pe frecvențele DVB-C dorite (de exemplu rețeaua DIGI cablu).
