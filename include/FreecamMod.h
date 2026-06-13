#pragma once

#include <unordered_map>
#include <string>
#include <optional>

#include "ll/api/plugin/NativePlugin.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/phys/Vec3.h"
#include "mc/math/Vec2.h"

namespace freecam {

// Snapshot of a player's state before freecam was enabled
struct PlayerSnapshot {
    Vec3        position;
    Vec2        rotation;       // x = pitch, y = yaw
    int         dimensionId;
    GameType    originalGamemode;
    std::string dummyName;      // unique name of the SimulatedPlayer body
};

// Central state registry: XUID -> snapshot
using FreecamRegistry = std::unordered_map<std::string, PlayerSnapshot>;

class FreecamMod {
public:
    static FreecamMod& getInstance();

    // Called by plugin lifecycle hooks
    bool onLoad();
    bool onEnable();
    bool onDisable();

    // Public API used by command & event hooks
    bool toggleFreecam(Player& player);
    bool isInFreecam(const Player& player) const;

    FreecamRegistry& getRegistry() { return mRegistry; }

private:
    FreecamMod() = default;

    void enableFreecam(Player& player);
    void disableFreecam(Player& player);

    FreecamRegistry mRegistry;
};

} // namespace freecam
