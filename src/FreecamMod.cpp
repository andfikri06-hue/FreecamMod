/**
 * FreecamMod.cpp
 * Server-Side Freecam for LeviLamina (Bedrock Dedicated Server)
 *
 * Build target : Linux shared library (.so)
 * Framework    : LeviLamina (modern LL API)
 *
 * Behaviour mirrors Java-Edition Freecam mods:
 *   ON  → spawn a SimulatedPlayer "body dummy" at the real player's position,
 *         switch real player to Spectator so their camera can fly freely.
 *   OFF → teleport real player back to dummy's position,
 *         destroy dummy, restore original gamemode.
 *
 * While in freecam all block-break / block-place / item-drop / attack events
 * are cancelled so the spectating camera cannot affect the world.
 */

// ── LeviLamina core ──────────────────────────────────────────────────────────
#include "ll/api/plugin/NativePlugin.h"
#include "ll/api/plugin/RegisterHelper.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/Logger.h"

// ── LeviLamina player events ─────────────────────────────────────────────────
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerLeaveEvent.h"
#include "ll/api/event/player/PlayerAttackEvent.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerPlacingBlockEvent.h"
#include "ll/api/event/player/PlayerDropItemEvent.h"

// ── Bedrock server classes ────────────────────────────────────────────────────
#include "mc/world/actor/player/Player.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/Vec3.h"
#include "mc/math/Vec2.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

// ── Standard library ─────────────────────────────────────────────────────────
#include <unordered_map>
#include <string>
#include <memory>
#include <optional>
#include <format>

#include "FreecamMod.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Module-level logger
// ─────────────────────────────────────────────────────────────────────────────
static ll::Logger gLogger("FreecamMod");

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: derive a stable, unique dummy name for a player
// ─────────────────────────────────────────────────────────────────────────────
static std::string makeDummyName(const Player& player) {
    // Use a prefix + truncated XUID so the name is unique per player but
    // stays well within Bedrock's 16-char name limit for SimulatedPlayers.
    const std::string xuid = player.getXuid();
    const std::string tag  = xuid.size() > 8 ? xuid.substr(xuid.size() - 8) : xuid;
    return "fc_body_" + tag;          // e.g. "fc_body_12345678"
}

// ─────────────────────────────────────────────────────────────────────────────
//  FreecamMod singleton
// ─────────────────────────────────────────────────────────────────────────────
namespace freecam {

FreecamMod& FreecamMod::getInstance() {
    static FreecamMod instance;
    return instance;
}

// ── isInFreecam ───────────────────────────────────────────────────────────────
bool FreecamMod::isInFreecam(const Player& player) const {
    return mRegistry.contains(player.getXuid());
}

// ── toggleFreecam ─────────────────────────────────────────────────────────────
bool FreecamMod::toggleFreecam(Player& player) {
    if (isInFreecam(player)) {
        disableFreecam(player);
        return false;   // freecam is now OFF
    } else {
        enableFreecam(player);
        return true;    // freecam is now ON
    }
}

// ── enableFreecam ─────────────────────────────────────────────────────────────
void FreecamMod::enableFreecam(Player& player) {
    const std::string xuid = player.getXuid();

    // 1. Capture the player's current world state ─────────────────────────────
    PlayerSnapshot snap;
    snap.position        = player.getPosition();
    // Bedrock stores pitch in rotationX(), yaw in rotationY()
    snap.rotation        = Vec2(player.getRotationX(), player.getRotationY());
    snap.dimensionId     = player.getDimensionId().id;
    snap.originalGamemode = player.getGameType();
    snap.dummyName       = makeDummyName(player);

    // 2. Spawn a SimulatedPlayer at that position to act as the body ──────────
    //
    //  SimulatedPlayer::create() is the LeviLamina-exposed wrapper around
    //  the Bedrock internal "fake player" system.  It registers a fully
    //  simulated client whose entity remains in the world until explicitly
    //  destroyed, and is visible to real players.
    //
    //  Signature (LeviLamina ≥ 0.12):
    //    static SimulatedPlayer* create(
    //        std::string const& name,
    //        BlockPos const& spawnPos,
    //        int dimensionId,
    //        Level& level
    //    );

    Level& level = player.getLevel();

    // Snap position to the player's feet block (SimulatedPlayer spawns at feet)
    BlockPos spawnBlock(
        static_cast<int>(std::floor(snap.position.x)),
        static_cast<int>(std::floor(snap.position.y)),
        static_cast<int>(std::floor(snap.position.z))
    );

    SimulatedPlayer* dummy = SimulatedPlayer::create(
        snap.dummyName,
        spawnBlock,
        snap.dimensionId,
        level
    );

    if (!dummy) {
        gLogger.error("Failed to spawn body dummy for player '{}'", player.getRealName());
        return;
    }

    // Fine-tune position to sub-block precision and apply the same rotation
    // as the real player so the body faces the right way.
    dummy->teleport(snap.position, snap.rotation);

    // Give the dummy a fixed Survival gamemode so it can take damage and is
    // treated as a real entity by the server physics.
    dummy->setGameType(GameType::Survival);

    // 3. Store snapshot BEFORE switching the real player's mode ───────────────
    mRegistry[xuid] = snap;

    // 4. Switch the real player to Spectator ──────────────────────────────────
    //
    //  In Spectator mode the Bedrock client automatically enters a free-fly
    //  camera that phases through blocks – exactly what we want.
    player.setGameType(GameType::Spectator);

    // 5. Inform the player ────────────────────────────────────────────────────
    player.sendMessage("§a[Freecam] §fEnabled — your body remains at your position.");
    gLogger.info("Freecam ENABLED for '{}'", player.getRealName());
}

// ── disableFreecam ────────────────────────────────────────────────────────────
void FreecamMod::disableFreecam(Player& player) {
    const std::string xuid = player.getXuid();

    auto it = mRegistry.find(xuid);
    if (it == mRegistry.end()) {
        return; // not in freecam – nothing to do
    }

    PlayerSnapshot& snap = it->second;
    Level&          level = player.getLevel();

    // 1. Find the dummy by its unique name ────────────────────────────────────
    SimulatedPlayer* dummy = nullptr;
    level.forEachPlayer([&](Player& candidate) -> bool {
        if (candidate.getRealName() == snap.dummyName) {
            // SimulatedPlayer IS a Player subclass; safe downcast
            dummy = static_cast<SimulatedPlayer*>(&candidate);
            return false; // stop iteration
        }
        return true; // continue
    });

    // 2. Determine teleport target ─────────────────────────────────────────────
    //    Use the dummy's *current* position in case it was moved (e.g., by
    //    gravity or physics after spawning).  Fall back to the saved snapshot
    //    if the dummy is no longer available.
    Vec3 returnPos  = snap.position;
    Vec2 returnRot  = snap.rotation;

    if (dummy) {
        returnPos = dummy->getPosition();
        returnRot = Vec2(dummy->getRotationX(), dummy->getRotationY());
    } else {
        gLogger.warn("Body dummy '{}' not found – teleporting to saved snapshot position.",
                     snap.dummyName);
    }

    // 3. Restore the real player's gamemode FIRST so the teleport is accepted──
    player.setGameType(snap.originalGamemode);

    // 4. Teleport the real player back to the dummy's feet ────────────────────
    player.teleport(returnPos, returnRot);

    // 5. Destroy the dummy safely ─────────────────────────────────────────────
    if (dummy) {
        // SimulatedPlayer::disconnect() cleanly removes the fake client and
        // despawns the entity without leaving ghost actors in the world.
        dummy->disconnect();
    }

    // 6. Clean up registry ────────────────────────────────────────────────────
    mRegistry.erase(it);

    // 7. Inform the player ────────────────────────────────────────────────────
    player.sendMessage("§c[Freecam] §fDisabled — you have returned to your body.");
    gLogger.info("Freecam DISABLED for '{}'", player.getRealName());
}

// ── onLoad / onEnable / onDisable ─────────────────────────────────────────────
bool FreecamMod::onLoad() {
    gLogger.info("FreecamMod loading…");
    return true;
}

bool FreecamMod::onEnable() {
    gLogger.info("FreecamMod enabled.");
    return true;
}

bool FreecamMod::onDisable() {
    // Force-disable freecam for all remaining players so no dummies are leaked
    // into the world on server shutdown / plugin reload.
    //
    // We collect XUIDs first to avoid mutating the registry while iterating.
    std::vector<std::string> active;
    active.reserve(mRegistry.size());
    for (auto& [xuid, _] : mRegistry) {
        active.push_back(xuid);
    }

    Level* level = ll::service::getLevel();
    if (level) {
        for (const auto& xuid : active) {
            level->forEachPlayer([&](Player& p) -> bool {
                if (p.getXuid() == xuid) {
                    disableFreecam(p);
                    return false;
                }
                return true;
            });
        }
    }

    gLogger.info("FreecamMod disabled – all body dummies cleaned up.");
    return true;
}

} // namespace freecam


// ─────────────────────────────────────────────────────────────────────────────
//  Command: /freecam
//  Permission: any player (GameMasters can expand this to ops-only)
// ─────────────────────────────────────────────────────────────────────────────

struct FreecamCommand {};

static void registerFreecamCommand() {
    auto& reg = ll::command::CommandRegistrar::getInstance();

    auto& cmd = reg.getOrCreateCommand(
        "freecam",
        "Toggle server-side freecam (spectator body-dummy mode)",
        CommandPermissionLevel::Any
    );

    cmd.overload<FreecamCommand>().execute(
        [](CommandOrigin const& origin, CommandOutput& output, FreecamCommand const&) {
            // Only players can use this command
            auto* entity = origin.getEntity();
            if (!entity || !entity->isPlayer()) {
                output.error("This command can only be used by players.");
                return;
            }

            auto& player = *static_cast<Player*>(entity);
            auto& mod    = freecam::FreecamMod::getInstance();

            bool nowOn = mod.toggleFreecam(player);
            output.success(nowOn ? "Freecam enabled." : "Freecam disabled.");
        }
    );
}


// ─────────────────────────────────────────────────────────────────────────────
//  Event listeners
//  All block/attack/drop events are cancelled while the player is in freecam.
// ─────────────────────────────────────────────────────────────────────────────

// We keep listener handles so we can unsubscribe cleanly on plugin disable.
static std::vector<ll::event::ListenerPtr> gListeners;

static void registerEventListeners() {
    auto& bus = ll::event::EventBus::getInstance();
    auto& mod = freecam::FreecamMod::getInstance();

    // ── Block destruction ─────────────────────────────────────────────────────
    gListeners.push_back(
        bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>(
            [&mod](ll::event::PlayerDestroyBlockEvent& ev) {
                if (mod.isInFreecam(ev.self())) {
                    ev.cancel();
                }
            },
            ll::event::EventPriority::High
        )
    );

    // ── Block placement ───────────────────────────────────────────────────────
    gListeners.push_back(
        bus.emplaceListener<ll::event::PlayerPlacingBlockEvent>(
            [&mod](ll::event::PlayerPlacingBlockEvent& ev) {
                if (mod.isInFreecam(ev.self())) {
                    ev.cancel();
                }
            },
            ll::event::EventPriority::High
        )
    );

    // ── Item drop ─────────────────────────────────────────────────────────────
    gListeners.push_back(
        bus.emplaceListener<ll::event::PlayerDropItemEvent>(
            [&mod](ll::event::PlayerDropItemEvent& ev) {
                if (mod.isInFreecam(ev.self())) {
                    ev.cancel();
                }
            },
            ll::event::EventPriority::High
        )
    );

    // ── Entity attack ─────────────────────────────────────────────────────────
    gListeners.push_back(
        bus.emplaceListener<ll::event::PlayerAttackEvent>(
            [&mod](ll::event::PlayerAttackEvent& ev) {
                if (mod.isInFreecam(ev.self())) {
                    ev.cancel();
                }
            },
            ll::event::EventPriority::High
        )
    );

    // ── Player leave: auto-disable freecam so no stray dummies are left ───────
    gListeners.push_back(
        bus.emplaceListener<ll::event::PlayerLeaveEvent>(
            [&mod](ll::event::PlayerLeaveEvent& ev) {
                if (mod.isInFreecam(ev.self())) {
                    mod.toggleFreecam(ev.self()); // toggles OFF
                }
            },
            ll::event::EventPriority::High
        )
    );
}

static void unregisterEventListeners() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : gListeners) {
        bus.removeListener(listener);
    }
    gListeners.clear();
}


// ─────────────────────────────────────────────────────────────────────────────
//  LeviLamina plugin entry-point
// ─────────────────────────────────────────────────────────────────────────────

namespace {

class FreecamPlugin final : public ll::plugin::NativePlugin {
public:
    explicit FreecamPlugin(ll::plugin::NativePlugin::Info info)
        : ll::plugin::NativePlugin(std::move(info)) {}

    bool onLoad() override {
        return freecam::FreecamMod::getInstance().onLoad();
    }

    bool onEnable() override {
        bool ok = freecam::FreecamMod::getInstance().onEnable();
        if (ok) {
            registerFreecamCommand();
            registerEventListeners();
        }
        return ok;
    }

    bool onDisable() override {
        unregisterEventListeners();
        return freecam::FreecamMod::getInstance().onDisable();
    }
};

} // anonymous namespace


// LL_REGISTER_PLUGIN is the modern LeviLamina macro that exports the
// required C symbols and wires up the plugin lifecycle automatically.
LL_REGISTER_PLUGIN(FreecamPlugin, FreecamPlugin::Info{
    .name        = "FreecamMod",
    .version     = ll::data::Version{1, 0, 0},
    .description = "Server-side freecam with SimulatedPlayer body dummy",
    .author      = "YourName",
});
