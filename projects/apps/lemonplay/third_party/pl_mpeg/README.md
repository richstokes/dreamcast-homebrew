Vendored from https://github.com/Dreamcast-Projects/pl_mpeg at 24b588bd6a63e8dae58b45c030a2018c381a31ab
(the SH-4 port of https://github.com/phoboslab/pl_mpeg). MIT licensed, see LICENSE.
LemonPlay only uses the MPEG-1 video decoder.

Local changes (each marked "LemonPlay" in pl_mpeg.h):

- `plm_sq_copy_bytes` is a plain memcpy: the store-queue version wrote RAM
  behind the operand cache
- DCT coefficients decode with a 9-bit lookup table
  generated from the tree tables, with the coefficient sign folded in
- Each macroblock's display bytes are claimed with `movca.l`, skipping the
  write-allocate read of lines that are about to be overwritten
- `LP_PROFILE` builds count cycles per decoding stage

Output is bit-identical to the original port (LemonPlay's decode benchmark
hashes every picture).
