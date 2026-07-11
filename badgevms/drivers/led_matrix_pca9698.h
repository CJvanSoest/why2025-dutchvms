/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Coordination surface between board_bringup.c (PCA9698 presence detection)
 * and led_matrix_pca9698.c (the matrix driver itself). App-facing control
 * lives in drivers/led_matrix_internal.h instead - this header is only for
 * the two driver-side .c files to talk to each other. */
#pragma once

#include <stdint.h>

/* Low-level PCA9698 bit-bang readback probe, shared with board_bringup.c's
 * presence-detection sweep across candidate SDA/SCL pin pairs and I2C
 * addresses. Returns the read-back register value, or -1 on NACK/failure. */
int bb_pca_readback(int sda, int scl, uint8_t addr7, uint8_t val);

/* Called by board_bringup.c once the PCA9698 has been confirmed present.
 * Independently re-probes which SDA/SCL pin order ACKs at MTX_ADDR (same
 * as the original inline logic this was extracted from -- not just reusing
 * board_bringup.c's earlier sweep result), then brings up the matrix
 * refresh task (hardware I2C peripheral if available, else the bit-bang
 * fallback) and starts the boot-time demo animation. */
void led_matrix_pca9698_start(void);
