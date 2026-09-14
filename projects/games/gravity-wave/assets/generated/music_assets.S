    .section .rodata
    .balign 32
    .global _gravity_wave_music_assets
    .global _gravity_wave_music_assets_end

_gravity_wave_music_assets:
    .incbin "assets/generated/gravity_wave_music.adpcm"
_gravity_wave_music_assets_end:
    .balign 32
