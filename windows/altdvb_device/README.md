# Dev_XboxOne_QAM256 — Direct Device Module for AltDVB (Windows)

Acest modul permite utilizarea tunerului **Microsoft Xbox One Digital TV Tuner (`045e:02d5`)** direct în **AltDVB** pe Windows (10/11), beneficiind de fix-ul nostru dezvoltat pentru eliminarea erorilor de continuitate pe multiplexurile de cablu DVB-C QAM-256 de mare viteză (SR 6900, ~50 Mbps).

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
