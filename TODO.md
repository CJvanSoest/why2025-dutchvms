# Stuff that should be fixed

* `window_framebuffer_free` doesn't "move" the other framebuffer
* events are only delivered once per frame, and then only one input. 
* clean frames don't get re-blitted but due to lack of occlusion culling this causes graphical glitches
* We include the `select()` system call, but it only kind of works.
* Memory leak in `run_task()`, we need to get to a "callee pays" model for task creation. The leak is small enough to not worry about it immediately.
* esp-idf assumes it owns FreeRTOS TLS slot 0
* It seems that LWIP allocates in the task context, but then frees in the LWIP task context. This causes a heap corruption because the free is attempted with a different dlmalloc heap. Work around by not using spiram for this for now.

