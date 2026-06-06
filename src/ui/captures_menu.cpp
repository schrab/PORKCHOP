// Captures Menu - View saved handshake captures

#include "captures_menu.h"
#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include "display.h"
#include "../web/wpasec.h"
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../core/wifi_utils.h"
#include "../core/heap_health.h"
#include <esp_heap_caps.h>

// Static member initialization
std::vector<CaptureInfo> CapturesMenu::captures;
uint8_t CapturesMenu::selectedIndex = 0;
uint8_t CapturesMenu::scrollOffset = 0;
bool CapturesMenu::active = false;
bool CapturesMenu::keyWasPressed = false;
bool CapturesMenu::nukeConfirmActive = false;
bool CapturesMenu::detailViewActive = false;
bool CapturesMenu::scanInProgress = false;
unsigned long CapturesMenu::lastScanTime = 0;
File CapturesMenu::scanDir;
File CapturesMenu::currentFile;
bool CapturesMenu::scanComplete = false;
size_t CapturesMenu::scanProgress = 0;
bool CapturesMenu::wpasecUpdateInProgress = false;
unsigned long CapturesMenu::lastWpasecUpdateTime = 0;
size_t CapturesMenu::wpasecUpdateProgress = 0;

// Hint rotation
uint8_t CapturesMenu::hintIndex = 0;
const char* const CapturesMenu::HINTS[] = {
    "FEED YO HASHCAT.",
    "COLLECTED PAIN. COMPRESSED.",
    "ENT:DET  S:SYNC  D:NUKE",
    "MALLOC SAID NAH.",
    "YOUR LOOT. YOUR PROBLEM."
};

// WPA-SEC Sync state
bool CapturesMenu::syncModalActive = false;
SyncState CapturesMenu::syncState = SyncState::IDLE;
char CapturesMenu::syncStatusText[48] = "";
uint8_t CapturesMenu::syncProgress = 0;
uint8_t CapturesMenu::syncTotal = 0;
unsigned long CapturesMenu::syncStartTime = 0;
uint8_t CapturesMenu::syncUploaded = 0;
uint8_t CapturesMenu::syncFailed = 0;
uint16_t CapturesMenu::syncCracked = 0;
char CapturesMenu::syncError[48] = "";

void CapturesMenu::init() {
    captures.clear();
    selectedIndex = 0;
    scrollOffset = 0;
}

void CapturesMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
    hintIndex = esp_random() % HINT_COUNT;

    // If scan fails, the captures list will remain empty
    // This is handled by the draw function which shows "No captures found"
    scanCaptures();
}

void CapturesMenu::hide() {
    active = false;
    
    // FIX: Always call emergencyCleanup first - ensures file handles closed
    emergencyCleanup();
    
    // Enhanced: Force cleanup even if interrupted
    captures.clear();
    captures.shrink_to_fit();  // Release vector capacity
    WPASec::freeCacheMemory();
    
    // Reset all async state to prevent leaks (redundant after emergencyCleanup but safe)
    scanInProgress = false;
    wpasecUpdateInProgress = false;
    if (scanDir) {
        scanDir.close();
    }
    if (currentFile) {
        currentFile.close();
    }
}

void CapturesMenu::emergencyCleanup() {
    // Can be called from main loop when heap is critical
    if (!active) return;
    
    Serial.println("[CAPTURES] Emergency cleanup triggered");
    captures.clear();
    captures.shrink_to_fit();
    WPASec::freeCacheMemory();
    
    // Stop any in-progress operations
    scanInProgress = false;
    wpasecUpdateInProgress = false;
    if (scanDir) {
        scanDir.close();
    }
    if (currentFile) {
        currentFile.close();
    }
}

bool CapturesMenu::scanCaptures() {
    // Initialize async scan
    captures.clear();
    captures.reserve(MAX_CAPTURES);  // Full upfront reserve — no mid-scan reallocations

    // Guard: Skip if no SD card available
    if (!Config::isSDAvailable()) {
        Serial.println("[CAPTURES] No SD card available");
        scanComplete = true;
        scanInProgress = false;
        return false;
    }

    // Guard: Skip SD scan at Warning+ pressure — file ops allocate FAT buffers
    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Warning) {
        Serial.println("[CAPTURES] Scan deferred: heap pressure");
        scanComplete = true;
        scanInProgress = false;
        return false;
    }

    // Create directory if it doesn't exist
    const char* handshakesDir = SDLayout::handshakesDir();
    if (!SD.exists(handshakesDir)) {
        Serial.println("[CAPTURES] No handshakes directory, creating...");
        if (!SD.mkdir(handshakesDir)) {
            Serial.println("[CAPTURES] Failed to create handshakes directory");
            scanComplete = true;
            scanInProgress = false;
            return false;
        }
    }

    scanDir = SD.open(handshakesDir);
    if (!scanDir || !scanDir.isDirectory()) {
        Serial.println("[CAPTURES] Failed to open handshakes directory");
        scanComplete = true;
        scanInProgress = false;
        scanDir.close();
        return false;
    }

    scanInProgress = true;
    scanComplete = false;
    scanProgress = 0;
    lastScanTime = millis();
    
    return true;
}

// Helper: check if string of length n is all hex chars
static bool isAllHex(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

void CapturesMenu::processAsyncScan() {
    if (!scanInProgress || scanComplete) {
        return;
    }

    // Throttle the scan to avoid blocking the UI
    if (millis() - lastScanTime < SCAN_DELAY) {
        return;
    }

    lastScanTime = millis();

    // Process a chunk of files
    size_t processed = 0;
    while (processed < SCAN_CHUNK_SIZE && !scanComplete) {
        currentFile = scanDir.openNextFile();

        if (!currentFile) {
            // No more files, we're done with scanning
            scanComplete = true;
            scanInProgress = false;
            scanDir.close();

            // Sort by capture time (newest first)
            std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
                return a.captureTime > b.captureTime;
            });

            // Start async WPA-SEC status update after scanning is complete
            if (!captures.empty()) {
                wpasecUpdateInProgress = true;
                wpasecUpdateProgress = 0;
                lastWpasecUpdateTime = millis();
            }

            Serial.printf("[CAPTURES] Async scan complete. Found %d captures\n", captures.size());
            break;
        }

        // Zero-String scan: use const char* from File directly
        const char* name = currentFile.name();
        size_t nameLen = strlen(name);

        bool isPCAP = (nameLen > 5 && strcmp(name + nameLen - 5, ".pcap") == 0);
        bool isHS22000 = (nameLen > 9 && strcmp(name + nameLen - 9, "_hs.22000") == 0);
        bool isPMKID = !isHS22000 && (nameLen > 6 && strcmp(name + nameLen - 6, ".22000") == 0);

        // Skip PCAP if we have the corresponding _hs.22000 (avoid duplicates).
        if (isPCAP) {
            // Build base name: everything before the dot
            const char* dot = strrchr(name, '.');
            size_t baseLen = dot ? (size_t)(dot - name) : nameLen;
            char hs22kPath[80];
            snprintf(hs22kPath, sizeof(hs22kPath), "%s/%.*s_hs.22000",
                     SDLayout::handshakesDir(), (int)baseLen, name);
            if (SD.exists(hs22kPath)) {
                currentFile.close();
                processed++;
                continue;
            }
        }

        if (isPCAP || isPMKID || isHS22000) {
            CaptureInfo info;
            memset(&info, 0, sizeof(info));
            strncpy(info.filename, name, sizeof(info.filename) - 1);
            info.fileSize = currentFile.size();
            info.captureTime = currentFile.getLastWrite();
            info.isPMKID = isPMKID;

            // Compute base name (strip extension and _hs suffix)
            const char* dot = strrchr(name, '.');
            size_t baseLen = dot ? (size_t)(dot - name) : nameLen;
            if (baseLen > 3 && strncmp(name + baseLen - 3, "_hs", 3) == 0) {
                baseLen -= 3;
            }

            // Dual-format detection:
            // Legacy: base name is exactly 12 hex chars (BSSID only)
            // New format: last 12 chars are hex, preceded by '_' (SSID_BSSID)
            if (baseLen == 12 && isAllHex(name, 12)) {
                // Legacy format: BSSID is first 12 chars
                const char* b = name;
                snprintf(info.bssid, sizeof(info.bssid),
                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                         b, b+2, b+4, b+6, b+8, b+10);

                // Try companion .txt for SSID (legacy files)
                char txtPath[80];
                if (isPMKID) {
                    snprintf(txtPath, sizeof(txtPath), "%s/%.12s_pmkid.txt",
                             SDLayout::handshakesDir(), name);
                } else {
                    snprintf(txtPath, sizeof(txtPath), "%s/%.12s.txt",
                             SDLayout::handshakesDir(), name);
                }
                if (SD.exists(txtPath)) {
                    File txtFile = SD.open(txtPath, FILE_READ);
                    if (txtFile) {
                        char buf[34];
                        int n = txtFile.readBytesUntil('\n', buf, sizeof(buf) - 1);
                        buf[n] = '\0';
                        while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\r' || buf[n-1] == '\t')) buf[--n] = '\0';
                        if (n > 0) {
                            strncpy(info.ssid, buf, sizeof(info.ssid) - 1);
                        }
                        txtFile.close();
                    }
                }
            } else if (baseLen > 13 && name[baseLen - 13] == '_' &&
                       isAllHex(name + baseLen - 12, 12)) {
                // New format: SSID_BSSID — extract BSSID from last 12 chars
                const char* b = name + baseLen - 12;
                snprintf(info.bssid, sizeof(info.bssid),
                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                         b, b+2, b+4, b+6, b+8, b+10);

                // Extract SSID from chars before _BSSID
                size_t ssidLen = baseLen - 13;
                if (ssidLen > sizeof(info.ssid) - 1) ssidLen = sizeof(info.ssid) - 1;
                memcpy(info.ssid, name, ssidLen);
                info.ssid[ssidLen] = '\0';
            } else {
                // Unknown format — use full base as BSSID display
                size_t copyLen = baseLen < sizeof(info.bssid) - 1 ? baseLen : sizeof(info.bssid) - 1;
                memcpy(info.bssid, name, copyLen);
                info.bssid[copyLen] = '\0';
            }

            if (info.ssid[0] == '\0') {
                strncpy(info.ssid, "[UNKNOWN]", sizeof(info.ssid) - 1);
            }

            info.status = CaptureStatus::LOCAL;

            captures.push_back(info);

            if (captures.size() >= MAX_CAPTURES) {
                scanComplete = true;
                scanInProgress = false;
                currentFile.close();
                scanDir.close();
                Serial.println("[CAPTURES] Hit capture limit, stopped scan");
                break;
            }
        }

        currentFile.close();
        processed++;
        scanProgress++;

        if (processed >= SCAN_CHUNK_SIZE) {
            break;
        }
    }
}

void CapturesMenu::updateWPASecStatus() {
    // Load WPA-SEC cache (lazy, only loads once)
    WPASec::loadCache();
    
    char normalized[13] = {0};
    for (auto& cap : captures) {
        // Normalize BSSID for lookup (remove colons)
        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
        if (normalized[0] == '\0') {
            cap.status = CaptureStatus::LOCAL;
            continue;
        }
        
        if (WPASec::isCracked(normalized)) {
            cap.status = CaptureStatus::CRACKED;
            strncpy(cap.password, WPASec::getPassword(normalized), sizeof(cap.password) - 1);
            cap.password[sizeof(cap.password) - 1] = '\0';
        } else if (WPASec::isUploaded(normalized)) {
            cap.status = CaptureStatus::UPLOADED;
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
    }
}

void CapturesMenu::processAsyncWPASecUpdate() {
    if (!wpasecUpdateInProgress || captures.empty()) {
        wpasecUpdateInProgress = false;
        return;
    }
    
    // Throttle the update to avoid blocking the UI
    if (millis() - lastWpasecUpdateTime < WPASEC_UPDATE_DELAY) {
        return;
    }
    
    lastWpasecUpdateTime = millis();
    
    // Process a chunk of captures
    size_t processed = 0;
    while (processed < WPASEC_UPDATE_CHUNK_SIZE && wpasecUpdateProgress < captures.size()) {
        auto& cap = captures[wpasecUpdateProgress];
        
        // Normalize BSSID for lookup (remove colons)
        char normalized[13] = {0};
        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
        
        if (normalized[0] != '\0') {
            if (WPASec::isCracked(normalized)) {
                cap.status = CaptureStatus::CRACKED;
                strncpy(cap.password, WPASec::getPassword(normalized), sizeof(cap.password) - 1);
                cap.password[sizeof(cap.password) - 1] = '\0';
            } else if (WPASec::isUploaded(normalized)) {
                cap.status = CaptureStatus::UPLOADED;
            } else {
                cap.status = CaptureStatus::LOCAL;
            }
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
        
        wpasecUpdateProgress++;
        processed++;
        
        // Yield periodically to allow other tasks to run
        if (processed >= WPASEC_UPDATE_CHUNK_SIZE) {
            // Still more to do, but yield control back to other tasks
            break;
        }
    }
    
    // Check if we're done with all captures
    if (wpasecUpdateProgress >= captures.size()) {
        wpasecUpdateInProgress = false;
        Serial.printf("[CAPTURES] Async WPA-SEC update complete. Updated %d captures\n", captures.size());
    }
}

void CapturesMenu::update() {
    if (!active) return;
    
    // Process sync state machine if active
    if (syncModalActive && syncState != SyncState::IDLE && 
        syncState != SyncState::COMPLETE && syncState != SyncState::ERROR) {
        processSyncState();
    }
    
    // Process async file scanning if in progress (not during sync)
    if (!syncModalActive) {
        processAsyncScan();
        
        // Process async WPA-SEC status updates if in progress
        processAsyncWPASecUpdate();
    }
    
    handleInput();
}

void CapturesMenu::handleInput() {
    bool anyPressed = hal_input_anyHeld();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = /* keysState replaced */;

    // Handle sync modal
    if (syncModalActive) {
        if (syncState == SyncState::ERROR || syncState == SyncState::COMPLETE) {
            // Enter closes the modal after completion/error
            if (hal_input_wasPressed(KEY_ENTER) || hal_input_wasPressed(KEY_BACKSPACE)) {
                syncModalActive = false;
                syncState = SyncState::IDLE;
                scanCaptures();  // Rescan captures after sync
            }
        } else {
            // ESC cancels during sync
            if (hal_input_wasPressed(KEY_BACKSPACE)) {
                cancelSync();
            }
        }
        return;  // Block other inputs during sync
    }

    // Handle nuke confirmation modal
    if (nukeConfirmActive) {
        if (hal_input_wasPressed('y') || hal_input_wasPressed('Y')) {
            nukeLoot();
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
            scanCaptures();  // Refresh list (should be empty now)
        } else if (hal_input_wasPressed('n') || hal_input_wasPressed('N') ||
                   hal_input_wasPressed(KEY_BACKSPACE) || hal_input_wasPressed(KEY_ENTER)) {
            nukeConfirmActive = false;  // Cancel
            Display::clearBottomOverlay();
        }
        return;
    }
    
    // Handle detail view modal - Enter/backspace closes
    if (detailViewActive) {
        if (hal_input_wasPressed(KEY_ENTER) || hal_input_wasPressed(KEY_BACKSPACE)) {
            detailViewActive = false;
            return;
        }
        return;  // Block other inputs while detail view is open
    }
    
    // Navigation with ; (up) and . (down) — also rotates hints
    if (hal_input_wasPressed(KEY_UP)) {
        hintIndex = (hintIndex + 1) % HINT_COUNT;
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }

    if (hal_input_wasPressed(KEY_DOWN)) {
        hintIndex = (hintIndex + 1) % HINT_COUNT;
