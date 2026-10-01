"""Blank Flycast memory cards for runners that must never touch real VMUs."""
SLOTS = [port + unit for port in 'ABCD' for unit in '12']


def blank_cards(folder):
    """Create an empty card for every slot in a new Dreamcast.VMUPath folder.

    When the custom folder lacks a card, Flycast reads the one in its default
    folder, saves it into the custom folder and deletes the original. Giving
    every slot its own file keeps real cards from being read or moved. Flycast
    formats an all-zero image when it loads it.
    """
    folder.mkdir()
    for slot in SLOTS:
        (folder / f'vmu_save_{slot}.bin').write_bytes(bytes(131072))
