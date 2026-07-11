# Pitfalls

Traps that have already cost time or shipped broken behaviour on this fork.
Each one is concrete. If you're about to do the thing on the left, read the
note first. See also [Guidelines.md](Guidelines.md) and
[Data-Flows.md](Data-Flows.md).

## `clang-format -i` on `connectivity_esp_hosted/` reformats the whole file

CI's format check only globs `badgevms/*.c`/`*.h`
(`.github/workflows/ci.yml`). Running `clang-format -i` on a
`connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol_server.c`
edit reformats the **entire pre-existing file** against `badgevms/`'s style
(different brace/pointer conventions), producing a 400+ line diff for what
was meant to be a one-function fix. Caught by `git diff --stat` showing an
implausibly large diff for the described change — always check that before
committing. If it happens, `git checkout --` the file and reapply only the
targeted edit by hand, matching that file's own existing style.

## A too-small request buffer failed silently, not loudly

`lora_proto_client.c`'s `request_reply()` had its internal request buffer
sized `sizeof(lora_protocol_header_t) + 64` — enough for small control
messages, not enough for a `PACKET_TX` (`1 + LORA_MAX_PACKET_LEN`, up to 256
bytes). The overflow produced **no log line at all**, just an unexplained
"radio busy" error surfaced to the app much later. If a kernel request path
has a fixed-size buffer, check it against the actual max payload the request
type can carry, and log a clear error at the overflow site rather than
letting a caller several layers up guess. See Data-Flows.md for what this
bug was masking underneath (the C6-side off-by-one, below).

## A length-prefix byte got radiated onto the air

`lora_protocol_handle_packet()`'s `PACKET_TX` case used to pass the whole
`lora_protocol_lora_packet_t` params blob (a `uint8_t length` byte followed
by the actual payload) straight to `transmit_packet()`, instead of
unwrapping it first. That length byte went out as the first byte of the
actual over-the-air LoRa payload, shifting every subsequent byte by one.
Every packet this firmware ever transmitted had this bug — it compiled, it
linked, the radio genuinely transmitted, nothing on the P4 side errored,
and every real MeshCore receiver silently rejected the malformed packet.
The fix was found by comparing a captured raw packet byte-for-byte against
an independent reference implementation (`CJvanSoest/meshcore`'s wire
format), not by anything CI or a build could catch. When a wire-format bug
is suspected, get a real captured packet from both sides and diff it
byte-for-byte against a working implementation before guessing.

## `abort()` reboots the whole badge, not just the offending task

BadgeVMS has real per-task crash isolation (`cerberos`,
`__wrap_xt_unhandled_exception` in `task.c`) for a misbehaving *app*. A bare
`abort()` in kernel code (a "should be unreachable"
`xSemaphoreTake(..., portMAX_DELAY)` failure, say) does **not** go through
that isolation — it's a full ESP-IDF system panic, same as any other
`abort()`, and reboots the entire badge. Use `why_die("reason")`
(`badgevms/why_io.h`) instead: same fatal outcome, but it logs a diagnosable
reason via `esp_system_abort()` first. This isn't a style preference, it's
the difference between a panic log you can read afterward and a bare,
undiagnosable reboot.

## Force-killing an orphaned child while holding its parent lock

`task.c`'s `hades()` used to call `vTaskDelete()` on a dead process's
orphaned children **while holding `process_table_lock`**. `vTaskDelete()`
kills a task instantly, mid-instruction, with no chance for it to run its
own cleanup — if the killed child happened to be inside a different kernel
critical section at that exact moment (holding `device_table_lock`, say),
that lock stays orphaned forever, wedging every future caller into it. The
fix: snapshot the handles to kill while holding the lock (a cheap table
read), release the lock, *then* call `vTaskDelete()` on each. If you're
about to call `vTaskDelete()`/`vTaskSuspend()`/anything that can interrupt a
task mid-instruction from inside a locked section, ask what that task might
currently be holding.

## The deploy protocol trusts its own path decoding

`deploy_protocol.c`'s `vms_to_posix()` used to copy device/directory/
filename segments verbatim with no rejection of `.`/`..` segments, and every
handler (`handle_put`/`handle_get`/`handle_list`/`handle_delete`, including
the **recursive** delete) trusted its output. A crafted VMS path like
`SD0:[..][..][flash_storage]` would have translated straight into a path
escaping the intended SD/flash sandbox. Any code that turns an
externally-supplied path into a filesystem path needs the traversal check
at the point of translation, not scattered across each caller — see the fix
in `vms_to_posix()` for the pattern (reject any `.`/`..` path segment before
returning).

## An app's manifest and its install directory are stored as siblings

`get_metadata_path()` writes `<uid>.json` into `applications_base_dir`
directly, not into the app's own `installed_path`. `application_destroy()`
used to only `rm_rf()` the install directory, leaving the manifest behind —
`application_list()` would then keep finding and successfully parsing the
orphaned manifest on every subsequent enumeration (it only checks the JSON
is valid, not that `installed_path` still exists), showing a ghost entry in
the launcher, and `application_create()` for the same `unique_identifier`
would keep refusing with "already exists" forever (it checks for the
manifest file, not the directory). Any code path that removes an app must
remove both; any code that creates one must expect both to already
potentially be gone independently.

## `application_list()` can transiently see an empty file that isn't empty

Enumerating `applications_base_dir` while a manifest is mid-write (or the SD
card is just slow) can return `0` bytes read for a file that has real
content moments later — not a corruption, a read race. `application_list()`
snapshots the directory listing with `opendir()` before opening any
individual file, and retries a failed parse up to 3 times before skipping
that entry, rather than either crashing or silently dropping it on the
first bad read. If you're enumerating anything on the SD card, expect this
class of transient failure and retry before giving up.

## A large struct as a stack-local variable compiles clean, then blows the stack

A `Launcher_Context`-sized struct declared as a plain stack-local variable
compiled without warning and then genuinely overflowed the task's stack at
runtime on a physical badge — requiring the SD card to be physically pulled
to recover, since it happened during boot. `-fstack-usage` (the CI
stack-usage gate, see Build-And-CI.md) catches this class of bug *if you run
it*, but the compiler itself gives no warning at the point of declaration.
Prefer `static` for large context structs in a long-lived function, and
always verify against the actual target build (not just "it compiled")
before flashing.

## `sdkconfig` can get corrupted by a stale ESP_HOSTED_CP_TARGET value

Symptoms: WiFi init fails, the C6 is reported unavailable, and firmware
version queries against it fail — even though the C6 is physically fine.
Check `ESP_HOSTED_CP_TARGET` in `sdkconfig` against the C6+SDIO
configuration expected for this board. Recovery: delete the stale
`sdkconfig_tanmatsu`-equivalent (or the relevant cached sdkconfig) and do a
clean rebuild rather than trying to hand-edit the corrupted value — this
config gets regenerated from `sdkconfig.defaults` and is not meant to be
edited directly.

## The UART deploy protocol's GET/LIST/DELETE paths still malloc a whole payload

`deploy_protocol.c`'s `handle_put()` used to malloc the entire incoming
payload as one contiguous block, and app binaries growing past what a
long-uptime badge's fragmented heap could still supply in one block hit
`ERR_OOM` well below any documented size cap (why2025-apps#1
hardware-test feedback: an app hit this at ~155KB). PUT now reads+CRCs+
writes its payload in bounded chunks straight off the wire
(`handle_put_streamed()`) instead — RAM use is a small constant regardless
of file size. GET/LIST/DELETE's payloads are still handled through the
generic malloc-the-whole-payload path in `process_one_frame()` (a bare
path for LIST/DELETE, the whole file for GET) since nothing has hit a wall
on those yet — if GET ever does for a large downloaded file, the same
streaming approach applies there too. The C6's own ~1.2MB
`network_adapter.bin` still goes through its direct-flash path
(`docs/guides/Flashing.md`) regardless — that's a different mechanism
entirely, not something PUT was ever meant to carry.

## USB port identity changes after a flash+reset, and that's expected

The badge has two USB-C ports on two different chips (side = P4/CH340 =
`/dev/ttyUSB0`, bottom = C6/native-USB = `/dev/ttyACM0` on Linux — see
`docs/guides/Flashing.md`). After flashing one chip and letting `esptool`
hard-reset it, don't assume the same device path is still valid or that
only one port is ever connected at a time — check `ls
/dev/ttyUSB*`/`/dev/ttyACM*` again before the next command rather than
reusing a cached port name. A DTR/RTS toggle sequence copied from a
different board's reset procedure can also put a chip into
download/bootloader mode instead of doing a clean app reset — if a boot-log
capture comes back with `waiting for download` instead of normal boot
output, that's what happened; reset cleanly with `esptool ... chip_id`
(or any no-op esptool command with `--after hard_reset`) rather than a raw
serial-line toggle.
