
// SPDX-Copyright-Text: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

/* Stub for the upstream badgelink "start app" request handler.
 *
 * Upstream badgelink_startapp.c resolves an AppFS slug via appfsOpen() /
 * appfsBootSelect() and reboots into it. BadgeVMS/DutchVMS has its own
 * process model (badgevms/process.h, application.c) with no AppFS-style
 * slug addressing, so there is nothing meaningful to wire this up to yet.
 * Answering StatusNotSupported keeps badgelink.c's request dispatch honest
 * about what this port currently implements (fs + nvs) rather than silently
 * mis-mapping "start app" onto the wrong thing. */

#include "badgelink_startapp.h"

#include "badgelink_internal.h"

void badgelink_startapp_handle() {
    badgelink_status_unsupported();
}
