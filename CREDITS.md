# Credits

This repository (why2025-dutchvms) is a fork of Team:Badge's BadgeVMS firmware
and is licensed under **GPL-3.0-or-later** (see COPYING) — the same license
as upstream, as required for a GPL derivative work. It is not MIT; the
separate [why2025-apps](https://github.com/CJvanSoest/why2025-apps) repository
(PIE-ELF apps + tooling, not a GPL derivative) is MIT instead.

| Component | Source | License |
|---|---|---|
| BadgeVMS OS / compositor / launcher base / firmware architecture | [Team:Badge / badge.team](https://github.com/badgeteam) | GPL-3.0-or-later |
| ELF loader, badgelink | badgeteam `esp32-component-badge-elf`, `esp32-component-badgelink` | see upstream |
| LoRa radio driver (SX126x) | [Nicolai Electronics](https://github.com/Nicolai-Electronics) — Tanmatsu LoRa port | see upstream |
| MeshCore protocol | [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore) (upstream protocol) | — |
| Display (ST7703), sensors (BMI270, BME690) | Espressif / Bosch reference drivers | see upstream |
| Vendored libraries (where used verbatim) | lodepng, qrcodegen, ed25519 | each under its own license — see the vendored source |
| ESP-IDF, ESP-Hosted, FreeRTOS-Kernel, SDL, and other `components/`/`sdk_libs/` dependencies | Espressif Systems and respective upstream projects | see each component's own LICENSE |
| DutchVMS-specific changes and additions | CJ van Soest | GPL-3.0-or-later (as a derivative work) |

Please respect each dependency's original license and notices — this
project's GPL-3.0 applies to CJ van Soest's own modifications and additions;
vendored third-party code keeps its original license.
