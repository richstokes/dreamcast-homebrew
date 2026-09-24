// Scheduler PC sampling, following KOS libgprof's histogram callback.
// No -pg traps and no hardware timers taken away from KOS.
#include <kos.h>
#include <vector>
#define DC_PROFILE 1
#include "profile.h"

extern char profile_start asm("__executable_start");
extern char profile_end asm("__etext");
namespace {
std::vector<uint32_t> histogram;
kthread_t* player = nullptr;
kthread_t* sampler = nullptr;
volatile bool active = false;
constexpr unsigned bucket_size = 16;
int sample(void*) {
    if (!active) return 1;
    if (thd_current == player && player->state == STATE_RUNNING) {
        auto pc = uintptr_t(CONTEXT_PC(player->context));
        auto first = uintptr_t(&profile_start), end = uintptr_t(&profile_end);
        if (pc >= first && pc < end) ++histogram[(pc - first) / bucket_size];
    }
    return 0;
}
void* poll(void*) { thd_poll(sample, nullptr, 0); return nullptr; }
}
void dc_profile_begin() {
    histogram.assign((uintptr_t(&profile_end) - uintptr_t(&profile_start) + bucket_size - 1) / bucket_size, 0);
    player = thd_get_current();
    active = true;
    sampler = thd_create(false, poll, nullptr);
    if (!sampler) { active = false; printf("PICO8_PROFILE: allocation failed\n"); }
}
void dc_profile_end(const char* cart) {
    active = false;
    if (sampler) thd_join(sampler, nullptr);
    sampler = nullptr;
    printf("PICO8_PROFILE: BEGIN %s bucket=%u hz=%lu\n", cart, bucket_size, (unsigned long)thd_get_hz());
    for (size_t i = 0; i < histogram.size(); ++i)
        if (histogram[i]) printf("PICO8_PROFILE: %08lx %lu\n",
            (unsigned long)(uintptr_t(&profile_start) + i * bucket_size), (unsigned long)histogram[i]);
    printf("PICO8_PROFILE: END\n");
}
