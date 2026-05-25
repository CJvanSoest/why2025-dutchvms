#pragma once

#include "esp_err.h"

/* Internal init — called once at boot from wifi.c after esp-hosted is up.
 * The public app-facing API lives in <badgevms/lora.h>. */
esp_err_t lora_proto_client_init(void);
