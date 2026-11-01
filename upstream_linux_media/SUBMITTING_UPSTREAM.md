# Cum trimitem patch-urile oficial în Kernelul Linux (`linux-media`), CoreELEC și LibreELEC

Acest folder conține cele două patch-uri formatate conform standardului oficial Linux Kernel (`git format-patch` / `checkpatch.pl`):
1. [`0001-media-mn88472-fix-DVB-C-QAM-256-high-bitrate-FIFO-st.patch`](0001-media-mn88472-fix-DVB-C-QAM-256-high-bitrate-FIFO-st.patch) — Fixul în driverul demodulatorului `drivers/media/dvb-frontends/mn88472.c`.
2. [`0002-media-dvb-usb-dib0700-use-VARIABLE_TS_CLOCK-on-Xbox-.patch`](0002-media-dvb-usb-dib0700-use-VARIABLE_TS_CLOCK-on-Xbox-.patch) — Trecerea `xbox_one_attach` pe `VARIABLE_TS_CLOCK` în `drivers/media/usb/dvb-usb/dib0700_devices.c`.

---

## 1. Trimiterea în Kernelul Linux Oficial (`linux-media@vger.kernel.org`)

Maintainerii oficiali pentru `mn88472` și `dvb-usb-dib0700` în kernelul Linux sunt:
- **Antti Palosaari** `<crope@iki.fi>` (autorul driverului `mn88472`)
- **Sean Young** `<sean@mess.org>` (maintainer DVB)
- **Mauro Carvalho Chehab** `<mchehab@kernel.org>` (maintainer Linux Media Subsystem)
- Lista oficială (fără abonare necesară, acceptă doar text simplu / plain-text): **`linux-media@vger.kernel.org`**

### Trimitere automată cu `git send-email`:
```bash
git send-email \
  --to="Antti Palosaari <crope@iki.fi>" \
  --to="Sean Young <sean@mess.org>" \
  --to="Mauro Carvalho Chehab <mchehab@kernel.org>" \
  --cc="linux-media@vger.kernel.org" \
  --cc="linux-kernel@vger.kernel.org" \
  upstream_linux_media/0001-media-mn88472-fix-DVB-C-QAM-256-high-bitrate-FIFO-st.patch \
  upstream_linux_media/0002-media-dvb-usb-dib0700-use-VARIABLE_TS_CLOCK-on-Xbox-.patch
```

---

## 2. Includerea în CoreELEC și LibreELEC

Atât **CoreELEC** cât și **LibreELEC** pot include aceste 2 fișiere `.patch` direct în build-ul lor:
- **Pentru CoreELEC:** Deschide un Issue sau Pull Request pe **https://github.com/CoreELEC/CoreELEC** (sau pe forumul `discourse.coreelec.org` la secțiunea DVB) cu link către acest repository și către cele 2 fișiere din `upstream_linux_media/`. În sistemul de build CoreELEC, cele 2 fișiere `.patch` se copiază direct în folderul `projects/Amlogic-ce/patches/linux/` (sau în pachetul `driver.dvb.dvb-latest/patches/`).
- **Pentru LibreELEC:** Pe **https://github.com/LibreELEC/LibreELEC.tv**, cele 2 patch-uri se pun direct în `packages/linux/patches/default/`.
