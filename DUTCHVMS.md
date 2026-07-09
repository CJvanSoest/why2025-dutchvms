# DutchVMS — CJ's fork van BadgeVMS

Dit is een fork van de [WHY2025 BadgeVMS firmware](https://gitlab.com/why2025/team-badge/firmware) met aanpassingen voor mijn dev-workflow en eigen launcher branding.

## Wijzigingen vs upstream

### 1. UART-deploy protocol (`badgevms/deploy_protocol.{c,h}`)
Kernel-task listener op UART0 RX die een CRC-framed binary protocol implementeert. Maakt het mogelijk om apps over de side-USB-C (CH340) naar de SD-kaart te schrijven zonder de P4-module los te schroeven.

- Wire format: `[MAGIC: DE AD BE EF][CMD:1][LEN:4 LE][PAYLOAD:N][CRC16:2 LE]`
- CRC-16/X.25 (`esp_rom_crc16_le`-compatibel)
- Commands: PUT (file write), PING (version probe)
- mkdir-p auto-create voor parent directories
- Host-side client: `WHY2025-apps/tools/badge_deploy.py`

Init hook in `why2025_firmware.c` (na device-init, vóór `run_init`).

### 2. CJ-PATCH in `badgevms/drivers/wifi.c`
Compile-time switch `CJ_BADGEVMS_ENABLE_WIFI` (default 0) die `flash_slave_c6_if_needed()` overslaat. Hiermee blijft custom C6-firmware (MeshCore) intact bij elke reboot in plaats van te worden overschreven met ESP-Hosted.

- Returnt nog steeds een valide `device_t*` (NULL → OTA rollback)
- Status: `WIFI_DISABLED`
- WiFi-features werken niet wanneer dit aanstaat, maar LoRa/MeshCore wel

### 3. Splash + branding
De launcher app (in `WHY2025-apps/apps/cj_launcher`) toont een DutchVMS-splash met geanimeerde wireframe-windmolen vóór het home-scherm. Geen firmware-wijziging, maar wel de visuele identiteit van deze fork.

## Build

Standaard ESP-IDF 5.5 workflow:

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.wchusbserial210 flash
idf.py sdk                    # eenmalig — produceert sdk_dist/ voor app-builds
```

## Remotes

- `origin` → [why2025-dutchvms](https://github.com/CJvanSoest/why2025-dutchvms) (GitHub, private dev)
- `upstream` → WHY-team gitlab (`https://gitlab.com/why2025/team-badge/firmware.git`)

Upstream-changes mergen:
```bash
git fetch upstream
git rebase upstream/main         # of merge — afhankelijk van situatie
git push origin
```

## Apps + tooling

De cj_apps en deploy-tool wonen in de [why2025-apps](https://github.com/CJvanSoest/why2025-apps) repo — apart gehouden om deze fork een schone diff vs upstream te laten houden.
