// WHY2025 badge hardware definitions for the ESP32-C6 radio module
// Ported from Tanmatsu (Nicolai Electronics) — pinning herschreven voor WHY badge
// SPDX-License-Identifier: MIT

#pragma once

// LoRa radio module pins (WHY badge C6 — schematic Connectivity (C6 + LoRa))
#define BSP_LORA_SCK   6
#define BSP_LORA_CS    4
#define BSP_LORA_MOSI  7
#define BSP_LORA_MISO  2
#define BSP_LORA_DIO1  5
#define BSP_LORA_BUSY  11
#define BSP_LORA_RESET 1
#define BSP_LORA_TXEN  3   // WHY-only: TX-enable switch (must be HIGH for TX path)
#define BSP_LORA_BUS   SPI2_HOST

// WHY badge backlight pins (driven by C6 via LEDC PWM)
#define BSP_DISPLAY_BL_GPIO  15
#define BSP_KEYBOARD_BL_GPIO 10

// Host interface pins (best-effort matched; verify against WHY schematic before BT-over-UART use)
#define BSP_HOST_INT  8
#define BSP_HOST_BOOT 9
#define BSP_HOST_TX   16
#define BSP_HOST_RX   17

// Infrared TX pin (Tanmatsu had GPIO15 — conflicts with WHY display backlight).
// WHY badge has no documented IR-TX pad; keep define for source-compat, expect IR-init to fail or skip.
#define BSP_IR_TX     -1

// SDIO pins (P4 <-> C6 high-bandwidth link — verify against WHY schematic before flash)
#define BSP_SDIO_CMD 18
#define BSP_SDIO_CLK 19
#define BSP_SDIO_D0  20
#define BSP_SDIO_D1  21
#define BSP_SDIO_D2  22
#define BSP_SDIO_D3  23
