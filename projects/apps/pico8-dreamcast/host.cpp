// Dreamcast host for FAKE-08. KOS owns video, Maple input and AICA streaming.
#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/maple/keyboard.h>
#include <algorithm>
#include <array>
#include <map>
#include <cstring>
#include "host.h"
#include "filehelpers.h"
#include "platform.h"
#include "video_convert.h"

namespace {
pvr_ptr_t texture = nullptr;
pvr_poly_hdr_t polygon;
alignas(32) uint16_t pixels[128 * 128];
alignas(32) int16_t samples[4096];
// KOS rounds stream requests to 2048-byte sectors. A 4096-byte ring gives
// 1024-sample refills instead of the old 2048-sample synthesis stalls.
constexpr size_t audio_buffer_bytes = 4096;
alignas(32) uint8_t previous_pixels[8192];
uint16_t previous_colors[16];
bool frame_valid = false;
uint8_t previous_mode = 0;
int previous_size = 0;
uint64_t submitted_frames = 0;
snd_stream_hnd_t stream = SND_STREAM_INVALID;
Audio* synth = nullptr;
bool initialized = false, sound_initialized = false, sound_enabled = false;
bool menu_requested = false, chord_held = false;
bool launcher = false, download_requested = false, download_held = false;
bool release_input = false;
uint8_t previous_buttons = 0, test_buttons = 0;
bool test_input = false;
bool test_pad = false;
cont_state_t test_pad_state{};
uint64_t deadline = 0, previous_frame = 0, sample_count = 0, nonzero_count = 0;
uint64_t frame_us = 33333;
double frame_delta_ms = 33.333;
int display_size = 384;
std::map<std::string, std::string> session_saves;

void* audio_callback(snd_stream_hnd_t, int requested, int* received) {
    // KOS 2.3 snd_stream_fill passes bytes, despite stream.h calling these
    // arguments samples. See kernel/arch/dreamcast/sound/snd_stream.c.
    const int count = std::min(requested / int(sizeof(int16_t)), int(sizeof(samples) / sizeof(samples[0])));
    if (sound_enabled && synth) {
        synth->FillMonoAudioBuffer(samples, 0, count);
        sample_count += count;
        for (int i = 0; i < count; ++i) nonzero_count += samples[i] != 0;
    } else {
        memset(samples, 0, count * sizeof(samples[0]));
    }
    *received = count * sizeof(int16_t);
    return samples;
}

void append_carts(const char* directory, std::vector<std::string>& carts) {
    file_t fd = fs_open(directory, O_RDONLY | O_DIR);
    if (fd < 0) return;
    while (const dirent_t* entry = fs_readdir(fd)) {
        std::string name = entry->name;
        if (entry->size >= 0 && !name.empty() && name[0] != '.' &&
            (hasEnding(name, ".p8") || hasEnding(name, ".p8.png")))
            carts.push_back(std::string(directory) + "/" + name);
    }
    fs_close(fd);
}
}

bool dc_ready() { return initialized; }
void dc_set_launcher(bool enabled) { launcher = enabled; }
bool dc_download_requested() { bool result = download_requested; download_requested = false; return result; }
uint64_t dc_audio_samples() { return sample_count; }
uint64_t dc_audio_nonzero() { return nonzero_count; }
uint64_t dc_video_frames() { return submitted_frames; }
void dc_set_test_input(uint8_t buttons) { test_input = true; test_buttons = buttons; }
void dc_set_test_pad(uint32_t buttons) { test_pad = true; test_pad_state.buttons = buttons; }
bool dc_menu_requested() {
    bool value = menu_requested;
    menu_requested = false;
    return value;
}
void dc_reset_input() { previous_buttons = 0; deadline = 0; release_input = true; frame_valid = false; }
void dc_poll_audio() { if (stream != SND_STREAM_INVALID) snd_stream_poll(stream); }
void dc_audio_enable(bool enabled) { sound_enabled = enabled; }

Host::Host(int, int) : currKDown(0), currKHeld(0) {
    _cartDirectory = "/rd/carts";
    _logFilePrefix = "/ram/";
}

void Host::oneTimeSetup(Audio* audio) {
    synth = audio;
    vid_set_mode(DM_640x480, PM_RGB565);
    if (pvr_init_defaults() < 0) { printf("PICO8: PVR initialization failed\n"); return; }
    texture = pvr_mem_malloc(sizeof(pixels));
    if (!texture) { printf("PICO8: texture allocation failed\n"); pvr_shutdown(); return; }
    pvr_poly_cxt_t context;
    pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
        PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 128, 128, texture, PVR_FILTER_NONE);
    pvr_poly_compile(&polygon, &context);
    pvr_set_bg_color(0.008f, 0.016f, 0.031f);
    initialized = true;
    frame_valid = false;
    if (snd_stream_init_ex(1, audio_buffer_bytes) == 0) {
        sound_initialized = true;
        stream = snd_stream_alloc(audio_callback, audio_buffer_bytes);
        if (stream != SND_STREAM_INVALID) {
            snd_stream_start(stream, 22050, 0);
            snd_stream_volume(stream, 220);
        }
    }
    printf("PICO8: video=640x480 texture=128x128 audio=%s\n",
        stream == SND_STREAM_INVALID ? "unavailable" : "22050Hz mono / four synth channels");
    if (!maple_enum_type(0, MAPLE_FUNC_CONTROLLER))
        printf("PICO8: no controller; connect one to navigate\n");
}

InputState_t Host::scanInput() {
    uint8_t held = 0;
    if (launcher) {
        auto keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        if (keyboard) {
            int key;
            while ((key = kbd_queue_pop(keyboard, true)) != KBD_QUEUE_END)
                if (key == 'y' || key == 'Y') download_requested = true;
        }
    }
    maple_device_t* device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    auto* pad = device ? static_cast<cont_state_t*>(maple_dev_status(device)) : nullptr;
    if (test_pad) pad = &test_pad_state;
    if (pad) {
        if ((pad->buttons & CONT_DPAD_LEFT) || pad->joyx < -48) held |= P8_KEY_LEFT;
        if ((pad->buttons & CONT_DPAD_RIGHT) || pad->joyx > 48) held |= P8_KEY_RIGHT;
        if ((pad->buttons & CONT_DPAD_UP) || pad->joyy < -48) held |= P8_KEY_UP;
        if ((pad->buttons & CONT_DPAD_DOWN) || pad->joyy > 48) held |= P8_KEY_DOWN;
        if (pad->buttons & (CONT_A | CONT_X)) held |= P8_KEY_O;
        if (pad->buttons & (CONT_B | (launcher ? 0 : CONT_Y))) held |= P8_KEY_X;
        bool download = launcher && (pad->buttons & CONT_Y);
        if (download && !download_held) download_requested = true;
        download_held = download;
        bool chord = pad->ltrig > 160 && pad->rtrig > 160 && (pad->buttons & CONT_START);
        if (chord && !chord_held) menu_requested = true;
        chord_held = chord;
        if ((pad->buttons & CONT_START) && !chord) held |= P8_KEY_PAUSE;
        if (chord) held = 0;
    } else { chord_held = false; download_held = false; }
    if (test_input) held = test_buttons;
    if (release_input) {
        if (!held) release_input = false;
        held = 0;
    }
    InputState_t state{};
    state.KDown = held & ~previous_buttons;
    state.KHeld = held;
    previous_buttons = held;
    return state;
}

void Host::setTargetFps(int fps) {
    const uint64_t period = 1000000 / (fps == 60 ? 60 : 30);
    if (period != frame_us) { frame_us = period; deadline = 0; }
}
void Host::waitForTargetFps() {
    uint64_t now = timer_us_gettime64();
    if (!deadline || now > deadline + frame_us * 3) deadline = now;
    while (now < deadline) {
        dc_poll_audio();
        thd_sleep(1);
        now = timer_us_gettime64();
    }
    frame_delta_ms = previous_frame ? (now - previous_frame) / 1000.0 : frame_us / 1000.0;
    previous_frame = now;
    deadline += frame_us;
}
double Host::deltaTMs() { return frame_delta_ms; }
bool Host::shouldRunMainLoop() { return initialized; }
bool Host::shouldQuit() { return false; }
void Host::changeStretch() {}
void Host::forceStretch(StretchOption option) { display_size = option == StretchToFit ? 480 : 384; }

void Host::drawFrame(uint8_t* framebuffer, uint8_t* palette, uint8_t mode) {
    uint16_t colors[16];
    for (unsigned i = 0; i < 16; ++i) {
        auto color = _paletteColors[palette[i] & 0x8f];
        colors[i] = ((color.Red >> 3) << 11) | ((color.Green >> 2) << 5) | (color.Blue >> 3);
    }
    // 30-Hz carts yield twice per draw. Retain the displayed PVR frame when
    // pixels, palette, transform and scale are unchanged; still tick the VM
    // and service input/audio at the normal rate. Modal screens invalidate it.
    if (frame_valid && previous_mode == mode && previous_size == display_size &&
        !memcmp(previous_colors, colors, sizeof(colors)) &&
        !memcmp(previous_pixels, framebuffer, sizeof(previous_pixels))) return;
    memcpy(previous_colors, colors, sizeof(colors));
    memcpy(previous_pixels, framebuffer, sizeof(previous_pixels));
    previous_mode = mode; previous_size = display_size; frame_valid = true;
    dc_convert_frame(pixels, framebuffer, colors, mode);
    pvr_wait_ready();
    pvr_txr_load(pixels, texture, sizeof(pixels));
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&polygon, sizeof(polygon));
    float left = (640-display_size)/2.0f, top = (480-display_size)/2.0f;
    for (int i = 0; i < 4; ++i) {
        pvr_vertex_t v{};
        v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = left + (i & 1) * display_size;
        v.y = top + (i >> 1) * display_size;
        v.z = 1.0f;
        v.u = float(i & 1); v.v = float(i >> 1);
        v.argb = 0xffffffff;
        pvr_prim(&v, sizeof(v));
    }
    pvr_list_finish();
    pvr_scene_finish();
    ++submitted_frames;
}

bool Host::shouldFillAudioBuff() { return false; }
void* Host::getAudioBufferPointer() { return samples; }
size_t Host::getAudioBufferSize() { return 0; }
void Host::playFilledAudioBuffer() { dc_poll_audio(); }
void Host::oneTimeCleanup() {
    if (stream != SND_STREAM_INVALID) { snd_stream_stop(stream); snd_stream_destroy(stream); }
    stream = SND_STREAM_INVALID;
    if (sound_initialized) snd_stream_shutdown();
    if (texture) { pvr_wait_ready(); pvr_mem_free(texture); texture = nullptr; }
    if (initialized) pvr_shutdown();
    initialized = false;
}

std::vector<std::string> Host::listcarts() {
    std::vector<std::string> carts;
    append_carts("/rd/carts", carts);
    append_carts("/cd/carts", carts);
    for (const char* path : {"/ram/download.p8", "/ram/download.p8.png"}) {
        file_t fd = fs_open(path, O_RDONLY);
        if (fd >= 0) { carts.emplace_back(path); fs_close(fd); }
    }
    std::sort(carts.begin(), carts.end());
    return carts;
}
std::vector<std::string> Host::listdirs() { return {}; }
std::string Host::getCartDirectory() { return _cartDirectory; }
void Host::setCartDirectory(std::string directory) { _cartDirectory = directory; }
const char* Host::logFilePrefix() { return _logFilePrefix.c_str(); }
void Host::overrideLogFilePrefix(const char* prefix) { _logFilePrefix = prefix; }
std::string Host::customBiosLua() { return ""; }
void Host::unpackCarts() {}

// Cartridge data survives cart switches, but deliberately makes no VMU writes.
// Bound storage so an arbitrary cart cannot exhaust Dreamcast RAM via dset keys.
std::string Host::getCartDataFileContents(std::string key) {
    auto found = session_saves.find(key);
    return found == session_saves.end() ? "" : found->second;
}
void Host::saveCartData(std::string key, std::string contents) {
    if (key.size() <= 64 && contents.size() <= 4096 &&
        (session_saves.size() < 32 || session_saves.count(key))) session_saves[key] = contents;
}
size_t Host::getFileContents(std::string, char*) { return 0; }
void Host::writeBufferToFile(std::string, char*, size_t) {}
int Host::getSetting(std::string name) {
    if (name == "p8_bgcolor") return 1;
    if (name == "p8_textcolor") return 7;
    if (name == "stretch") return display_size == 480 ? StretchToFit : PixelPerfect;
    return 0;
}
void Host::setSetting(std::string name, int value) {
    if (name == "stretch") forceStretch(static_cast<StretchOption>(value));
}

void Host::setUpPaletteColors() {
    const Color base[] = {COLOR_00,COLOR_01,COLOR_02,COLOR_03,COLOR_04,COLOR_05,COLOR_06,COLOR_07,
        COLOR_08,COLOR_09,COLOR_10,COLOR_11,COLOR_12,COLOR_13,COLOR_14,COLOR_15};
    const Color extra[] = {COLOR_128,COLOR_129,COLOR_130,COLOR_131,COLOR_132,COLOR_133,COLOR_134,COLOR_135,
        COLOR_136,COLOR_137,COLOR_138,COLOR_139,COLOR_140,COLOR_141,COLOR_142,COLOR_143};
    memset(_paletteColors, 0, sizeof(_paletteColors));
    std::copy(base, base+16, _paletteColors);
    _paletteColors[0] = {0,0,0,255};
    std::copy(extra, extra+16, _paletteColors+128);
}
Color* Host::GetPaletteColors() { return _paletteColors; }
