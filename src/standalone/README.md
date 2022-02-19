Standalone Support
====

This directory serves as a home for minimal polyfill of some common APIs within
Chromium, `//base` in particular. This allows the ipcz implementation to depend
on a small subset of the Chromium tree when building against Chromium, but to
still build and behave as expected when used as a standalone library.

Currently only a subset of logging macros are polyfilled here. Internal ipcz
code which includes `debug/log.h` will get Chromium's `base/logging.h`
definitions when building as part of Chromium; when building standalone, this
will instead include `standalone/base/logging.h`.

Any additional polyfill in this directory should be added sparingly and with
careful consideration of maintenance costs, since the vast majority of the
Chromium tree does not expose stable APIs. Log macros, for example, have been
extremely consistent over the lifetime of the Chromium project and are likely to
to remain that way, so they're a reasonably good fit here.
