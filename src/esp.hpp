#pragma once
#include "memory.hpp"
#include "offsets.hpp"
#include "client_dll.hpp"
#include <vector>
#include <array>
#include <optional>
#include <cstdint>

namespace off = cs2_dumper::offsets::client_dll;
namespace sch = cs2_dumper::schemas::client_dll;

struct Vec3 { float x, y, z; };

struct EspPlayer {
    size_t    index;
    char      name[128];
    int32_t   health;
    int32_t   armor;
    int32_t   team;
    Vec3      pos;
    Vec3      headPos;
    Vec3      bonePos; // Dynamically targeted bone
    Vec3      bones[16]; // 16 skeleton joints
    bool      hasBones;
    bool      isSpotted;
};

// Source 2 entity system layout (confirmed from Osiris reverse engineering):
//   CGameEntitySystem is obtained from *(clientBase + dwEntityList).
//   CConcreteEntityList is embedded within CGameEntitySystem at byte offset 0x10.
//   CConcreteEntityList::chunks[i] is a pointer to CEntityIdentity[512].
//   sizeof(CEntityIdentity) == 112  (entity* at +0, class* at +8, handle at +16, pad[92] to end).
//
// Full address for entity at flat index i:
//   chunk    = i / 512
//   inChunk  = i % 512
//   chunkPtr = *(entitySystem + 0x10 + 8 * chunk)
//   entity   = *(chunkPtr     + 112  * inChunk)
static constexpr uintptr_t kEntityStride      = 112; // sizeof(CEntityIdentity)
static constexpr uintptr_t kEntityListOffset  = 0x10; // CConcreteEntityList offset in entity system
static constexpr int       kChunkSize         = 512;  // kNumberOfIdentitiesPerChunk

// Resolves a CHandle value to its entity pointer.
inline uintptr_t resolveHandle(const Process& proc, uintptr_t entitySystem, uint32_t handle) {
    if (handle == 0 || handle == 0xFFFFFFFF) return 0;
    uint32_t idx     = handle & 0x7FFF;
    uint32_t chunk   = idx >> 9;
    uint32_t inChunk = idx & 0x1FF;
    auto chunkPtr = proc.read<uintptr_t>(entitySystem + kEntityListOffset + 8 * chunk);
    if (!chunkPtr || !*chunkPtr) return 0;
    return proc.read<uintptr_t>(*chunkPtr + kEntityStride * inChunk).value_or(0);
}

// Raw in-memory layout of CEntityIdentity (112 bytes, confirmed by Osiris).
struct alignas(8) RawIdent {
    uintptr_t entity;       // +0x00
    uintptr_t entityClass;  // +0x08
    uint32_t  handle;       // +0x10
    uint8_t   pad[92];      // +0x14 → total = 112 bytes
};
static_assert(sizeof(RawIdent) == 112, "CEntityIdentity stride mismatch");

// Scans the first chunk's 1..64 slots for player controllers.
// Resolves controllers to player pawns, reading health, team, armor, names and bone coordinates.
inline std::vector<EspPlayer> collectPlayers(
    const Process& proc, uintptr_t clientBase, uintptr_t localPawn, int targetBoneIndex)
{
    std::vector<EspPlayer> players;

    auto esOpt = proc.read<uintptr_t>(clientBase + off::dwEntityList);
    if (!esOpt || !*esOpt) return players;
    uintptr_t es = *esOpt;

    // Player controllers are always in chunk 0 (indices 1 to 64)
    auto cpOpt = proc.read<uintptr_t>(es + kEntityListOffset); // Chunk 0 ptr
    if (!cpOpt || !*cpOpt) return players;

    // We only need the first 65 identities (index 0 is world, 1..64 are controllers)
    // 65 identities * 112 bytes = 7280 bytes
    RawIdent s_buf[65];
    SIZE_T nr = proc.readRaw(*cpOpt, s_buf, sizeof(s_buf));
    int valid = static_cast<int>(nr / sizeof(RawIdent));
    if (valid > 65) valid = 65;

    for (int i = 1; i < valid; ++i) {
        uintptr_t ctrl = s_buf[i].entity;
        if (!ctrl) continue;

        // Read pawn handle
        auto pawnHandle = proc.read<uint32_t>(ctrl + sch::CBasePlayerController::m_hPawn);
        if (!pawnHandle || *pawnHandle == 0xFFFFFFFF) continue;

        // Resolve pawn handle to pawn entity
        uintptr_t pawn = resolveHandle(proc, es, *pawnHandle);
        if (!pawn || pawn == localPawn) continue;

        // Read health
        auto health = proc.read<int32_t>(pawn + sch::C_BaseEntity::m_iHealth);
        if (!health || *health < 1 || *health > 100) continue;

        // Read team
        auto teamByte = proc.read<uint8_t>(pawn + sch::C_BaseEntity::m_iTeamNum);
        if (!teamByte || *teamByte < 2 || *teamByte > 3) continue;

        // Read armor
        auto armor = proc.read<int32_t>(pawn + sch::C_CSPlayerPawn::m_ArmorValue).value_or(0);

        // Read origin
        float x = proc.read<float>(pawn + sch::C_BasePlayerPawn::m_vOldOrigin).value_or(0.f);
        float y = proc.read<float>(pawn + sch::C_BasePlayerPawn::m_vOldOrigin + 4).value_or(0.f);
        float z = proc.read<float>(pawn + sch::C_BasePlayerPawn::m_vOldOrigin + 8).value_or(0.f);

        // Read view offset
        float vx = proc.read<float>(pawn + sch::C_BaseModelEntity::m_vecViewOffset).value_or(0.f);
        float vy = proc.read<float>(pawn + sch::C_BaseModelEntity::m_vecViewOffset + 4).value_or(0.f);
        float vz = proc.read<float>(pawn + sch::C_BaseModelEntity::m_vecViewOffset + 8).value_or(0.f);

        // Read player name from controller
        char nameBuf[128] = {};
        proc.readRaw(ctrl + sch::CBasePlayerController::m_iszPlayerName, nameBuf, 127);

        EspPlayer ep{};
        ep.index  = static_cast<size_t>(i);
        ep.health = *health;
        ep.armor  = armor;
        ep.team   = static_cast<int32_t>(*teamByte);
        ep.pos    = { x, y, z };
        ep.headPos = { x + vx, y + vy, z + vz };
        ep.bonePos = ep.headPos;
        ep.hasBones = false;
        ep.isSpotted = proc.read<bool>(pawn + sch::C_CSPlayerPawn::m_entitySpottedState + 0x8).value_or(false);
        memset(ep.bones, 0, sizeof(ep.bones));
        memcpy(ep.name, nameBuf, sizeof(ep.name));

        // Read scene node for bone array
        auto sceneNode = proc.read<uintptr_t>(pawn + sch::C_BaseEntity::m_pGameSceneNode);
        if (sceneNode && *sceneNode) {
            auto boneArray = proc.read<uintptr_t>(*sceneNode + 0x1D0);
            if (boneArray && *boneArray) {
                auto headPosRead = proc.read<Vec3>(*boneArray + 6 * 32);
                if (headPosRead) ep.headPos = *headPosRead;

                if (targetBoneIndex == 6) {
                    ep.bonePos = ep.headPos;
                } else {
                    auto targetPosRead = proc.read<Vec3>(*boneArray + targetBoneIndex * 32);
                    if (targetPosRead) ep.bonePos = *targetPosRead;
                }

                // Bone indices to load into ep.bones (16 total joints):
                // 0: Head (6)
                // 1: Neck (5)
                // 2: Spine (4)
                // 3: Pelvis (0)
                // 4: L_Shoulder (8), 5: L_Elbow (9), 6: L_Wrist (10)
                // 7: R_Shoulder (13), 8: R_Elbow (14), 9: R_Wrist (15)
                // 10: L_Hip (22), 11: L_Knee (23), 12: L_Ankle (24)
                // 13: R_Hip (25), 14: R_Knee (26), 15: R_Ankle (27)
                static const int kBonesToRead[16] = { 6, 5, 4, 0, 8, 9, 10, 13, 14, 15, 22, 23, 24, 25, 26, 27 };
                for (int b = 0; b < 16; ++b) {
                    auto bonePosRead = proc.read<Vec3>(*boneArray + kBonesToRead[b] * 32);
                    ep.bones[b] = bonePosRead.value_or(Vec3{0,0,0});
                }
                ep.hasBones = true;
            }
        }

        players.push_back(ep);
    }
    return players;
}

inline std::optional<std::array<float, 16>> readViewMatrix(
    const Process& proc, uintptr_t clientBase)
{
    return proc.read<std::array<float, 16>>(clientBase + off::dwViewMatrix);
}

inline int32_t readLocalTeam(const Process& proc, uintptr_t localPawn) {
    if (localPawn == 0) return 0;
    auto b = proc.read<uint8_t>(localPawn + sch::C_BaseEntity::m_iTeamNum);
    return b ? static_cast<int32_t>(*b) : 0;
}
