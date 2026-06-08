#include "snout.h"
#include "oink.h"
#include "../core/network_recon.h"
#include "../core/wartales.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"
#include "../audio/sfx.h"
#include "../ui/display.h"
#include <esp_wifi.h>
#include <string.h>

// ============================================================================
// State
// ============================================================================

static bool     _running     = false;
static uint8_t  _tab         = 0;

// Evil twin detection
static SnoutMode::EvilTwin _twins[8];
static uint8_t  _twinCount   = 0;

// Deauth storm detection
static uint16_t _deauthsThisSec  = 0;
static uint16_t _deauthPeak      = 0;
static uint32_t _deauthWindow    = 0;
static bool     _stormActive     = false;
static uint32_t _stormStartMs    = 0;
static uint8_t  _stormSrcMac[6]  = {};

// Hidden SSID prober
static SnoutMode::HiddenResult _hidden[8];
static uint8_t  _hiddenCount = 0;
static uint32_t _lastProbe   = 0;
static uint8_t  _probeIdx    = 0;

// ============================================================================
// Promiscuous callback — deauth/disassoc frame counter
// ============================================================================

static void _mgmtCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const uint8_t* frame = pkt->payload;
    uint8_t subtype = (frame[0] >> 4) & 0x0F;
    // 0x0C = deauth, 0x0A = disassociation
    if (subtype == 0x0C || subtype == 0x0A) {
        _deauthsThisSec++;
        // Record transmitter MAC (bytes 10-15 of management frame)
        memcpy(_stormSrcMac, &frame[10], 6);
    }
}

// ============================================================================
// Evil Twin Detection
// ============================================================================

static void _scanEvilTwins() {
    NetworkRecon::enterCritical();
    auto& nets = NetworkRecon::getNetworks();
    uint8_t count = nets.size();
    if (count < 2) {
        NetworkRecon::exitCritical();
        return;
    }

    for (uint8_t i = 0; i < count && _twinCount < 8; i++) {
        if (nets[i].ssid[0] == '\0') continue;
        for (uint8_t j = i + 1; j < count && _twinCount < 8; j++) {
            if (nets[j].ssid[0] == '\0') continue;
            if (strcmp(nets[i].ssid, nets[j].ssid) != 0) continue;

            // Check if already recorded
            bool dup = false;
            for (uint8_t k = 0; k < _twinCount; k++) {
                if (strcmp(_twins[k].ssid, nets[i].ssid) == 0) {
                    _twins[k].seenAt = millis();
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            auto& t = _twins[_twinCount];
            strncpy(t.ssid, nets[i].ssid, 32);
            t.ssid[32] = '\0';
            memcpy(t.bssid1, nets[i].bssid, 6);
            memcpy(t.bssid2, nets[j].bssid, 6);
            t.auth1 = nets[i].authmode;
            t.auth2 = nets[j].authmode;
            t.rssi1 = nets[i].rssi;
            t.rssi2 = nets[j].rssi;
            t.seenAt = millis();
            _twinCount++;

            SFX::play(SFX::DEAUTH);
            Mood::setStatusMessage("EVIL TWIN!");
            Serial.printf("[SNOUT] EVIL TWIN: '%s' [%02X:%02X:%02X] vs [%02X:%02X:%02X]\n",
                t.ssid, t.bssid1[3], t.bssid1[4], t.bssid1[5],
                t.bssid2[3], t.bssid2[4], t.bssid2[5]);
        }
    }
    NetworkRecon::exitCritical();
}

// ============================================================================
// Hidden SSID Prober
// ============================================================================

static void _sendProbe(const uint8_t* bssid, uint8_t channel) {
    // Build minimal 802.11 probe request
    uint8_t frame[64] = {};
    // Frame control: probe request (0x40)
    frame[0] = 0x40;
    frame[1] = 0x00;
    // Duration
    frame[2] = 0x00; frame[3] = 0x00;
    // DA = broadcast
    memset(&frame[4], 0xFF, 6);
    // SA = our MAC
    esp_wifi_get_mac(WIFI_IF_STA, &frame[10]);
    // BSSID = target
    memcpy(&frame[16], bssid, 6);
    // Sequence control
    frame[22] = 0x00; frame[23] = 0x00;
    // SSID IE (empty = directed probe)
    frame[24] = 0x00;  // SSID element ID
    frame[25] = 0x00;  // length 0
    // Supported rates IE
    frame[26] = 0x01;  // Rates element ID
    frame[27] = 0x08;  // length
    frame[28] = 0x82; frame[29] = 0x84;
    frame[30] = 0x8B; frame[31] = 0x96;
    frame[32] = 0x0C; frame[33] = 0x12;
    frame[34] = 0x18; frame[35] = 0x24;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, 36, false);
}

static void _refreshHiddenList() {
    NetworkRecon::enterCritical();
    auto& nets = NetworkRecon::getNetworks();
    for (auto& n : nets) {
        if (!n.isHidden) continue;
        // Check if already in list
        bool found = false;
        for (uint8_t i = 0; i < _hiddenCount; i++) {
            if (memcmp(_hidden[i].bssid, n.bssid, 6) == 0) {
                found = true;
                break;
            }
        }
        if (!found && _hiddenCount < 8) {
            auto& h = _hidden[_hiddenCount];
            memcpy(h.bssid, n.bssid, 6);
            h.ssid[0] = '\0';
            h.channel = n.channel;
            h.revealed = false;
            _hiddenCount++;
        }
    }
    NetworkRecon::exitCritical();
}

static void _checkRevealedHidden() {
    NetworkRecon::enterCritical();
    auto& nets = NetworkRecon::getNetworks();
    for (uint8_t i = 0; i < _hiddenCount; i++) {
        if (_hidden[i].revealed) continue;
        for (auto& n : nets) {
            if (memcmp(_hidden[i].bssid, n.bssid, 6) == 0 && n.ssid[0] != '\0') {
                strncpy(_hidden[i].ssid, n.ssid, 32);
                _hidden[i].ssid[32] = '\0';
                _hidden[i].revealed = true;
                SFX::play(SFX::PMKID);
                Mood::setStatusMessage("HIDDEN REVEALED");
                Serial.printf("[SNOUT] HIDDEN REVEALED: '%s' [%02X:%02X:%02X]\n",
                    _hidden[i].ssid,
                    _hidden[i].bssid[3], _hidden[i].bssid[4], _hidden[i].bssid[5]);
                break;
            }
        }
    }
    NetworkRecon::exitCritical();
}

// ============================================================================
// Lifecycle
// ============================================================================

void SnoutMode::start() {
    if (_running) return;
    _running      = true;
    _twinCount    = 0;
    _hiddenCount  = 0;
    _probeIdx     = 0;
    _tab          = 0;
    _deauthsThisSec = 0;
    _deauthPeak   = 0;
    _stormActive  = false;
    _deauthWindow = millis();
    memset(_twins,  0, sizeof(_twins));
    memset(_hidden, 0, sizeof(_hidden));

    if (!NetworkRecon::isRunning()) NetworkRecon::start();

    // Enable MGMT frames for deauth detection
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    NetworkRecon::setPacketCallback(_mgmtCallback);

    Avatar::setState(AvatarState::HAPPY);
    Mood::setStatusMessage("snout to the ground");
    Display::showToast("SNOUT MODE ON", 1500);
    Serial.println("[SNOUT] started");
}

void SnoutMode::stop() {
    if (!_running) return;
    _running = false;
    NetworkRecon::setPacketCallback(nullptr);

    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);

    Avatar::setGrassMoving(false);
    Avatar::setState(AvatarState::NEUTRAL);
    Display::showToast("SNOUT OFF", 1200);
    Serial.printf("[SNOUT] stopped. twins=%u storm_peak=%u hidden=%u\n",
        _twinCount, _deauthPeak, _hiddenCount);
}

void SnoutMode::update() {
    if (!_running) return;
    uint32_t now = millis();

    // Deauth storm detection (1-second buckets)
    if (now - _deauthWindow >= 1000) {
        if (_deauthsThisSec >= 10 && !_stormActive) {
            _stormActive = true;
            _stormStartMs = now;
            SFX::play(SFX::DEAUTH);
            Mood::setStatusMessage("DEAUTH STORM!");
        }
        if (_deauthsThisSec < 3) {
            _stormActive = false;
        }
        if (_deauthsThisSec > _deauthPeak) {
            _deauthPeak = _deauthsThisSec;
        }
        _deauthsThisSec = 0;
        _deauthWindow = now;
    }

    // Evil twin scan every 3s
    static uint32_t lastTwinScan = 0;
    if (now - lastTwinScan >= 3000) {
        lastTwinScan = now;
        _scanEvilTwins();
    }

    // Hidden SSID refresh + reveal check every 4s
    static uint32_t lastHiddenScan = 0;
    if (now - lastHiddenScan >= 4000) {
        lastHiddenScan = now;
        _refreshHiddenList();
        _checkRevealedHidden();
    }

    // Directed probe every 800ms
    if (now - _lastProbe >= 800) {
        _lastProbe = now;
        if (_hiddenCount > 0) {
            // Find next unrevealed hidden network
            for (uint8_t tries = 0; tries < _hiddenCount; tries++) {
                if (!_hidden[_probeIdx].revealed) {
                    _sendProbe(_hidden[_probeIdx].bssid, _hidden[_probeIdx].channel);
                    break;
                }
                _probeIdx = (_probeIdx + 1) % _hiddenCount;
            }
            _probeIdx = (_probeIdx + 1) % _hiddenCount;
        }
    }
}

// ============================================================================
// Drawing
// ============================================================================

static void _drawTabBar(DisplayCanvas& canvas, int y) {
    static const char* tabs[] = {"EVIL TWIN", "D3AUTH", "H1DD3N"};
    uint16_t fg = COLOR_FG;
    uint16_t bg = COLOR_BG;
    int tabW = DISPLAY_W / 3;

    canvas.fillRect(0, y, DISPLAY_W, 14, fg);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);

    for (uint8_t t = 0; t < 3; t++) {
        if (t == _tab) {
            canvas.fillRect(t * tabW, y, tabW, 14, bg);
            canvas.setTextColor(fg, bg);
        } else {
            canvas.setTextColor(bg, fg);
        }
        canvas.drawString(tabs[t], t * tabW + tabW / 2, y + 3);
    }
    canvas.setTextDatum(top_left);
}

static void _drawEvilTwinTab(DisplayCanvas& canvas, int y) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    if (_twinCount == 0) {
        canvas.setTextDatum(top_center);
        canvas.drawString("no evil twins detected", DISPLAY_W / 2, y + 20);
        canvas.drawString("sniffing...", DISPLAY_W / 2, y + 36);
        return;
    }

    int lineY = y + 2;
    for (uint8_t i = 0; i < _twinCount && i < 4; i++) {
        auto& t = _twins[i];
        char ssidBuf[16];
        strncpy(ssidBuf, t.ssid, 14);
        ssidBuf[14] = '\0';
        char line[48];
        snprintf(line, sizeof(line), "!! %s", ssidBuf);
        canvas.drawString(line, 6, lineY);
        lineY += 12;
        snprintf(line, sizeof(line), " %02X:%02X:%02X vs %02X:%02X:%02X",
            t.bssid1[3], t.bssid1[4], t.bssid1[5],
            t.bssid2[3], t.bssid2[4], t.bssid2[5]);
        canvas.drawString(line, 6, lineY);
        lineY += 16;
    }
}

static void _drawDeauthTab(DisplayCanvas& canvas, int y) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    char line[48];
    snprintf(line, sizeof(line), "RATE: %u/sec", _deauthsThisSec);
    canvas.drawString(line, 6, y + 2);
    snprintf(line, sizeof(line), "PEAK: %u/sec", _deauthPeak);
    canvas.drawString(line, 6, y + 16);

    if (_stormActive) {
        canvas.setTextDatum(top_center);
        canvas.drawString("!! DEAUTH STORM !!", DISPLAY_W / 2, y + 38);
        canvas.setTextDatum(top_left);
        snprintf(line, sizeof(line), "SRC: %02X:%02X:%02X:%02X:%02X:%02X",
            _stormSrcMac[0], _stormSrcMac[1], _stormSrcMac[2],
            _stormSrcMac[3], _stormSrcMac[4], _stormSrcMac[5]);
        canvas.drawString(line, 6, y + 54);
    } else if (_deauthPeak == 0) {
        canvas.setTextDatum(top_center);
        canvas.drawString("watching for storms...", DISPLAY_W / 2, y + 38);
    } else {
        canvas.setTextDatum(top_center);
        canvas.drawString("storm subsided", DISPLAY_W / 2, y + 38);
    }
}

static void _drawHiddenTab(DisplayCanvas& canvas, int y) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    if (_hiddenCount == 0) {
        canvas.setTextDatum(top_center);
        canvas.drawString("no hidden nets found", DISPLAY_W / 2, y + 20);
        canvas.drawString("probing...", DISPLAY_W / 2, y + 36);
        return;
    }

    int lineY = y + 2;
    for (uint8_t i = 0; i < _hiddenCount && i < 5; i++) {
        auto& h = _hidden[i];
        char line[40];
        if (h.revealed) {
            snprintf(line, sizeof(line), ">> %s", h.ssid);
            canvas.drawString(line, 6, lineY);
        } else {
            snprintf(line, sizeof(line), "?? %02X:%02X:%02X:%02X:%02X:%02X",
                h.bssid[0], h.bssid[1], h.bssid[2],
                h.bssid[3], h.bssid[4], h.bssid[5]);
            canvas.drawString(line, 6, lineY);
        }
        lineY += 14;
    }
}

void SnoutMode::draw(DisplayCanvas& canvas) {
    int tabH = 14;
    int footerH = 12;
    int contentY = tabH + 2;
    int contentH = MAIN_H - tabH - footerH - 4;

    _drawTabBar(canvas, 0);

    switch (_tab) {
        case 0: _drawEvilTwinTab(canvas, contentY); break;
        case 1: _drawDeauthTab(canvas, contentY); break;
        case 2: _drawHiddenTab(canvas, contentY); break;
    }

    // Footer
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(bottom_center);
    canvas.drawString("SELECT=tab  ESC=exit", DISPLAY_W / 2, MAIN_H - 2);
    canvas.setTextDatum(top_left);
}

// ============================================================================
// Accessors
// ============================================================================

bool SnoutMode::isRunning()         { return _running; }
uint8_t SnoutMode::getTab()         { return _tab; }
void    SnoutMode::setTab(uint8_t t){ _tab = t % 3; }
uint8_t SnoutMode::getTwinCount()   { return _twinCount; }
uint8_t SnoutMode::getHiddenCount() { return _hiddenCount; }
bool    SnoutMode::isStormActive()  { return _stormActive; }
uint16_t SnoutMode::getDeauthPeak() { return _deauthPeak; }
uint16_t SnoutMode::getDeauthRate() { return _deauthsThisSec; }

void SnoutMode::getStormSrc(uint8_t* out) {
    memcpy(out, _stormSrcMac, 6);
}

void SnoutMode::getTwin(uint8_t i, char* ssid, uint8_t* b1, uint8_t* b2, uint8_t* a1, uint8_t* a2) {
    if (i >= _twinCount) return;
    strncpy(ssid, _twins[i].ssid, 33);
    memcpy(b1, _twins[i].bssid1, 6);
    memcpy(b2, _twins[i].bssid2, 6);
    *a1 = _twins[i].auth1;
    *a2 = _twins[i].auth2;
}

void SnoutMode::getHiddenEntry(uint8_t i, uint8_t* bssid, char* ssid, bool* revealed) {
    if (i >= _hiddenCount) return;
    memcpy(bssid, _hidden[i].bssid, 6);
    strncpy(ssid, _hidden[i].ssid, 33);
    *revealed = _hidden[i].revealed;
}
