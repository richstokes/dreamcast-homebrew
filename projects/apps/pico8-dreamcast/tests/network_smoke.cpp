#include <kos.h>
#include <fstream>
#include <sstream>
#include "vm.h"
#include "../download.h"
#include "../platform.h"

bool dc_network_smoke(Host& host, Vm& vm) {
    std::ifstream cases("/rd/tests/network-cases.txt");
    std::string line;
    bool passed = cases.good();
    dc_audio_enable(false);
    // Exercise stable idle rendering and the same raw controller mapping used
    // by the normal launcher before opening the native URL screen.
    dc_set_test_pad(0);
    bool launcher = vm.LoadCart("/rd/launcher.p8", false);
    vm.vm_run();
    uint32_t first_hash = 0;
    for (int frame = 0; launcher && frame < 360; ++frame) {
        host.setTargetFps(vm.GetTargetFps());
        host.waitForTargetFps();
        launcher = vm.Step() && vm.GetBiosError().empty();
        uint32_t hash = 2166136261u;
        for (int i = 0; i < 8192; ++i) hash = (hash ^ vm.GetPicoInteralFb()[i]) * 16777619u;
        if (frame == 4) first_hash = hash;
        if (frame > 4) launcher &= hash == first_hash;
        host.drawFrame(vm.GetPicoInteralFb(), vm.GetScreenPaletteMap(), vm.getPicoRam()->drawState.drawMode);
    }
    dc_set_launcher(true);
    dc_set_test_pad(CONT_Y);
    auto input = host.scanInput();
    launcher &= dc_download_requested() && !(input.KHeld & P8_KEY_X);
    host.scanInput();
    launcher &= !dc_download_requested(); // Holding Y must not reopen the screen.
    dc_set_test_pad(0); host.scanInput();
    dc_set_launcher(false);
    dc_set_test_pad(CONT_Y);
    input = host.scanInput();
    launcher &= !dc_download_requested() && (input.KHeld & P8_KEY_X);
    dc_set_test_pad(0); host.scanInput();
    printf("PICO8_NET_TEST: %s LAUNCHER_INPUT_IDLE\n", launcher ? "PASS" : "FAIL");
    passed &= launcher;
    vm.CloseCart();
    while (std::getline(cases, line)) {
        std::istringstream fields(line);
        std::string name, url; int expected;
        fields >> name >> expected >> url;
        if (name.empty()) continue;
        bool ok = dc_download_start(url);
        uint64_t start = timer_ms_gettime64();
        bool cancel_sent = false;
        while (dc_download_status().busy) {
            auto status = dc_download_status();
            if ((expected == 2 && status.received > 0) ||
                timer_ms_gettime64() - start > 120000) {
                dc_download_cancel(); cancel_sent = true;
            }
            thd_sleep(10);
        }
        auto status = dc_download_status();
        dc_download_reap();
        ok &= status.success == (expected == 1);
        if (expected == 2) ok &= cancel_sent && status.message == "Download canceled.";
        if (status.success) {
            dc_reset_input();
            ok &= vm.LoadCart(status.path, false);
            vm.vm_run();
            for (int i = 0; ok && i < 5; ++i) ok = vm.Step() && vm.GetBiosError().empty();
            host.drawFrame(vm.GetPicoInteralFb(), vm.GetScreenPaletteMap(), vm.getPicoRam()->drawState.drawMode);
            vm.CloseCart();
        }
        printf("PICO8_NET_TEST: %s %s %s\n", ok ? "PASS" : "FAIL", name.c_str(), status.message.c_str());
        passed &= ok;
    }
    // The screen build injects characters into the same editor used by physical
    // keyboards, then submits through the ordinary worker and launch path.
    auto path = dc_download_screen();
    bool screen = !path.empty() && vm.LoadCart(path, false);
    if (screen) {
        vm.vm_run();
        for (int i = 0; screen && i < 10; ++i) screen = vm.Step() && vm.GetBiosError().empty();
        host.drawFrame(vm.GetPicoInteralFb(), vm.GetScreenPaletteMap(), vm.getPicoRam()->drawState.drawMode);
    }
    printf("PICO8_NET_TEST: %s URL_SCREEN_LAUNCH\n", screen ? "PASS" : "FAIL");
    passed &= screen;
    printf("PICO8_NET_TEST: %s ALL\n", passed ? "PASS" : "FAIL");
    thd_sleep(1500);
    return passed;
}
