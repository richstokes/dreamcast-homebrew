#pragma once
#include <cstdint>
#include <string>

bool dc_menu_requested();
void dc_reset_input();
void dc_poll_audio();
void dc_audio_enable(bool enabled);
void dc_set_test_input(uint8_t buttons);
uint64_t dc_audio_samples();
uint64_t dc_audio_nonzero();
bool dc_ready();
