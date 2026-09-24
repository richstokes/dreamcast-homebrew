#include <kos.h>
#include <memory>
#include <algorithm>
#include <array>
#include "vm.h"
#include "logger.h"
#include "platform.h"
#include "fontdata.h"
#include "download.h"
#include "tests/profile.h"
#include "tests/kernels.h"
#ifdef DC_NETWORK_TEST
bool dc_network_smoke(Host&, Vm&);
#endif

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET | INIT_FS_RND);

[[maybe_unused]] static bool load(Vm& vm, const std::string& path) {
    dc_audio_enable(false);
    dc_reset_input();
    if (!vm.LoadCart(path, false)) {
        printf("PICO8: LOAD FAILED %s: %s\n", path.c_str(), vm.GetBiosError().c_str());
        return false;
    }
    vm.vm_run();
    if (!vm.GetBiosError().empty()) {
        printf("PICO8: RUN FAILED %s: %s\n", path.c_str(), vm.GetBiosError().c_str());
        return false;
    }
    dc_audio_enable(true);
    printf("PICO8: RUN %s\n", path.c_str());
    return true;
}

#ifdef DC_SMOKE_TEST
#include "tests/performance_checks.h"
static uint32_t framebuffer_hash(Vm& vm) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 8192; ++i) hash = (hash ^ vm.GetPicoInteralFb()[i]) * 16777619u;
    return hash;
}

static bool smoke(Host& host, Vm& vm) {
    auto carts = host.listcarts();
    bool passed = carts.size() == 5 && dc_kernel_checks() && dc_frame_cache_checks(host);
    printf("PICO8_TEST: BEGIN carts=%u\n", unsigned(carts.size()));
    for (const auto& cart : carts) {
        dc_set_test_input(0);
        bool ok = load(vm, cart);
        uint32_t first_hash = 0, last_hash = 0;
        uint64_t audio_start = dc_audio_samples(), sound_start = dc_audio_nonzero();
        uint64_t video_start = dc_video_frames();
        std::array<uint32_t, 360> frame_times{};
        uint64_t start = timer_ms_gettime64(), step_us = 0, video_us = 0, audio_us = 0;
        unsigned changes = 0, completed = 0;
        dc_profile_begin();
        for (unsigned frame = 0; ok && frame < 360; ++frame) {
            uint64_t frame_start = timer_us_gettime64();
            // Leave title screens, then exercise both actions and all directions.
            uint8_t buttons = frame == 35 || frame == 70 ? (P8_KEY_O | P8_KEY_X) : 0;
            if (frame > 110) {
                unsigned phase = (frame / 30) % 4;
                buttons = (1u << phase) | (frame % 24 < 4 ? P8_KEY_O : 0);
            }
            dc_set_test_input(buttons);
            host.setTargetFps(vm.GetTargetFps());
            host.waitForTargetFps();
            uint64_t timing = timer_us_gettime64();
            ok = vm.Step() && vm.GetBiosError().empty() && vm.CurrentCartFilename() == cart;
            step_us += timer_us_gettime64() - timing;
            if (!ok) break;
            uint32_t hash = framebuffer_hash(vm);
            if (!frame) first_hash = hash;
            else changes += hash != last_hash;
            last_hash = hash;
            timing = timer_us_gettime64();
            host.drawFrame(vm.GetPicoInteralFb(), vm.GetScreenPaletteMap(), vm.getPicoRam()->drawState.drawMode);
            video_us += timer_us_gettime64() - timing;
            timing = timer_us_gettime64();
            dc_poll_audio();
            audio_us += timer_us_gettime64() - timing;
            frame_times[frame] = timer_us_gettime64() - frame_start;
            ++completed;
        }
        ok = ok && completed == 360 && changes > 0;
        printf("PICO8_TEST: %s %s frames=%u changes=%u first=%08lx last=%08lx audio=%llu nonzero=%llu elapsed_ms=%llu error=%s\n",
            ok ? "PASS" : "FAIL", cart.c_str(), completed, changes,
            (unsigned long)first_hash, (unsigned long)last_hash,
            (unsigned long long)(dc_audio_samples()-audio_start),
            (unsigned long long)(dc_audio_nonzero()-sound_start),
            (unsigned long long)(timer_ms_gettime64()-start), vm.GetBiosError().c_str());
        passed &= ok;
        std::sort(frame_times.begin(), frame_times.end());
        printf("PICO8_TEST: PACING rendered=%llu p50_us=%lu p95_us=%lu max_us=%lu\n",
            (unsigned long long)(dc_video_frames()-video_start), (unsigned long)frame_times[180],
            (unsigned long)frame_times[342], (unsigned long)frame_times[359]);
        dc_profile_end(cart.c_str());
        printf("PICO8_TEST: TIMING vm_us=%llu video_us=%llu audio_us=%llu\n",
            (unsigned long long)step_us, (unsigned long long)video_us, (unsigned long long)audio_us);
    }
    dc_set_test_input(0);
    bool api = load(vm, "/rd/tests/api.p8");
    for (int i = 0; api && i < 5; ++i) api = vm.Step() && vm.GetBiosError().empty();
    printf("PICO8_TEST: %s API %s\n", api ? "PASS" : "FAIL", vm.GetBiosError().c_str());
    passed &= api;
    bool optimized = dc_video_conversion_check() && load(vm, "/rd/tests/performance.p8");
    for (int i = 0; optimized && i < 5; ++i) optimized = vm.Step() && vm.GetBiosError().empty();
    printf("PICO8_TEST: %s PERFORMANCE_CONFORMANCE %s\n", optimized ? "PASS" : "FAIL", vm.GetBiosError().c_str());
    passed &= optimized;
    // Exercise the actual Lua picker and pause/resume input path.
    bool picker = load(vm, "/rd/launcher.p8");
    for (int i = 0; picker && i < 10; ++i) picker = vm.Step();
    dc_set_test_input(P8_KEY_O);
    for (int i = 0; picker && i < 4; ++i) picker = vm.Step();
    dc_set_test_input(0);
    for (int i = 0; picker && i < 10; ++i) picker = vm.Step();
    picker &= !carts.empty() && vm.CurrentCartFilename() == carts[0];
    dc_set_test_input(P8_KEY_PAUSE);
    for (int i = 0; picker && i < 4; ++i) picker = vm.Step();
    picker &= vm.IsPaused();
    dc_set_test_input(0);
    for (int i = 0; picker && i < 4; ++i) picker = vm.Step();
    dc_set_test_input(P8_KEY_PAUSE);
    for (int i = 0; picker && i < 4; ++i) picker = vm.Step();
    picker &= !vm.IsPaused();
    dc_set_test_input(0);
    // Returning to the launcher from a paused game must also reset the pause.
    vm.togglePauseMenu();
    picker &= vm.IsPaused() && load(vm, "/rd/launcher.p8") && !vm.IsPaused();
    printf("PICO8_TEST: %s PICKER_PAUSE\n", picker ? "PASS" : "FAIL");
    passed &= picker;
    // Saving a cartridge and loading it again preserves this session's data.
    bool saves = load(vm, "/rd/tests/api.p8");
    for (int i = 0; saves && i < 5; ++i) saves = vm.Step();
    saves &= vm.ExecuteLua("dset(1,123.5)", "");
    saves &= load(vm, "/rd/launcher.p8");
    saves &= load(vm, "/rd/tests/api.p8");
    for (int i = 0; saves && i < 5; ++i) saves = vm.Step();
    saves &= vm.ExecuteLua("assert(dget(1)==123.5)", "");
    printf("PICO8_TEST: %s SESSION_SAVE\n", saves ? "PASS" : "FAIL");
    passed &= saves;
    printf("PICO8_TEST: %s ALL\n", passed ? "PASS" : "FAIL");
    return passed;
}
#endif

static int run_player() {
    printf("PICO8: Dreamcast / FAKE-08 starting\n");
    Host host;
    auto memory = std::make_unique<PicoRam>();
    memory->Reset();
    Audio audio(memory.get());
    Graphics graphics(get_font_data(), memory.get());
    Input input(memory.get());
    host.setUpPaletteColors();
    host.oneTimeSetup(&audio);
    if (!dc_ready()) return 1;
    {
        Vm vm(&host, memory.get(), &graphics, &input, &audio);
        vm.SetCartList(host.listcarts());
#if defined(DC_SMOKE_TEST) || defined(DC_NETWORK_TEST) || defined(DC_KERNEL_TEST)
#ifdef DC_KERNEL_TEST
        bool passed = dc_kernel_checks() && dc_frame_cache_checks(host);
#elif defined(DC_NETWORK_TEST)
        bool passed = dc_network_smoke(host, vm);
#else
        bool passed = smoke(host, vm);
#endif
        vm.CloseCart();
        host.oneTimeCleanup();
        return passed ? 0 : 1;
#else
        if (!load(vm, "/rd/launcher.p8")) { host.oneTimeCleanup(); return 1; }
        std::string last_cart = vm.CurrentCartFilename();
        while (host.shouldRunMainLoop()) {
            dc_set_launcher(vm.CurrentCartFilename() == "/rd/launcher.p8");
            host.setTargetFps(vm.GetTargetFps());
            host.waitForTargetFps();
            bool ok = vm.Step();
            if (dc_download_requested()) {
                auto path = dc_download_screen();
                vm.SetCartList(host.listcarts());
                if (!path.empty()) {
                    ok = load(vm, path);
                    if (!ok) printf("PICO8_NET: downloaded cart failed to start\n");
                } else dc_audio_enable(true);
            }
            bool go_menu = dc_menu_requested();
            std::string error = vm.GetBiosError();
            std::string current = vm.CurrentCartFilename();
            if (go_menu || !ok || !error.empty() || current == "__FAKE08-DEFAULT.p8") {
                if (!error.empty()) printf("PICO8: runtime error: %s\n", error.c_str());
                if (!load(vm, "/rd/launcher.p8")) break;
                if (!error.empty()) vm.ExecuteLua("status='cart error - see serial log'", "");
                current = vm.CurrentCartFilename();
            }
            if (current != last_cart) {
                printf("PICO8: ACTIVE %s\n", current.c_str());
                last_cart = current;
            }
            host.drawFrame(vm.GetPicoInteralFb(), vm.GetScreenPaletteMap(), memory->drawState.drawMode);
            dc_poll_audio();
        }
        vm.CloseCart();
        host.oneTimeCleanup();
#endif
    }
    printf("PICO8: clean shutdown\n");
    return 0;
}

static void* player_thread(void*) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(run_player()));
}

int main(int, char**) {
    // The PNG decoder / C++ parser need more than KOS's default worker stack.
    kthread_attr_t attributes{};
    attributes.stack_size = 256 * 1024;
    attributes.prio = PRIO_DEFAULT;
    attributes.label = "PICO-8 player";
    kthread_t* thread = thd_create_ex(&attributes, player_thread, nullptr);
    if (!thread) { printf("PICO8: cannot allocate player thread\n"); return 1; }
    void* result = nullptr;
    thd_join(thread, &result);
    printf("PICO8: player exited cleanly status=%d\n", static_cast<int>(reinterpret_cast<intptr_t>(result)));
    return static_cast<int>(reinterpret_cast<intptr_t>(result));
}
