Vendored from https://github.com/Dreamcast-Projects/pl_mpeg at 24b588bd6a63e8dae58b45c030a2018c381a31ab
(the SH-4 port of https://github.com/phoboslab/pl_mpeg). MIT licensed, see LICENSE.
LemonPlay only uses the MPEG-1 video decoder.

Local change: `plm_sq_copy_bytes` is a plain memcpy. The store-queue version
wrote RAM behind the cache and corrupted the stream on real hardware.
