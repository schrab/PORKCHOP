// Porkchop core state machine implementation

#include "porkchop.h"
#include "../hal/hal_input.h"
#include "../ui/display.h"
#include "../ui/menu.h"
#include "../ui/settings_menu.h"
#include "../ui/captures_menu.h"
#include "../ui/achievements_menu.h"
#include "../ui/bounty_status_menu.h"
#include "../ui/crash_viewer.h"
#include "../ui/diagnostics_menu.h"
#include "../ui/swine_stats.h"
#include "../ui/boar_bros_menu.h"
#include "../ui/wigle_menu.h"
#include "../ui/unlockables_menu.h"
#include "../ui/sd_format_menu.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "heap_policy.h"
#include "../modes/donoham.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../modes/spectrum.h"
#include "../modes/pigsync_client.h"
#include "../modes/bacon.h"
#include "../modes/pork_patrol.h"
#include "../modes/swine_radar.h"
#include "../modes/snout.h"
#include "../modes/charging.h"
#include "../core/ghost.h"
#include "../core/wartales.h"
#include "../web/fileserver.h"
#include "../web/webui.h"
#include "../web/serial_api.h"
#include "../audio/sfx.h"
#include "config.h"
#include "heap_health.h"
#include "xp.h"
#include "sdlog.h"
#include "sd_format.h"
#include "challenges.h"
#include "stress_test.h"
#include "network_recon.h"
#include "wifi_utils.h"
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <WiFi.h>

static const char* modeToString(PorkchopMode mode) {
    switch (mode) {
        case PorkchopMode::IDLE: return "IDLE";
        case PorkchopMode::OINK_MODE: return "OINK";
        case PorkchopMode::DNH_MODE: return "DNH";
        case PorkchopMode::WARHOG_MODE: return "WARHOG";
        case PorkchopMode::PIGGYBLUES_MODE: return "PIGGYBLUES";
        case PorkchopMode::SPECTRUM_MODE: return "SPECTRUM";
        case PorkchopMode::MENU: return "MENU";
        case PorkchopMode::SETTINGS: return "SETTINGS";
        case PorkchopMode::CAPTURES: return "CAPTURES";
        case PorkchopMode::ACHIEVEMENTS: return "ACHIEVEMENTS";
        case PorkchopMode::FILE_TRANSFER: return "FILE_TRANSFER";
        case PorkchopMode::CRASH_VIEWER: return "CRASH_VIEWER";
        case PorkchopMode::DIAGNOSTICS: return "DIAGNOSTICS";
        case PorkchopMode::SWINE_STATS: return "SWINE_STATS";
        case PorkchopMode::BOAR_BROS: return "BOAR_BROS";
        case PorkchopMode::WIGLE_MENU: return "WIGLE_MENU";
        case PorkchopMode::UNLOCKABLES: return "UNLOCKABLES";
        case PorkchopMode::BOUNTY_STATUS: return "BOUNTY_STATUS";
        case PorkchopMode::PIGSYNC_DEVICE_SELECT: return "PIGSYNC_DEVICE_SELECT";
        case PorkchopMode::BACON_MODE: return "BACON";
        case PorkchopMode::SD_FORMAT: return "SD_FORMAT";
        case PorkchopMode::CHARGING: return "CHARGING";
        case PorkchopMode::PORK_PATROL: return "PORKPATROL";
        case PorkchopMode::SWINE_RADAR: return "SWINERADAR";
        case PorkchopMode::SNOUT_MODE: return "SNOUT";
        case PorkchopMode::WEBUI_MODE: return "WEBUI";
        case PorkchopMode::ABOUT: return "ABOUT";
        default: return "UNKNOWN";
    }
}

// Crash-loop guard: count early reboots using RTC memory (survives soft resets).
RTC_DATA_ATTR static uint8_t bootGuardStreak = 0;
static uint32_t bootGuardStartMs = 0;
static const uint8_t BOOT_GUARD_THRESHOLD = 3;
static const uint32_t BOOT_GUARD_WINDOW_MS = 60000;

static PorkchopMode bootModeToPorkchop(BootMode mode) {
    switch (mode) {
        case BootMode::OINK: return PorkchopMode::OINK_MODE;
        case BootMode::DNOHAM: return PorkchopMode::DNH_MODE;
        case BootMode::WARHOG: return PorkchopMode::WARHOG_MODE;
        case BootMode::IDLE:
        default:
            return PorkchopMode::IDLE;
    }
}

static const char* bootModeLabel(BootMode mode) {
    switch (mode) {
        case BootMode::OINK: return "OINK";
        case BootMode::DNOHAM: return "DN0HAM";
        case BootMode::WARHOG: return "WARHOG";
        case BootMode::IDLE:
        default:
            return "IDLE";
    }
}

static bool healthBootToastShown = false;

Porkchop::Porkchop() 
    : currentMode(PorkchopMode::IDLE)
    , previousMode(PorkchopMode::IDLE)
    , startTime(0)
    , handshakeCount(0)
    , networkCount(0)
    , deauthCount(0) {
}

static bool isAutoConditionSafe(PorkchopMode mode) {
    switch (mode) {
        case PorkchopMode::IDLE:
        case PorkchopMode::MENU:
        case PorkchopMode::SETTINGS:
        case PorkchopMode::ABOUT:
        case PorkchopMode::ACHIEVEMENTS:
        case PorkchopMode::CRASH_VIEWER:
        case PorkchopMode::DIAGNOSTICS:
        case PorkchopMode::SWINE_STATS:
        case PorkchopMode::BOAR_BROS:
        case PorkchopMode::UNLOCKABLES:
        case PorkchopMode::BOUNTY_STATUS:
        case PorkchopMode::SD_FORMAT:
            return true;
        default:
            return false;
    }
}

static void maybeAutoConditionHeap(PorkchopMode mode) {
    if (!isAutoConditionSafe(mode)) {
        return;
    }
    if (FileServer::isRunning() || FileServer::isConnecting()) {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }
    // At Critical pressure (<30KB free), brew needs 35KB transient — would fail anyway
    if (static_cast<uint8_t>(HeapHealth::getPressureLevel()) > HeapPolicy::kMaxPressureLevelForAutoBrew) {
        return;
    }
    if (!HeapHealth::consumeConditionRequest()) {
        return;
    }

    bool wasReconRunning = NetworkRecon::isRunning();
    if (wasReconRunning) {
        NetworkRecon::pause();
    }
    // Small, low-disruption brew to coalesce heap when health drops.
    WiFiUtils::brewHeap(HeapPolicy::kBrewAutoDwellMs, false);
    if (wasReconRunning) {
        NetworkRecon::resume();
    }
}

void Porkchop::init() {
    startTime = millis();
    
    // Initialize background network reconnaissance service
    NetworkRecon::init();
    
    // Initialize Ghost Mode (periodic MAC rotation)
    GhostMode::init();
    
    // Initialize WarTales session diary
    Wartales::init();
    
    // Initialize XP system
    XP::init();
    
    // Initialize SwineStats (buff/debuff system)
    SwineStats::init();
    
    // Register level up callback to show popup
    XP::setLevelUpCallback([](uint8_t oldLevel, uint8_t newLevel) {
        Display::showLevelUp(oldLevel, newLevel);
        Avatar::cuteJump();  // Celebratory jump on level up!
        
        // Check if class tier changed (every 5 levels: 6, 11, 16, 21, 26, 31, 36)
        PorkClass oldClass = XP::getClassForLevel(oldLevel);
        PorkClass newClass = XP::getClassForLevel(newLevel);
        if (newClass != oldClass) {
            // Small delay between popups
            delay(500);
            Display::showClassPromotion(
                XP::getClassNameFor(oldClass),
                XP::getClassNameFor(newClass)
            );
        }
    });
    
    // Register default event handlers
    registerCallback(PorkchopEvent::HANDSHAKE_CAPTURED, [this](PorkchopEvent, void*) {
        handshakeCount++;
    });
    
    registerCallback(PorkchopEvent::NETWORK_FOUND, [this](PorkchopEvent, void*) {
        networkCount++;
    });
    
    registerCallback(PorkchopEvent::DEAUTH_SENT, [this](PorkchopEvent, void*) {
        deauthCount++;
    });
    
    // Menu selection handler - items now defined in menu.cpp as static arrays
    Menu::setCallback([this](uint8_t actionId) {
        switch (actionId) {
            case 1: setMode(PorkchopMode::OINK_MODE); break;
            case 2: setMode(PorkchopMode::WARHOG_MODE); break;
            case 3: setMode(PorkchopMode::FILE_TRANSFER); break;
            case 4: setMode(PorkchopMode::CAPTURES); break;
            case 5: setMode(PorkchopMode::SETTINGS); break;
            case 6: setMode(PorkchopMode::ABOUT); break;
            case 7: setMode(PorkchopMode::CRASH_VIEWER); break;
            case 8: setMode(PorkchopMode::PIGGYBLUES_MODE); break;
            case 9: setMode(PorkchopMode::ACHIEVEMENTS); break;
            case 10: setMode(PorkchopMode::SPECTRUM_MODE); break;
            case 11: setMode(PorkchopMode::SWINE_STATS); break;
            case 12: setMode(PorkchopMode::BOAR_BROS); break;
            case 13: setMode(PorkchopMode::WIGLE_MENU); break;
            case 14: setMode(PorkchopMode::DNH_MODE); break;
            case 15: setMode(PorkchopMode::UNLOCKABLES); break;
            case 16: setMode(PorkchopMode::PIGSYNC_DEVICE_SELECT); break;
            case 17: setMode(PorkchopMode::BOUNTY_STATUS); break;
            case 18: setMode(PorkchopMode::BACON_MODE); break;
            case 19: setMode(PorkchopMode::DIAGNOSTICS); break;
            case 20: setMode(PorkchopMode::SD_FORMAT); break;
            case 21: setMode(PorkchopMode::CHARGING); break;
            case 22: setMode(PorkchopMode::PORK_PATROL); break;
            case 23: setMode(PorkchopMode::SWINE_RADAR); break;
            case 24: setMode(PorkchopMode::WEBUI_MODE); break;
            case 25: setMode(PorkchopMode::SNOUT_MODE); break;
        }
    });

    bootGuardStartMs = millis();
    if (bootGuardStreak < 255) {
        bootGuardStreak++;
    }
    bool bootGuardActive = bootGuardStreak >= BOOT_GUARD_THRESHOLD;

    BootMode bootMode = Config::personality().bootMode;
    bootModeTarget = bootModeToPorkchop(bootMode);
    if (bootModeTarget != PorkchopMode::IDLE && !bootGuardActive) {
        bootModePending = true;
        bootModeStartMs = millis();
        char buf[32];
        snprintf(buf, sizeof(buf), "BOOT -> %s IN 5S", bootModeLabel(bootMode));
        Display::showToast(buf, 5000);
    } else if (bootModeTarget != PorkchopMode::IDLE && bootGuardActive) {
        Display::showToast("BOOT GUARD - IDLE", 4000);
    }
    
    // Boot-time actions
    Wartales::sessionStart();
    if (Config::wifi().ghostEnabled) {
        GhostMode::apply();
        Wartales::logEvent("GHOST MODE active from saved config");
    }
    
    Avatar::setState(AvatarState::HAPPY);
    
    // Initialize non-blocking audio system
    SFX::init();

    if (!healthBootToastShown) {
        healthBootToastShown = true;
        Display::showToast(
            "HEALTH BAR IS HEAP HEALTH.\n"
            "LARGEST CONTIG DRIVES TLS.\n"
            "FRAGMENTATION YOINKS IT.\n"
            "BREW FIXES. JAH BLESS DI RF.",
            5000
        );
    }
    
    Serial.println("[PORKCHOP] Initialized");
    SDLog::log("PORK", "Initialized - LV%d %s", XP::getLevel(), XP::getTitle());
}

void Porkchop::update() {
    // Update background network reconnaissance (channel hopping, cleanup)
    NetworkRecon::update();
    
    // Update Ghost Mode (periodic MAC rotation)
    GhostMode::update();
    
    // Update WarTales session diary (flush buffer)
    Wartales::update();
    
    processEvents();
    yield(); // Allow other tasks to run between operations
    handleInput();
    yield(); // Allow other tasks to run between operations
    
    if (bootGuardStreak > 0 && (millis() - bootGuardStartMs >= BOOT_GUARD_WINDOW_MS)) {
        bootGuardStreak = 0;
    }
    if (bootModePending) {
        if (currentMode != PorkchopMode::IDLE) {
            bootModePending = false;
        } else if (millis() - bootModeStartMs >= 5000) {
            bootModePending = false;
            setMode(bootModeTarget);
        }
    }
    updateMode();

    maybeAutoConditionHeap(currentMode);
    
    // Tick non-blocking audio engine
    SFX::update();
    yield(); // Allow other tasks to run between operations
    
    // Poll serial for API key entry (SETTINGS mode only)
    SerialAPI::poll();
    yield(); // Allow other tasks to run between operations
    
    // Process one queued achievement celebration (debounced)
    XP::processAchievementQueue();
    yield(); // Allow other tasks to run between operations
    
    // Stress test injection (if active)
    StressTest::update();
    yield(); // Allow other tasks to run between operations
    
    // Check for session time XP bonuses
    XP::updateSessionTime();
    yield(); // Allow other tasks to run between operations
    
    // Periodic SD writes — ring buffer flush for SDLog, throttled XP backup
    SDLog::periodicFlush();
    XP::periodicBackup();
    yield(); // Allow other tasks to run between operations
}

void Porkchop::setMode(PorkchopMode mode) {
    if (mode == currentMode) return;
    
    // Store the mode we're leaving for cleanup
    PorkchopMode oldMode = currentMode;

    Serial.printf("[MODE] EXIT %s free=%u\r\n",
        modeToString(oldMode),
        (unsigned)esp_get_free_heap_size());
    
    // Save "real" modes as previous (not modal menus)
    // Exception: CAPTURES and WIGLE_MENU ARE saved as previousMode so OINK recovery returns to them
    if (currentMode != PorkchopMode::SETTINGS &&
        currentMode != PorkchopMode::ABOUT &&
        currentMode != PorkchopMode::ACHIEVEMENTS &&
        currentMode != PorkchopMode::MENU &&
        currentMode != PorkchopMode::FILE_TRANSFER &&
        currentMode != PorkchopMode::CRASH_VIEWER &&
        currentMode != PorkchopMode::DIAGNOSTICS &&
        currentMode != PorkchopMode::SWINE_STATS &&
        currentMode != PorkchopMode::BOAR_BROS &&
        currentMode != PorkchopMode::BOUNTY_STATUS &&
        currentMode != PorkchopMode::PIGSYNC_DEVICE_SELECT &&
        currentMode != PorkchopMode::UNLOCKABLES &&
        currentMode != PorkchopMode::SD_FORMAT) {
        previousMode = currentMode;
    }
    // ALSO save CAPTURES and WIGLE_MENU as return points from OINK recovery
    if (currentMode == PorkchopMode::CAPTURES ||
        currentMode == PorkchopMode::WIGLE_MENU) {
        previousMode = currentMode;
    }
    currentMode = mode;

    Serial.printf("[MODE] ENTER %s free=%u\r\n",
        modeToString(currentMode),
        (unsigned)esp_get_free_heap_size());
    
    // Cleanup the mode we're actually leaving (oldMode), not previousMode
    switch (oldMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::stop();
            break;
        case PorkchopMode::DNH_MODE:
            DoNoHamMode::stop();
            break;
        case PorkchopMode::WARHOG_MODE:
            WarhogMode::stop();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            PiggyBluesMode::stop();
            break;
        case PorkchopMode::SPECTRUM_MODE:
            SpectrumMode::stop();
            break;
        case PorkchopMode::MENU:
            Menu::hide();
            break;
        case PorkchopMode::SETTINGS:
            SettingsMenu::hide();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::hide();
            break;
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::hide();
            break;
        case PorkchopMode::FILE_TRANSFER:
            FileServer::stop();
            // Restart NetworkRecon after FILE_TRANSFER to resume background scanning
            NetworkRecon::start();
            break;
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::hide();
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::hide();
            break;
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::hide();
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::hide();
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::hide();
            break;
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::hide();
            break;
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::hide();
            break;
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::hide();
            break;
        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            PigSyncMode::stopDiscovery();
            break;
        case PorkchopMode::BACON_MODE:
            BaconMode::stop();
            break;
        case PorkchopMode::PORK_PATROL:
            PorkPatrolMode::stop();
            break;
        case PorkchopMode::SWINE_RADAR:
            SwineRadarMode::stop();
            break;
        case PorkchopMode::SNOUT_MODE:
            SnoutMode::stop();
            break;
        case PorkchopMode::WEBUI_MODE:
            WebUI::stop();
            break;
        case PorkchopMode::CHARGING:
            ChargingMode::stop();
            break;
        default:
            break;
    }
    
    // Init new mode
    switch (currentMode) {
        case PorkchopMode::IDLE:
            Avatar::setState(AvatarState::NEUTRAL);
            Mood::onIdle();
            XP::save();  // Save XP when returning to idle
            XP::backupToSD();  // Ensure backup on IDLE transition
            SDLog::log("PORK", "Mode: IDLE");
            break;
        case PorkchopMode::OINK_MODE:
            Avatar::setState(AvatarState::HUNTING);
            Display::notify(NoticeKind::STATUS, "PROPER MAD ONE INNIT", 5000, NoticeChannel::TOP_BAR);
            SDLog::log("PORK", "Mode: OINK");
            Wartales::logEvent("OINK MODE started");
            OinkMode::start();
            break;
        case PorkchopMode::DNH_MODE:
            Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
            SDLog::log("PORK", "Mode: DO NO HAM");
            Wartales::logEvent("DNH MODE started");
            DoNoHamMode::start();
            break;
        case PorkchopMode::WARHOG_MODE:
            Avatar::setState(AvatarState::EXCITED);
            Display::notify(NoticeKind::STATUS, "SNIFFING THE AIR", 5000, NoticeChannel::TOP_BAR);
            SDLog::log("PORK", "Mode: WARHOG");
            Wartales::logEvent("WARHOG started");
            // Disable ML/Enhanced features for heap savings
            {
                auto mlCfg = Config::ml();
                mlCfg.enabled = false;
                mlCfg.collectionMode = MLCollectionMode::BASIC;
                Config::setML(mlCfg);
            }
            WarhogMode::start();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            Avatar::setState(AvatarState::ANGRY);
            SDLog::log("PORK", "Mode: PIGGYBLUES");
            Wartales::logEvent("PIGGYBLUES started");
            PiggyBluesMode::start();
            // If user aborted warning dialog, return to menu
            if (!PiggyBluesMode::isRunning()) {
                currentMode = PorkchopMode::MENU;
                Menu::show();
            }
            break;
        case PorkchopMode::SPECTRUM_MODE:
            Avatar::setState(AvatarState::HUNTING);
            SDLog::log("PORK", "Mode: SPECTRUM");
            Wartales::logEvent("SPECTRUM started");
            SpectrumMode::start();
            break;
        case PorkchopMode::MENU:
            Menu::show();
            break;
        case PorkchopMode::SETTINGS:
            SettingsMenu::show();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::show();
            break;
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::show();
            break;
        case PorkchopMode::FILE_TRANSFER:
            // Stop NetworkRecon and free its ~19KB network vector — FILE_TRANSFER doesn't use it
            NetworkRecon::stop();
            NetworkRecon::freeNetworks();
            Avatar::setState(AvatarState::HAPPY);
            FileServer::start(Config::wifi().otaSSID, Config::wifi().otaPassword);
            break;
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::show();
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::show();
            break;
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::show();
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::show();
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::show();
            break;
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::show();
            break;
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::show();
            break;
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::show();
            break;
        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            Avatar::setState(AvatarState::EXCITED);
            SDLog::log("PORK", "Mode: PIGSYNC Device Select");
            PigSyncMode::start();
            PigSyncMode::startDiscovery();
            break;
        case PorkchopMode::BACON_MODE:
            Avatar::setState(AvatarState::HAPPY);
            SDLog::log("PORK", "Mode: BACON");
            Wartales::logEvent("BACON started");
            BaconMode::init();
            BaconMode::start();
            break;
        case PorkchopMode::PORK_PATROL:
            Avatar::setState(AvatarState::HUNTING);
            SDLog::log("PORK", "Mode: PORKPATROL");
            Wartales::logEvent("PORKPATROL started");
            PorkPatrolMode::start();
            break;
        case PorkchopMode::SWINE_RADAR:
            Avatar::setState(AvatarState::HUNTING);
            SDLog::log("PORK", "Mode: SWINERADAR");
            Wartales::logEvent("SWINERADAR started");
            SwineRadarMode::start();
            break;
        case PorkchopMode::SNOUT_MODE:
            Avatar::setState(AvatarState::HAPPY);
            SDLog::log("PORK", "Mode: SNOUT");
            Wartales::logEvent("SNOUT started");
            SnoutMode::start();
            break;
        case PorkchopMode::WEBUI_MODE:
            SDLog::log("PORK", "Mode: WEBUI");
            Wartales::logEvent("WEBUI started");
            WebUI::start();
            break;
        case PorkchopMode::ABOUT:
            Display::resetAboutState();
            break;
        case PorkchopMode::CHARGING:
            ChargingMode::start();
            break;
        
            
        default:
            break;
    }
    
    postEvent(PorkchopEvent::MODE_CHANGE, nullptr);
}

void Porkchop::postEvent(PorkchopEvent event, void* data) {
    // Prevent event queue overflow that could cause heap fragmentation
    if (eventQueue.size() >= MAX_EVENT_QUEUE_SIZE) {
        // Drop oldest event to maintain queue size
        eventQueue.clear();
    }
    eventQueue.push_back({event, data});
}

void Porkchop::registerCallback(PorkchopEvent event, EventCallback callback) {
    // Prevent duplicate callbacks for the same event to avoid multiple executions
    // Note: We can't reliably compare std::function objects, so we just ensure each event
    // type has only one callback by replacing any existing one
    for (auto& pair : callbacks) {
        if (pair.first == event) {
            pair.second = callback; // Replace existing callback
            return;
        }
    }
    // Add bounds checking to prevent unlimited growth
    if (callbacks.size() >= MAX_EVENT_QUEUE_SIZE) {
        // Remove the oldest callback if we're at capacity
        callbacks.erase(callbacks.begin(), callbacks.end());
    }
    callbacks.push_back({event, callback});
}

void Porkchop::processEvents() {
    // Process events with bounds checking and yield for WDT safety
    // NOTE: All postEvent() callers pass nullptr for data — no ownership to track.
    size_t processed = 0;
    const size_t MAX_EVENTS_PER_UPDATE = 16; // Limit events processed per update to prevent WDT

    // Index-based loop to avoid iterator invalidation from erase()
    size_t i = 0;
    while (i < eventQueue.size() && processed < MAX_EVENTS_PER_UPDATE) {
        const auto& item = eventQueue[i];

        for (const auto& cb : callbacks) {
            if (cb.first == item.event) {
                cb.second(item.event, item.data);

                if (++processed % 4 == 0) {
                    yield();
                }
            }
        }
        i++;
    }

    // Erase all processed events in one operation after the loop
    if (i >= eventQueue.size()) {
        eventQueue.clear();
    } else {
        eventQueue.erase(eventQueue.begin(), eventQueue.begin() + i);
    }
}

void Porkchop::handleInput() {
    // G0 button not present on ESP32-S3 Mini - skip check to avoid floating GPIO triggering

    if (!hal_input_isChange()) return;

    // Reset dim timer on any key state change
    Display::resetDimTimer();
    
    InputEvent keys = hal_input_keysState();
    (void)keys;  // joystick build: all navigation via wasPressed(), not raw keysState
    // ESC = long-press LEFT (800ms) → KEY_ESC
    bool escPressed = hal_input_wasPressed(KEY_ESC) || hal_input_shouldExit();

    // ESC to return to IDLE from any active mode
    if (escPressed && currentMode != PorkchopMode::IDLE) {
        setMode(PorkchopMode::IDLE);
        return;
    }
    
    // In MENU mode, let Menu::handleInput() process navigation keys
    if (currentMode == PorkchopMode::MENU) {
        Menu::update();
        // Only go to IDLE if user closed menu (LEFT) without selecting an item.
        // If a callback already changed mode, currentMode != MENU — don't override.
        if (currentMode == PorkchopMode::MENU && !Menu::isActive()) {
            setMode(PorkchopMode::IDLE);
        }
        yield();
        return;
    }
    
    // In SETTINGS mode, let SettingsMenu handle everything
    if (currentMode == PorkchopMode::SETTINGS) {
        if (SettingsMenu::shouldExit()) {
            SettingsMenu::clearExit();
            SettingsMenu::hide();
            setMode(PorkchopMode::MENU);
        }
        return;
    }

    // In PIGSYNC_DEVICE_SELECT mode, handle navigation and channel switching
    if (currentMode == PorkchopMode::PIGSYNC_DEVICE_SELECT) {
        uint8_t deviceCount = PigSyncMode::getDeviceCount();

        // Handle device navigation (up/down) with joystick
        if (deviceCount > 0) {
            if (hal_input_wasPressed(KEY_UP)) {
                PigSyncMode::selectDevice(PigSyncMode::getSelectedIndex() > 0 ?
                    PigSyncMode::getSelectedIndex() - 1 : deviceCount - 1);
            }
            if (hal_input_wasPressed(KEY_DOWN)) {
                PigSyncMode::selectDevice((PigSyncMode::getSelectedIndex() + 1) % deviceCount);
            }
        }

        // ENTER/SELECT to connect
        if (hal_input_wasPressed(KEY_ENTER) && PigSyncMode::getDeviceCount() > 0) {
            uint8_t selectedIdx = PigSyncMode::getSelectedIndex();
            if (selectedIdx < PigSyncMode::getDeviceCount()) {
                PigSyncMode::connectTo(selectedIdx);
            }
        }

        // LEFT to abort sync (when connected)
        if (PigSyncMode::isConnected() && hal_input_wasPressed(KEY_LEFT)) {
            if (PigSyncMode::isSyncing()) {
                PigSyncMode::abortSync();
            }
        }

        // RIGHT to disconnect (when connected)
        if (PigSyncMode::isConnected() && hal_input_wasPressed(KEY_RIGHT)) {
            PigSyncMode::disconnect();
        }

        // SELECT (ENTER) to rescan (when not connected)
        if (!PigSyncMode::isConnected() && hal_input_wasPressed(KEY_ENTER)) {
            PigSyncMode::startScan();
        }

        return;
    }
    
    // ENTER from IDLE opens menu
    if (currentMode == PorkchopMode::IDLE && hal_input_wasPressed(KEY_ENTER)) {
        setMode(PorkchopMode::MENU);
        return;
    }
    
    // ENTER in About mode - easter egg
    if (currentMode == PorkchopMode::ABOUT && hal_input_wasPressed(KEY_ENTER)) {
        Display::onAboutEnterPressed();
        return;
    }
    
    // Joystick navigation in IDLE:
    // UP   → OINK (attack mode)
    // DOWN → DO NO HAM (passive mode)
    // LEFT → SPECTRUM
    // RIGHT→ WARHOG (wardriving)
    // ENTER→ menu (handled above)
    if (currentMode == PorkchopMode::IDLE) {
        if (hal_input_wasPressed(KEY_UP)) {
            setMode(PorkchopMode::OINK_MODE);
        } else if (hal_input_wasPressed(KEY_DOWN)) {
            setMode(PorkchopMode::DNH_MODE);
        } else if (hal_input_wasPressed(KEY_LEFT)) {
            setMode(PorkchopMode::SPECTRUM_MODE);
        } else if (hal_input_wasPressed(KEY_RIGHT)) {
            setMode(PorkchopMode::WARHOG_MODE);
        }
        yield();
    }
    
    // OINK mode - long-press ENTER to exclude current network (BOAR BRO), LEFT to switch to DNH
    if (currentMode == PorkchopMode::OINK_MODE) {
        if (hal_input_isLongEnter()) {
            int idx = OinkMode::getSelectionIndex();
            if (OinkMode::excludeNetwork(idx)) {
                Display::showToast("BOAR BRO ADDED!");
                delay(500);
                OinkMode::moveSelectionDown();
            } else {
                Display::showToast("ALREADY A BRO");
                delay(500);
            }
        }
        if (hal_input_wasPressed(KEY_LEFT)) {
            SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
            sess.passiveTimeStart = millis();
            Display::notify(NoticeKind::STATUS, "IRIE VIBES ONLY NOW", 0, NoticeChannel::TOP_BAR);
            delay(800);
            setMode(PorkchopMode::DNH_MODE);
            return;
        }
    }
    
    // DNH mode - RIGHT to switch back to OINK
    if (currentMode == PorkchopMode::DNH_MODE) {
        if (hal_input_wasPressed(KEY_RIGHT)) {
            SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
            sess.passiveTimeStart = 0;
            Display::notify(NoticeKind::STATUS, "PROPER MAD ONE INNIT", 0, NoticeChannel::TOP_BAR);
            delay(800);
            setMode(PorkchopMode::OINK_MODE);
            return;
        }
    }

    // Manual channel lock — UP/DOWN in OINK and DNH modes
    if (currentMode == PorkchopMode::OINK_MODE || currentMode == PorkchopMode::DNH_MODE) {
        bool pressedUp = hal_input_wasPressed(KEY_UP);
        bool pressedDown = hal_input_wasPressed(KEY_DOWN);
        if (pressedUp || pressedDown) {
            uint8_t ch;
            if (NetworkRecon::isManualChannelLocked()) {
                ch = NetworkRecon::getManualLockedChannel();
                if (pressedUp) {
                    ch = (ch >= 13) ? 1 : ch + 1;
                } else {
                    ch = (ch <= 1) ? 13 : ch - 1;
                }
            } else {
                ch = NetworkRecon::getCurrentChannel();
                if (ch < 1 || ch > 13) ch = 1;
            }
            NetworkRecon::setManualChannelLock(ch);
            // Sync local channel display — setManualChannelLock only updates
            // NetworkRecon::currentChannel, not the mode's local mirror.
            if (currentMode == PorkchopMode::OINK_MODE) {
                OinkMode::setChannel(ch);
            }
            char buf[24];
            snprintf(buf, sizeof(buf), "CH LOCK: %d", ch);
            Display::showToast(buf, 1500);
        }
    }
    
    // WARHOG mode - ESC returns to idle globally
    if (currentMode == PorkchopMode::WARHOG_MODE) {
        // no-op: ESC handled globally above
    }
    
    // PIGGYBLUES mode - ESC returns to idle globally
    if (currentMode == PorkchopMode::PIGGYBLUES_MODE) {
        // no-op: ESC handled globally above
    }
    
    // SPECTRUM mode - ESC returns to idle globally
    // If monitoring a network, Spectrum handles its own keys
    if (currentMode == PorkchopMode::SPECTRUM_MODE) {
        // no-op: ESC handled globally
    }
    
    // SNOUT mode - SELECT cycles tabs
    if (currentMode == PorkchopMode::SNOUT_MODE) {
        if (hal_input_wasPressed(KEY_ENTER)) {
            SnoutMode::setTab(SnoutMode::getTab() + 1);
        }
    }
    
    // FILE_TRANSFER mode - use ESC to return to idle
    if (currentMode == PorkchopMode::FILE_TRANSFER) {
        // no-op: ESC handled globally
    }
    
    yield(); // Allow other tasks to run after processing input
}

void Porkchop::updateMode() {
    switch (currentMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::update();
            break;
        case PorkchopMode::DNH_MODE:
            DoNoHamMode::update();
            break;
        case PorkchopMode::WARHOG_MODE:
            WarhogMode::update();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            PiggyBluesMode::update();
            break;
        case PorkchopMode::SPECTRUM_MODE:
            SpectrumMode::update();
            break;
        case PorkchopMode::BACON_MODE:
            BaconMode::update();
            // Check if user exited
            if (!BaconMode::isRunning()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::PORK_PATROL:
            PorkPatrolMode::update();
            break;
        case PorkchopMode::SWINE_RADAR:
            SwineRadarMode::update();
            break;
        case PorkchopMode::SNOUT_MODE:
            SnoutMode::update();
            break;
        case PorkchopMode::WEBUI_MODE:
            WebUI::update();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::update();
            if (!CapturesMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::update();
            if (!AchievementsMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::FILE_TRANSFER:
            FileServer::update();
            break;
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::update();
            if (!CrashViewer::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::update();
            if (!DiagnosticsMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::update();
            if (!SdFormatMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::update();
            if (!SwineStats::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::update();
            if (!BoarBrosMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::update();
            if (!WigleMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::update();
            if (!UnlockablesMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::update();
            if (!BountyStatusMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            // Update PigSync discovery process (includes dialogue phases)
            PigSyncMode::update();
            // Stay in device select mode for terminal display
            if (!PigSyncMode::isRunning()) {
                // User exited, go back to menu
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::CHARGING:
            ChargingMode::update();
            if (ChargingMode::shouldExit()) {
                ChargingMode::clearExit();
                setMode(PorkchopMode::IDLE);
            }
            break;
        
            hal_input_update();
            if (hal_input_shouldExit()) {
                setMode(PorkchopMode::IDLE);
            }
            break;
        default:
            break;
    }
}

uint32_t Porkchop::getUptime() const {
    return (millis() - startTime) / 1000;
}

uint16_t Porkchop::getHandshakeCount() const {
    // Include both handshakes and PMKIDs - both are crackable captures
    return OinkMode::getCompleteHandshakeCount() + OinkMode::getPMKIDCount();
}

uint16_t Porkchop::getNetworkCount() const {
    return OinkMode::getNetworkCount();
}

uint16_t Porkchop::getDeauthCount() const {
    return OinkMode::getDeauthCount();
}
