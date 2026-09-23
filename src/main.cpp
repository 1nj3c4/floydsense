#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>       // timeBeginPeriod / timeEndPeriod
#include "overlay.hpp"
#include "launcher.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

static std::atomic<bool> g_stop{false};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Set Windows timer resolution to 1 ms so sleep_for(16ms) is precise.
    // Default resolution is 15.6 ms, which turns a 16 ms sleep into 31 ms.
    timeBeginPeriod(1);

    auto state = std::make_shared<AppState>();
    auto mtx   = std::make_shared<std::mutex>();

    std::thread overlayThread;

    auto onAttach = [&]() {
        // The overlay thread owns its own Process handle and reads game memory
        // directly every render frame — no separate reader thread needed.
        overlayThread = std::thread(overlayThreadProc, state, mtx, std::ref(g_stop));
        std::lock_guard lk(*mtx);
        state->status = "Connecting...";
    };

    auto onDetach = [&]() {
        g_stop.store(true, std::memory_order_relaxed);
        if (overlayThread.joinable()) overlayThread.join();
        g_stop.store(false, std::memory_order_relaxed);
        std::lock_guard lk(*mtx);
        state->status  = "Detached.";
        state->players = {};
        state->viewMatrix = {};
    };

    runLauncher(state, mtx, g_stop, onAttach, onDetach);

    // If the overlay thread is active (due to successful inject), wait for it to exit naturally
    if (overlayThread.joinable() && !g_stop.load(std::memory_order_relaxed)) {
        overlayThread.join();
    }

    g_stop.store(true, std::memory_order_relaxed);
    if (overlayThread.joinable()) overlayThread.join();

    timeEndPeriod(1);
    return 0;
}
