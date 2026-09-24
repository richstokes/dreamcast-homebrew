#pragma once
#include <cstdint>
#include <string>

bool dc_menu_requested();
void dc_reset_input();
void dc_poll_audio();
void dc_audio_enable(bool enabled);
void dc_set_test_input(uint8_t buttons);
void dc_set_test_pad(uint32_t buttons);
uint64_t dc_audio_samples();
uint64_t dc_audio_nonzero();
uint64_t dc_video_frames();
bool dc_ready();
void dc_set_launcher(bool enabled);
bool dc_download_requested();
