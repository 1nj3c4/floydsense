#pragma once
#include "esp.hpp"
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <cstdint>

inline std::string VKToString(int vk) {
    switch (vk) {
        case VK_LBUTTON: return "M1";
        case VK_RBUTTON: return "M2";
        case VK_MBUTTON: return "M3";
        case VK_XBUTTON1: return "M4";
        case VK_XBUTTON2: return "M5";
        case VK_SHIFT: return "SHIFT";
        case VK_LSHIFT: return "LSHIFT";
        case VK_CONTROL: return "CTRL";
        case VK_LCONTROL: return "LCTRL";
        case VK_MENU: return "ALT";
        case VK_SPACE: return "SPACE";
        default: {
            char keyName[32];
            if (GetKeyNameTextA(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16, keyName, sizeof(keyName))) {
                return std::string(keyName);
            }
            return "Key [0x" + std::to_string(vk) + "]";
        }
    }
}

struct AppState {
    std::string            status         = "Starting...";
    DWORD                  pid            = 0;
    uintptr_t              clientBase     = 0;
    std::vector<EspPlayer> players;
    std::array<float, 16>  viewMatrix{};
    bool                   showTeammates  = false;
    bool                   showHealthBars = true;
    int32_t                localTeam      = 0;

    // Customization
    bool showNames    = true;
    int  boxThickness = 1;     // 1, 2, or 3 px
    int  enemyPreset  = 0;     // index into kEnemyColors[]
    int  teamPreset   = 0;     // index into kTeamColors[]
    int  healthPreset = 0;     // index into kHealthColors[]
    int  boxStyle     = 1;     // 0=Off, 1=Full 2D, 2=Corner 2D
    bool showArmor    = true;  // Shield bar
    bool showSkeleton = true;  // Skeleton bones

    // Aimbot
    bool  aimbotEnabled    = false;
    int   aimKeybind       = VK_XBUTTON2;
    int   targetBone       = 6;    // 6=Head, 5=Neck, 4=Chest, 2=Stomach
    int   targetSelection  = 0;    // 0=FOV, 1=Distance, 2=Health
    bool  showFOVCircle    = true;
    float aimbotFOV        = 5.0f; // FOV radius
    float aimbotSmoothness = 5.0f; // 1.0f (snappy) to 30.0f (legit/smooth)
    bool  rcsEnabled       = true; // Anti-spread/Recoil control
    float rcsVertical      = 100.0f; // 0% to 100%
    float rcsHorizontal    = 100.0f; // 0% to 100%
    bool  aimVisibleOnly   = false;
    bool  aimIgnoreJumping = false;

    // Misc
    bool bhopEnabled       = true;
    bool bhopAutoStrafe    = true;
    bool exitRequested     = false; // Clean shutdown flag
};

void overlayThreadProc(
    std::shared_ptr<AppState>    state,
    std::shared_ptr<std::mutex>  mtx,
    const std::atomic<bool>&     stop);
