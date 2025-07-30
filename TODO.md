# Stuff that should be fixed

* window_framebuffer_free doesn't "move" the other framebuffer
* events are only delivered once per frame, and then only one input. 
* clean frames don't get re-blitted but due to lack of occlusion culling this causes graphical glitches
* We include the `select()` system call, but it only kind of works.
* Memory leak in run_task, we need to get to a "callee pays" model for task creation. The leak is small enough to not worry about it immediately.

