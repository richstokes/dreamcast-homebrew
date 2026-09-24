#include "download.h"
#include "platform.h"
#include <kos.h>
#include <dc/biosfont.h>
#include <dc/maple/keyboard.h>
#include <malloc.h>
#include <algorithm>
#include <cctype>
#ifdef DC_NETWORK_TEST
#include <fstream>
#endif

namespace {
constexpr int width = 512, height = 448, columns = 12;
constexpr uint16_t background = 0x0843, foreground = 0xffff, accent = 0x7e7f;
const std::string keys = "abcdefghijklmnopqrstuvwxyz0123456789./:-_?=&%#~+@,;!$()[]";
struct Screen {
    uint16_t* pixels = nullptr;
    pvr_ptr_t texture = nullptr;
    pvr_poly_hdr_t polygon{};
    Screen() {
        pixels = static_cast<uint16_t*>(memalign(32, width * 512 * 2));
        texture = pvr_mem_malloc(width * 512 * 2);
        if (!pixels || !texture) return;
        pvr_poly_cxt_t context;
        pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                        width, 512, texture, PVR_FILTER_NONE);
        pvr_poly_compile(&polygon, &context);
    }
    ~Screen() {
        pvr_wait_ready();
        if (texture) pvr_mem_free(texture);
        free(pixels);
    }
    void text(int x, int y, std::string s, uint16_t color = foreground) {
        s.resize(std::min(s.size(), size_t((width - x) / BFONT_THIN_WIDTH)));
        bfont_draw_str_ex(pixels + y * width + x, width, color, background, 16, false, s.c_str());
    }
    void draw(const std::string& url, size_t cursor, int selected, bool upper,
              const DownloadStatus& status, bool canceling, const std::string& notice) {
        std::fill(pixels, pixels + width * 512, background);
        text(16, 8, "DOWNLOAD & PLAY", accent);
        text(16, 38, "Direct .p8 or .p8.png HTTPS URL");
        // Three lines follow the caret, including an end-of-line insertion cell.
        size_t first = cursor >= 117 ? ((cursor / 39) - 2) * 39 : 0;
        for (int row = 0; row < 3; ++row) {
            size_t start = first + row * 39;
            if (start <= url.size()) text(16, 72 + row * 24, url.substr(start, 39));
        }
        if (!status.busy && cursor >= first && cursor < first + 117) {
            int x = 16 + ((cursor - first) % 39) * 12, y = 94 + ((cursor - first) / 39) * 24;
            std::fill(pixels + y * width + x, pixels + y * width + x + 12, accent);
        }
        text(16, 154, canceling ? "Canceling; waiting for network call..." :
             !notice.empty() ? notice : status.message.empty() ? "BBA / saved ISP / DreamPi supported" : status.message, accent);
        if (status.busy) {
            char progress[64];
            if (status.total) snprintf(progress, sizeof(progress), "%lu / %lu bytes  -  B cancels",
                (unsigned long)status.received, (unsigned long)status.total);
            else snprintf(progress, sizeof(progress), "%lu bytes received  -  B cancels", (unsigned long)status.received);
            text(16, 184, progress);
        } else text(16, 184, "Start / Enter: download and play");
        for (int i = 0; i < 60; ++i) {
            int x = 25 + (i % columns) * 39, y = 218 + (i / columns) * 28;
            if (i == selected && !status.busy)
                for (int yy = y; yy < y + 24; ++yy)
                    std::fill(pixels + yy * width + x - 6, pixels + yy * width + x + 24, uint16_t(0x326b));
            if (i < int(keys.size())) {
                char c = upper ? std::toupper(keys[i]) : keys[i];
                text(x, y, std::string(1, c), i == selected ? accent : foreground);
            }
        }
        text(16, 368, "A type   X case   Y delete   B back");
        text(16, 398, "L/R move cursor   Keyboard supported");
        pvr_wait_ready();
        pvr_txr_load(pixels, texture, width * 512 * 2);
        pvr_scene_begin(); pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&polygon, sizeof(polygon));
        for (int i = 0; i < 4; ++i) {
            pvr_vertex_t v{};
            v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = 64 + (i & 1) * width; v.y = 16 + (i >> 1) * height; v.z = 1;
            v.u = float(i & 1); v.v = (i >> 1) * height / 512.f; v.argb = 0xffffffff;
            pvr_prim(&v, sizeof(v));
        }
        pvr_list_finish(); pvr_scene_finish();
    }
};
}

std::string dc_download_screen() {
    static std::string url = "https://";
    Screen screen;
    if (!screen.pixels || !screen.texture) {
        printf("PICO8_NET: cannot allocate download screen\n"); return "";
    }
    dc_audio_enable(false);
    size_t cursor = url.size();
    unsigned previous = ~0u;
    uint64_t next_repeat = 0;
    int selected = 0;
    bool upper = false, canceling = false, submitted = false, attempted = false, previous_left = true, previous_right = true;
    std::string notice;
#ifdef DC_NETWORK_TEST
    std::string script;
    std::ifstream script_file("/rd/tests/screen-url.txt");
    std::getline(script_file, script);
    if (script.empty()) return "";
    url.clear(); cursor = 0;
    size_t script_index = 0;
#endif
    printf("PICO8_NET: download screen opened\n");
    auto keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if (keyboard) while (kbd_queue_pop(keyboard, true) != KBD_QUEUE_END) {}
    for (;;) {
        auto status = dc_download_status();
        if (submitted && !status.busy) {
            dc_download_reap();
            if (canceling) break;
            if (status.success) { dc_reset_input(); return status.path; }
            submitted = false;
#ifdef DC_NETWORK_TEST
            break;
#endif
        }
        auto device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        auto pad = device ? static_cast<cont_state_t*>(maple_dev_status(device)) : nullptr;
        unsigned held = pad ? pad->buttons : 0;
        if (pad) {
            if (pad->joyx < -48) held |= CONT_DPAD_LEFT;
            if (pad->joyx > 48) held |= CONT_DPAD_RIGHT;
            if (pad->joyy < -48) held |= CONT_DPAD_UP;
            if (pad->joyy > 48) held |= CONT_DPAD_DOWN;
        }
        unsigned pressed = held & ~previous;
        auto now = timer_ms_gettime64();
        if (held != previous) next_repeat = now + 400;
        else if (now >= next_repeat) {
            pressed |= held & (CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT | CONT_DPAD_RIGHT | CONT_Y);
            next_repeat = now + 100;
        }
        previous = held;
        bool back = pressed & CONT_B, submit = pressed & CONT_START;
        auto erase = [&] { if (cursor) url.erase(--cursor, 1); };
        auto insert = [&](char c) {
            if (url.size() < DC_URL_LIMIT) { url.insert(cursor++, 1, c); notice.clear(); }
            else notice = "URL limit: 1024 characters.";
        };
        if (!status.busy) {
            if (pressed & CONT_DPAD_LEFT) selected = (selected + 59) % 60;
            if (pressed & CONT_DPAD_RIGHT) selected = (selected + 1) % 60;
            if (pressed & CONT_DPAD_UP) selected = (selected + 48) % 60;
            if (pressed & CONT_DPAD_DOWN) selected = (selected + 12) % 60;
            if (pressed & CONT_X) upper = !upper;
            if (pressed & CONT_Y) erase();
            if ((pressed & CONT_A) && selected < int(keys.size()))
                insert(upper ? std::toupper(keys[selected]) : keys[selected]);
            bool left = pad && pad->ltrig > 160, right = pad && pad->rtrig > 160;
            if (left && !previous_left && cursor) --cursor;
            if (right && !previous_right && cursor < url.size()) ++cursor;
            previous_left = left; previous_right = right;
        }
        keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        if (keyboard) {
            int key;
            while ((key = kbd_queue_pop(keyboard, true)) != KBD_QUEUE_END) {
                if (key == 27 || key == (KBD_KEY_ESCAPE << 8)) back = true;
                if (status.busy) continue;
                if (key == '\r' || key == '\n' || key == (KBD_KEY_ENTER << 8)) submit = true;
                else if (key == '\b' || key == (KBD_KEY_BACKSPACE << 8)) erase();
                else if (key == (KBD_KEY_LEFT << 8)) { if (cursor) --cursor; }
                else if (key == (KBD_KEY_RIGHT << 8)) { if (cursor < url.size()) ++cursor; }
                else if (key == (KBD_KEY_DEL << 8)) { if (cursor < url.size()) url.erase(cursor, 1); }
                else if (key >= 33 && key <= 126) insert(char(key));
            }
        }
#ifdef DC_NETWORK_TEST
        if (!status.busy) {
            if (script_index < script.size()) insert(script[script_index++]);
            else submit = true;
        }
#endif
        if (back) {
            if (!status.busy) break;
            dc_download_cancel(); canceling = true;
        }
        if (submit && !status.busy && !canceling) {
            notice.clear(); attempted = true; submitted = dc_download_start(url);
        }
        screen.draw(url, cursor, selected, upper, attempted ? dc_download_status() : DownloadStatus{}, canceling, notice);
        dc_poll_audio();
        thd_sleep(16);
    }
    dc_download_reap();
    dc_reset_input();
    return "";
}
