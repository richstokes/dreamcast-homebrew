# Five license-first sample games

These are real third-party cartridges, not rewritten approximations. The menu
loads all five directly from the embedded ROM disk, without downloads.

| Game | Creator | License | GitHub stars when checked | Exercises |
| --- | --- | --- | --- | --- |
| [Anteform](https://github.com/Feneric/Anteform) | Eric W. Brown / Feneric | GPL-3.0 | 26 | RPG, maps, coroutines, music, cartdata |
| [Pikoralli](https://github.com/nothke/pikoralli) | Ivan Notaros | MIT | 19 | Racing, 60 Hz updates, rotation/math, sound |
| [Elephant in the Room](https://github.com/ekaktusz/elephant) | Csaba Ekart and contributors | MIT | 9 | Puzzle game, sprites, palette/memory tricks |
| [Bull Sheep](https://github.com/derpycoder/bull-sheep) | Abhijit Kar | MIT | 7 | Action game, scrolling map, collisions, input |
| [Picolumia](https://github.com/andrewedstrom/picolumia) | Andrew Edstrom | MIT | 6 | Block puzzle, compressed PNG cart, audio |

Checked 2026-09-24. Selection used GitHub searches for `pico-8` / `pico8` with
MIT and GPL licenses, sorted by stars, followed by inspection of the actual
cartridges and license files. Stars are a limited popularity proxy, not a
global PICO-8 ranking. These are **not claimed to be the five most popular
PICO-8 games overall**. The unrestricted-license requirement rules out many
better-known carts with noncommercial terms, missing licenses or restricted
assets. Engines, visual-only demos, incomplete cartridge sets and obvious
third-party asset restrictions were also excluded.

Both MIT and GPL permit commercial use; GPL's source-sharing conditions still
apply. Preserve each game's license and attribution. The complete interpreted
source is included in each `.p8`, with Picolumia's readable source separately in
`source/picolumia.p8`. The PNG is the author's checked-in playable release;
the readable `.p8` comes from the same repository revision. Their packaging
need not be byte-identical. Elephant's `#include` files and Picolumia's readable
source are flattened without changing gameplay code. Anteform is playable,
but its author describes the story as not yet winnable.

[manifest.json](manifest.json) records exact upstream revisions, cartridge
paths, licenses, changes and SHA-256 hashes. [../licenses/](../licenses/) contains
the unmodified upstream license texts and available README attribution.
No upstream HTML/JavaScript exports (which embed the proprietary web runtime)
are included.

Run `uv run tests/check_samples.py` from the project directory to verify the
checked-in bundle. Normal builds are fully offline and do not need Python.
