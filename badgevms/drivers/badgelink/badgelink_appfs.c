
// SPDX-Copyright-Text: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

/* Stub for the upstream badgelink AppFS request handlers.
 *
 * Upstream badgelink_appfs.c (badgeteam/esp32-component-badgelink) implements
 * these against Nicolai Electronics' "appfs" component — a dedicated flash
 * partition + slug-addressed app store used by MCH2022/Tanmatsu-style
 * launchers. BadgeVMS/DutchVMS has no such component: apps live as ELF files
 * on SD0:/FLASH0: addressed by VMS-style paths (see application.c,
 * deploy_protocol.c) and are deployed via this project's own deploy_protocol.
 *
 * Rather than dragging in the unrelated "appfs" component (and its own
 * flash-partition assumptions) just to satisfy badgelink.c's function
 * references, every AppFS request is answered with StatusNotSupported. The
 * SD0/FLASH0 filesystem is already fully reachable through the FS request
 * family in badgelink_fs.c, which is what this port is actually for. */

#include "badgelink_appfs.h"

#include "badgelink_internal.h"

void badgelink_appfs_handle() {
    badgelink_status_unsupported();
}

void badgelink_appfs_xfer_upload() {
}

void badgelink_appfs_xfer_download() {
}

void badgelink_appfs_xfer_stop(bool abnormal) {
    (void)abnormal;
}

void badgelink_appfs_list() {
    badgelink_status_unsupported();
}

void badgelink_appfs_delete() {
    badgelink_status_unsupported();
}

void badgelink_appfs_upload() {
    badgelink_status_unsupported();
}

void badgelink_appfs_download() {
    badgelink_status_unsupported();
}

void badgelink_appfs_stat() {
    badgelink_status_unsupported();
}

void badgelink_appfs_crc32() {
    badgelink_status_unsupported();
}

void badgelink_appfs_usage() {
    badgelink_status_unsupported();
}
