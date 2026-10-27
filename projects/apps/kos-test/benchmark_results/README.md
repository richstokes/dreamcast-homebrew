# KOS_Test benchmark results

Console runs of KOS_Test's auto mode, each recorded against the KOS commit it
was built with.

| File | What |
| --- | --- |
| `benchmarks.csv` | One row per run: date, target, KOS version and commit, a hash of the SDK's local changes, GCC, KOS_Test commit, CPU clock, passes, test counts, then every benchmark's median and its spread between passes (`<key>_spread`, %) |
| `runs/<date>_<target>_kos-<commit>.txt` | The whole run: environment, every test line, every benchmark with its unit, spread and which way is better |

Benchmark columns are the keys in `src/bench.c`; new ones get a new column and
old rows stay blank. Most are rates where higher is better; times (`ms`) and
the stream's CPU cost (`%`) are lower-is-better, as the run file says.

## Adding a run

```sh
make auto                                  # console over dcload-ip, records here
tools/record_results.py path/to/dc-tool-ip.log    # or any log with a KT RESULTS block
tools/record_results.py --dry-run path/to/log     # just compare with the last run
```

Each run prints what changed against the previous run on the same target
(`--all` lists everything). A change only counts when it beats that
benchmark's noise: the spread between passes in either run, or between
earlier runs on the same KOS build and patches, and at least 2%. **Record a
few runs per KOS build: the more history, the better the noise estimate.**

## When one benchmark moves on its own

Small loops are at the mercy of the 8 KB direct-mapped instruction cache, and
every link moves the code around. Before blaming KOS, list the functions on
the benchmark's hot path and check both ELFs:

```sh
tools/icache_clash.py -f readdir,fs_readdir,romdisk_readdir,strncpy,strnlen \
    kos-test-auto.elf kos-test-auto-stock.elf
```

A `CLASH` in only one of them is the likely cause. That is what made romdisk
readdir ~12% slower on the stock SDK: `romdisk_readdir` and `strncpy` shared
two cache lines there and nowhere else. The next build overwrites the ELFs,
so check before rebuilding.

## Gotchas

- `kos_patches` hashes `git diff HEAD` in the KOS tree, so it changes with `tools/kos/patches`: two rows on one commit with different hashes ran different code
- Changes to the benchmarks themselves move the numbers too; check `kos_test_commit` before blaming KOS
- Runs on the stock SDK (`make auto-stock`) belong here too: they record `kos_patches none` and the upstream commit. Recording one straight after a patched run compares the two
- Only console runs belong here. `make auto-flycast` records under `build/auto/results` because Flycast's timings say nothing about the console's
- A single hung run is usually dcload-ip, not KOS: rerun it
