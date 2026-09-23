#include "keyboard_transition_queue.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using Keys = std::array<std::uint8_t, 6>;
using Press = std::pair<std::uint8_t, std::uint8_t>;

static void push(KeyboardTransitionBuffer& buffer, Keys keys = {},
                 std::uint8_t modifiers = 0, int port = 0)
{
    buffer.push(port, modifiers, keys.data());
}

// Model the guest's edge detection: only newly held keys produce characters.
struct Guest
{
    Keys previous{};
    std::vector<Press> presses;

    bool poll(KeyboardTransitionBuffer& buffer, int port = 0)
    {
        Keys keys{};
        std::uint8_t modifiers = 0;
        if (!buffer.read(port, modifiers, keys.data()))
            return false;
        for (auto key : keys)
            if (key != 0 && std::find(previous.begin(), previous.end(), key) == previous.end())
                presses.emplace_back(key, modifiers);
        previous = keys;
        return true;
    }
};

static void testBatchedTaps()
{
    KeyboardTransitionBuffer buffer;
    Guest guest;
    std::vector<Press> expected;
    for (int i = 0; i < 100; ++i)
    {
        auto key = static_cast<std::uint8_t>(4 + i % 26);
        push(buffer, {key});
        push(buffer);
        expected.emplace_back(key, 0);
    }
    // All 200 host events have arrived before the guest performs any reads.
    for (int i = 0; i < 220; ++i)
        assert(guest.poll(buffer));
    assert(guest.presses == expected);
    assert(guest.previous == Keys{});
}

static void testRepeatedKeyAndModifiers()
{
    KeyboardTransitionBuffer buffer;
    Guest guest;
    push(buffer, {4});
    for (int i = 0; i < 20; ++i)
        push(buffer, {4}); // Host repeats must not add a backlog.
    push(buffer);
    push(buffer, {}, 2);
    push(buffer, {4}, 2);
    push(buffer, {4}); // Releasing Shift does not press A again.
    push(buffer);
    assert(guest.poll(buffer) && guest.previous == Keys{4});
    assert(guest.poll(buffer) && guest.previous == Keys{});
    for (int i = 0; i < 4; ++i)
        guest.poll(buffer);
    assert((guest.presses == std::vector<Press>{{4, 0}, {4, 2}}));
    assert(guest.previous == Keys{});
}

static void testEmptyAndHeldState()
{
    KeyboardTransitionBuffer buffer;
    Guest guest;
    assert(!guest.poll(buffer));
    push(buffer);
    assert(guest.poll(buffer) && guest.previous == Keys{});
    push(buffer, {4}, 1);
    for (int i = 0; i < 100; ++i)
        assert(guest.poll(buffer) && guest.previous == Keys{4});
    assert((guest.presses == std::vector<Press>{{4, 1}}));
    push(buffer);
    assert(guest.poll(buffer) && guest.previous == Keys{});
}

static void testOverlapAndPorts()
{
    KeyboardTransitionBuffer buffer;
    Guest first, second;
    push(buffer, {4});
    push(buffer, {4, 5});
    push(buffer, {5});
    push(buffer, {5, 6}, 1);
    push(buffer, {6}, 1);
    push(buffer);
    push(buffer, {7}, 2, 1);
    push(buffer, {}, 0, 1);
    for (int i = 0; i < 10; ++i)
    {
        first.poll(buffer);
        second.poll(buffer, 1);
    }
    assert((first.presses == std::vector<Press>{{4, 0}, {5, 0}, {6, 1}}));
    assert((second.presses == std::vector<Press>{{7, 2}}));
}

static void testOverflowAndReset()
{
    KeyboardTransitionBuffer buffer;
    Guest guest;
    push(buffer, {4});
    guest.poll(buffer); // The guest is holding A when the backlog overflows.
    for (std::size_t i = 0; i < KeyboardTransitionBuffer::Capacity / 2; ++i)
    {
        push(buffer);
        push(buffer, {4});
    }
    push(buffer, {5}, 2);
    push(buffer);
    guest.poll(buffer);
    assert(guest.previous == Keys{}); // Explicit release before resynchronizing.
    guest.poll(buffer);
    assert(guest.previous == Keys{5});
    guest.poll(buffer);
    assert(guest.previous == Keys{});
    assert((guest.presses == std::vector<Press>{{4, 0}, {5, 2}}));

    // Overflow while the latest host state is a release must also release.
    buffer.resetAll();
    for (std::size_t i = 0; i < KeyboardTransitionBuffer::Capacity / 2; ++i)
    {
        push(buffer, {4});
        push(buffer, {4}, 2);
    }
    push(buffer);
    guest.poll(buffer);
    assert(guest.previous == Keys{});

    push(buffer, {6});
    buffer.reset(0);
    Keys fallback{9};
    std::uint8_t modifiers = 4;
    assert(!buffer.read(0, modifiers, fallback.data()));
    assert(fallback == Keys{9} && modifiers == 4); // Unbuffered paths are untouched.
    push(buffer, {7}, 0, 1);
    buffer.resetAll();
    assert(!buffer.read(1, modifiers, fallback.data()));
    for (int port : {-1, 4, 100})
    {
        push(buffer, {8}, 0, port);
        buffer.reset(port);
        assert(!buffer.read(port, modifiers, fallback.data()));
    }
}

static void testThreads()
{
    KeyboardTransitionBuffer buffer;
    Guest guest;
    std::atomic<int> consumed{0};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::thread producer([&] {
        for (int batch = 0; batch < 6; ++batch)
        {
            for (int i = 0; i < 50; ++i)
            {
                push(buffer, {4});
                push(buffer);
            }
            while (consumed.load() < (batch + 1) * 50)
            {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
        }
    });
    while (guest.presses.size() < 300)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        guest.poll(buffer);
        consumed.store(static_cast<int>(guest.presses.size()));
    }
    producer.join();
    guest.poll(buffer);
    assert(guest.previous == Keys{});
    assert((guest.presses == std::vector<Press>(300, {4, 0})));
}

int main()
{
    testBatchedTaps();
    testRepeatedKeyAndModifiers();
    testEmptyAndHeldState();
    testOverlapAndPorts();
    testOverflowAndReset();
    testThreads();
    std::cout << "Keyboard input tests passed: batched/repeated taps, modifiers, overlap, "
                 "ports, overflow, reset, concurrent input.\n";
}
