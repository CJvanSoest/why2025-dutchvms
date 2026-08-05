# Bekende problemen — overzicht

Dit document verzamelt de belangrijkste bugs/root-cause-onderzoeken van het WHY2025-badgeproject (DutchVMS-firmware), met wat er precies mis was, welke bestanden/code het raakte, welke fix-pogingen zijn gedaan en het resultaat. Elk item heeft een eigen GitHub-issue met de volledige details.

Laatst bijgewerkt: 2026-08-05.

## Inhoudsopgave

### Opgelost

1. [BLE transport-timeout crash (cj_meshcore reset)](#1-ble-transport-timeout-crash-cj_meshcore-reset)
2. [C6 SDIO crash-loop → OTA-rollback naar oude versie](#2-c6-sdio-crash-loop--ota-rollback-naar-oude-versie)
3. [why_sbrk() shrink-pad heap-corruptie](#3-why_sbrk-shrink-pad-heap-corruptie)
4. [cj_launcher boot-loop (Launcher_Context stack-overflow)](#4-cj_launcher-boot-loop-launcher_context-stack-overflow)
5. [application_list() manifest-read-race](#5-application_list-manifest-read-race)
6. [cj_wifi_analyzer R/D-hang (task-prioriteit)](#6-cj_wifi_analyzer-rd-hang-task-prioriteit)
7. [Display-backlight GPIO + PWM-curve](#7-display-backlight-gpio--pwm-curve)
8. [PAX app-side symbol-gaten](#8-pax-app-side-symbol-gaten)
9. [cj_launcher PAX splash-performance](#9-cj_launcher-pax-splash-performance)

### Open / gedeeltelijk opgelost

11. [P4-OTA wordt nooit bevestigd (validate_ota_partition)](#11-p4-ota-wordt-nooit-bevestigd-validate_ota_partition)
12. [C6-radio herflasht soms elke boot](#12-c6-radio-herflasht-soms-elke-boot)
13. [Deploy PUT: OOM + UART-overrun bij grote bestanden](#13-deploy-put-oom--uart-overrun-bij-grote-bestanden)
14. [Factory-flash wist WiFi-credentials](#14-factory-flash-wist-wifi-credentials)
15. [About-tile crash na firmware-update](#15-about-tile-crash-na-firmware-update)
16. [Losse render-stipjes op Home-tiles](#16-losse-render-stipjes-op-home-tiles)

---

## Opgelost

### 1. BLE transport-timeout crash (cj_meshcore reset)
`cj_meshcore` crashte 100% reproduceerbaar ~4-5s na openen. Root cause: een bug in Espressif's eigen `esp_hosted`-component (`transport_drv_reconfigure()`) die een 5s watchdog-timer nooit stopt als de transport al actief is (altijd het geval hier, want WiFi brengt 'm al omhoog vóór BLE later reconfigureert). Fix: lokale git-tracked component-override met de ontbrekende timer-stop toegevoegd. Hardware-bevestigd, PR #49 gemerged.
**Volledige details:** [issue #50](https://github.com/CJvanSoest/why2025-dutchvms/issues/50)

### 2. C6 SDIO crash-loop → OTA-rollback naar oude versie
Firmware viel na een paar reboots terug naar een oudere versie. Bleek een crash-loop rond de C6-radio-herflash-logica (3 stacked bugs: reflash-timing, ESP-Hosted reset-elke-boot, `flash_binary()` short-read-corruptie) die ESP-IDF's eigen bootloader-rollback triggerde. Opgelost v1.3.9.
**Volledige details:** [issue #51](https://github.com/CJvanSoest/why2025-dutchvms/issues/51)

### 3. why_sbrk() shrink-pad heap-corruptie
App-launch degradeerde stil na een paar keer starten/sluiten — manifest-reads rapporteerden succes maar gaven nul-bytes terug. Root cause: `why_sbrk()`'s shrink-pad berekende de nieuwe totale heap-grootte in plaats van de vrij te geven delta, waardoor te veel geheugen werd ge-unmapt. Gefixt, host-side dlmalloc-simulatie geverifieerd, hardware-bevestigd, PR #17 gemerged.
**Volledige details:** [issue #52](https://github.com/CJvanSoest/why2025-dutchvms/issues/52)

### 4. cj_launcher boot-loop (Launcher_Context stack-overflow)
Een grote struct (`Launcher_Context`) als stack-lokale variabele in de launcher's entry-point paste niet meer op de app-stack → onmiddellijke crash, oneindige boot-loop (launcher is de eerste app). Fix: `static` in plaats van stack-lokaal + `_Static_assert` als vangnet.
**Volledige details:** [issue #53](https://github.com/CJvanSoest/why2025-dutchvms/issues/53)

### 5. application_list() manifest-read-race
De app-lijst toonde structureel maar een deel van de geïnstalleerde apps — deterministisch dezelfde apps faalden elke boot. Fix: `application_list()` herschreven om alle bestandsnamen eerst te snapshotten en `closedir()` te doen vóór file-opens, plus een retry-laag.
**Volledige details:** [issue #54](https://github.com/CJvanSoest/why2025-dutchvms/issues/54)

### 6. cj_wifi_analyzer R/D-hang (task-prioriteit)
Rescan/Diagnostic hingen door een systeembrede scheduling-bug: de UART-deploy-listener (prio 6, core 0) verdrong de WiFi `hermes`-taak (prio 5, core 0) volledig zodra de listener actief werd. Fix: listener-prioriteit verlaagd naar 3.
**Volledige details:** [issue #55](https://github.com/CJvanSoest/why2025-dutchvms/issues/55)

### 7. Display-backlight GPIO + PWM-curve
Brightness-tegel dimde het scherm niet echt (verkeerd GPIO getraceerd, backlight-PWM zit op de C6 niet de P4) en de PWM-curve gaf tussen 30-100% nauwelijks zichtbaar verschil. Fix: juiste GPIO (KiCad-geverifieerd) + gamma-2.2-curve. Hardware-bevestigd.
**Volledige details:** [issue #56](https://github.com/CJvanSoest/why2025-dutchvms/issues/56)

### 8. PAX app-side symbol-gaten
PAX-apps startten niet — eerste diagnose dacht aan een missende kernel-TLS-runtime, bleek onjuist: drie kleine gaten tegen de `symbols.yml`-allow-list (`__tls_get_addr`, 7 long-double-libgcc-helpers, `aligned_alloc`). Alle drie app-side opgelost zonder kernel-wijziging. Hardware-bevestigd, PR #18 gemerged.
**Volledige details:** [issue #57](https://github.com/CJvanSoest/why2025-dutchvms/issues/57)

### 9. cj_launcher PAX splash-performance
Boot-splash-animatie liep hortend. Twee rondes CPU-tekenkosten-optimalisatie hadden nul effect — de echte bottleneck was een vaste 60ms `usleep()` na elke frame plus een gehalveerde rotatie-teller. Fix: 60ms→15ms + rotatie niet meer halveren. Hardware-bevestigd.
**Volledige details:** [issue #58](https://github.com/CJvanSoest/why2025-dutchvms/issues/58)

---

## Open / gedeeltelijk opgelost

### 11. P4-OTA wordt nooit bevestigd (validate_ota_partition)
De OTA-partitie werd nooit als geldig gemarkeerd ondanks dat het nieuwe image prima draaide — vier onafhankelijke instrumentatiekanalen (UART, SD, NVS-vlag, NVS-stack-watermark) lieten allemaal geen enkel bewijs zien dat de bevestigingscode zelf uitvoert. **Workaround toegepast**: bootloader-rollback uitgezet (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` uit) sinds v1.3.17 — OTA-updates werken sindsdien betrouwbaar. De onderliggende mystery is niet gevonden; dat blijft open en vereist een JTAG-sessie.
**Volledige details:** [issue #59](https://github.com/CJvanSoest/why2025-dutchvms/issues/59)

### 12. C6-radio herflasht soms elke boot
Na een P4-reflash begon de C6-radio zichzelf elke boot opnieuw te flashen ondanks correcte MD5-pairing — een app-level resync via de launcher loste het niet op. **Workaround**: direct een esptool-bin-flash van de C6 vanaf de NAS (buiten de app-level flash-logica om) werkt betrouwbaar. De onderliggende bug in `slave_c6_flasher.c`'s eigen flash-then-verify-logica is niet gevonden.
**Volledige details:** [issue #60](https://github.com/CJvanSoest/why2025-dutchvms/issues/60)

### 13. Deploy PUT: OOM + UART-overrun bij grote bestanden
Twee samenhangende bugs in dezelfde codepath. De OOM-bug (malloc van het hele frame) is gefixt via een streaming-herschrijving, maar die PR is niet gemerged — geblokkeerd door een losstaande, niet cleanly geïsoleerde intermitterende heap-crash bij vroege boot. Los daarvan crasht een groot bestand (>100KB) de badge nog steeds via een vermoede UART RX FIFO-overflow tijdens SD-writes — twee mitigatiepogingen (kleinere chunks, `ftruncate` vooraf) losten dit niet op.
**Volledige details:** [issue #61](https://github.com/CJvanSoest/why2025-dutchvms/issues/61)

### 14. Factory-flash wist WiFi-credentials
Een volledige/factory esptool-flash wist de NVS-partitie (WiFi-credentials). **Workaround**: bij niet-destructieve updates wordt bewust alleen de app-partitie geflasht (offset 0x10000), NVS blijft dan ongemoeid. Geen structurele preventie (bv. automatische backup/restore) voor het geval een echte factory-flash toch nodig is.
**Volledige details:** [issue #62](https://github.com/CJvanSoest/why2025-dutchvms/issues/62)

### 15. About-tile crash na firmware-update
About-scherm crasht incidenteel direct na een firmware-update. Nog geen gerichte root-cause-diagnose gedaan; mogelijk (deels) gerelateerd aan issue #11's OTA-`PENDING_VERIFY`-status. Komt per 2026-08-05 nog incidenteel voor, maar minder vaak dan eerder.
**Volledige details:** [issue #63](https://github.com/CJvanSoest/why2025-dutchvms/issues/63)

### 16. Losse render-stipjes op Home-tiles
Render-artefacten op de Apps/Storage-tiles waarvan de positie tussen frames verschuift. Nog niet onderzocht.
**Volledige details:** [issue #64](https://github.com/CJvanSoest/why2025-dutchvms/issues/64)
