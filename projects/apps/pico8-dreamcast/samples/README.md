# Sample games

Seven real third-party cartridges, bundled in the ROM disk and loaded by the
picker without downloads.

| Game | Creator | License | GitHub stars when checked | Exercises |
| --- | --- | --- | --- | --- |
| [Anteform](https://github.com/Feneric/Anteform) | Eric W. Brown / Feneric | GPL-3.0 | 26 | RPG, maps, coroutines, music, cartdata |
| [Pikoralli](https://github.com/nothke/pikoralli) | Ivan Notaros | MIT | 19 | Racing, 60 Hz updates, rotation/math, sound |
| [Elephant in the Room](https://github.com/ekaktusz/elephant) | Csaba Ekart and contributors | MIT | 9 | Puzzle game, sprites, palette/memory tricks |
| [Bull Sheep](https://github.com/derpycoder/bull-sheep) | Abhijit Kar | MIT | 7 | Action game, scrolling map, collisions, input |
| [Picolumia](https://github.com/andrewedstrom/picolumia) | Andrew Edstrom | MIT | 6 | Block puzzle, compressed PNG cart, audio |
| [NanoMan](https://github.com/FunFetched/NanoMan) | Fun Fetched (music by Gruber) | GPL-3.0 | 30 | Action platformer, 60 Hz updates, music |
| [WIZRND](https://github.com/mattleblanc/WIZRND) | mattleblanc | Apache-2.0 | 3 | Procedurally generated platformer, 30 Hz |

Picked by searching GitHub for `pico-8` / `pico8` carts under MIT, GPL and
other permissive licenses, sorted by stars, then inspecting the actual
cartridge and license files. Stars are a rough proxy; this is not a PICO-8
popularity list. Better-known carts with noncommercial terms, missing licenses
or restricted assets were excluded, as were engines, visual-only demos,
incomplete cartridge sets and PICO-CRATE (noncommercially licensed music). The
first five were checked 2026-09-24, NanoMan and WIZRND on 2026-10-16.

MIT, Apache-2.0 and GPL all permit commercial use; GPL's source-sharing
conditions still apply. Preserve each game's license and attribution. Every
`.p8` carries its complete interpreted source. Picolumia's readable source is
in `source/picolumia.p8`; the PNG is the author's checked-in playable release
from the same revision, so the packaging need not be byte-identical. No
upstream HTML/JavaScript exports are included, since they embed the
proprietary web runtime.

Per-game notes:

- Elephant's `#include` files are flattened into one cartridge; gameplay code
  is unchanged
- Anteform is playable, but its author describes the story as not yet winnable
- NanoMan's repository holds the main cartridge only; the author mentions
  unpublished map carts. The main cart makes no `reload`/`cstore` calls and
  passes the smoke test; finishing the whole game was not checked
- WIZRND is renamed to `wizrnd.p8`, contents unchanged

[manifest.json](manifest.json) records the exact upstream revisions, cartridge
paths, licenses, changes and SHA-256 hashes. [../licenses/](../licenses/)
holds the unmodified upstream license texts and available README attribution.
`uv run tests/check_samples.py` (from the project directory; CI runs it too)
verifies the checked-in bundle. Normal builds are offline and need no Python.
