# Stuff that should be fixed

* events are only delivered once per frame, up to 10. Should be more granular.
* We include the `select()` system call, but it only kind of works.
* It seems that LWIP allocates in the task context, but then frees in the LWIP task context. This causes a heap corruption because the free is attempted with a different dlmalloc heap. Work around by not using spiram for this for now.
* There is a data race in thread/process creation and a process getting killed while the message is still in flight towards Zeus, the thread will leak.
* Writing to and reading from flash should be handled by a kernel task
* There are some sequencing problems in the wifi connect/disconnect code
* bmi270 currently only one axis is reported
* bmi270 if the device is busy we return stale results, should just wait until the next cycle instead
* restructure compositor to be a bit easier to deal with
* scaled window content will always appear at the top of the windows, there's no pillar boxing or letter boxing
* single buffered windows could be better
* we should never allow tasks to hold FreeRTOS synchtonization primitives, if the task gets killed FreeRTOS will just randomly kill a different task in retaliation after a timeout
* what `WINDOW_FLAG_FLIP_HORIZONTAL` means is currently hardcoded for the why2025 badge
* the ~10s black screen before the launcher's boot splash appears has no progress feedback at all -- Nicolai-Electronics/tanmatsu-launcher shows a startup_dialog() at every boot stage instead of a silent blank screen; see GitHub issue #96. Infrastructure exists (badgevms/boot_progress.c/.h, still built) and PANEL0 is already registered early enough to use it -- pulled from why2025_firmware.c's call sites for now, not deleted. Blocker found on hardware: the display backlight is driven by the C6 co-processor, which gets a mandatory hard reset partway through WIFI0 bring-up (slave_c6_flasher.c's C6_POST_RESET_SETTLE_MS, ~3s) -- anything drawn in that window is undrawable regardless of framebuffer content, no matter how the text itself is rendered/positioned/held. Whoever picks this back up should design around that window (e.g. only draw before it starts and after it's confirmed settled) rather than trying to paper over it again.

# Notes

* esp-idf assumes it owns FreeRTOS TLS slot 0 and will just overwrite whatever is there.
* If you disable the `CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS` setting then lwIP will leak memory if the task that used it exits.
* The ESP32P4 PPA hardware will hard crash if you feed if blocks where `if (height > 32 && (height % 32) == 1)`.
* The ESP32P4 can run its PSRAM at 200MHz, but when you actually do that on some device single bit errors will happen with heavy memory i/o. There does not seem to be a workaround.

