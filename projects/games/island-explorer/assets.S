.section .rodata
.balign 32
.global _coast0_start, _coast0_end
_coast0_start:
.incbin "assets/generated/coast0.bin"
_coast0_end:
.balign 32
.global _coast1_start, _coast1_end
_coast1_start:
.incbin "assets/generated/coast1.bin"
_coast1_end:
.balign 32
.global _textures_start,_textures_end
_textures_start:
.incbin "assets/generated/textures.bin"
_textures_end:
.balign 32
.global _sky_start,_sky_end
_sky_start:
.incbin "assets/generated/sky.bin"
_sky_end:
.balign 32
.global _ambience_start,_ambience_end
_ambience_start:
.incbin "assets/generated/ambience.pcm"
_ambience_end:
.balign 32
.global _font_start
_font_start:
.incbin "assets/generated/font.bin"

.balign 32
.global _foam0_start,_foam1_start
_foam0_start:
.incbin "assets/generated/foam0.bin"
.balign 32
_foam1_start:
.incbin "assets/generated/foam1.bin"

.balign 4
.global _sh4zam_license_start
_sh4zam_license_start:
.incbin "third_party/sh4zam/LICENSE"
.byte 0
