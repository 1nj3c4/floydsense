#pragma once
#include "overlay.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

// Borderless launcher/injector window.  Runs on the main thread.
// onAttach/onDetach are called when the user toggles the connection.
// Returns when the user closes the window.
void runLauncher(
    std::shared_ptr<AppState>    state,
    std::shared_ptr<std::mutex>  mtx,
    std::atomic<bool>&           stop,
    std::function<void()>        onAttach,
    std::function<void()>        onDetach);
