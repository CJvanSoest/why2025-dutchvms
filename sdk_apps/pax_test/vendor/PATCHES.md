# Local patches against upstream `robotman2412/pax-graphics`

Vendored as a plain source drop, no submodule. Track any local delta here so
a future re-vendor doesn't silently drop it.

## `core/src/pax_gfx.c`

`static __thread pax_err_t last_err` -> `static pax_err_t last_err`.

BadgeVMS apps are position-independent shared objects; `__thread` forces
GCC's general-dynamic TLS model, which calls `__tls_get_addr` -- a
dynamic-loader symbol BadgeVMS' ELF loader doesn't implement (no TLS block,
DTV, or per-task `tp` setup). `last_err` only backs `pax_get_err()`/
`pax_set_err()`, a diagnostic "last error" register with no cross-task
sharing requirement in this prototype, so a process-global is a safe
substitute. See task #83's investigation notes for the full analysis.
