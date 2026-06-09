#include "pork_patrol.h"
#include "oink.h"
#include "../core/network_recon.h"
#include "../core/wartales.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"
#include "../audio/sfx.h"
#include "../gps/gps.h"
#include "../ui/display.h"

static bool       _running = false;
static uint32_t   _lastScan = 0;
static uint32_t   _totalDetected = 0;
static PorkPatrolDetection _hits[8];
static uint8_t    _hitCount = 0;

// SSID substrings: Flock Safety (case-insensitive)
static const char* SSID_PATTERNS[] = {
    "flock", "fs ext", "pigvision", "penguin", "flockca",
};
static const uint8_t SSID_PATTERN_COUNT = 5;

// Bodycam SSID patterns — Axon/Motorola BWC
static const char* BWC_SSID_PATTERNS[] = {
    "axon", "bwcviewer", "evidence", "taser", "motorola bwc",
    "vievu", "v300", "v500", "ibr",
};
static const uint8_t BWC_SSID_PATTERN_COUNT = 9;

// Flock Safety OUI prefixes (from deflock.me dataset)
static const uint8_t FLOCK_OUIS[][3] = {
    {0x00,0x17,0xF2}, {0x00,0x1D,0xC9}, {0x18,0x0F,0x76},
    {0x20,0x02,0xAF}, {0x24,0xA4,0x3C}, {0x2C,0xCF,0x67},
    {0x34,0xE8,0x94}, {0x3C,0x5A,0xB4}, {0x40,0x9F,0x38},
    {0x44,0xD9,0xE7}, {0x48,0x2C,0x6A}, {0x50,0xC7,0xBF},
    {0x54,0xAF,0x97}, {0x5C,0xBA,0xEF}, {0x60,0x38,0xE0},
    {0x6C,0x40,0x08}, {0x70,0x3A,0xCB}, {0x74,0xDA,0x38},
    {0x78,0x8A,0x20}, {0x7C,0x1E,0xB3},
};
static const uint8_t FLOCK_OUI_COUNT = 20;

// Axon Enterprise OUI prefixes — bodycam WiFi modules
static const uint8_t AXON_OUIS[][3] = {
    {0x00,0x25,0xDF},
    {0x00,0x17,0xF2},
    {0xB4,0xE6,0x2D},
};
static const uint8_t AXON_OUI_COUNT = 3;

static bool _ssidMatch(const char* ssid, uint8_t& typeOut) {
    if (!ssid || !ssid[0]) return false;
    char lower[33]; int i=0;
    while (ssid[i] && i<32) { lower[i]=(char)tolower((uint8_t)ssid[i]); i++; }
    lower[i]=0;
    for (uint8_t p=0; p<SSID_PATTERN_COUNT; p++)
        if (strstr(lower, SSID_PATTERNS[p])) { typeOut=0; return true; }
    for (uint8_t p=0; p<BWC_SSID_PATTERN_COUNT; p++)
        if (strstr(lower, BWC_SSID_PATTERNS[p])) { typeOut=1; return true; }
    return false;
}

static bool _ouiMatch(const uint8_t* bssid, uint8_t& typeOut) {
    for (uint8_t i=0; i<FLOCK_OUI_COUNT; i++)
        if (bssid[0]==FLOCK_OUIS[i][0] && bssid[1]==FLOCK_OUIS[i][1] && bssid[2]==FLOCK_OUIS[i][2])
            { typeOut=0; return true; }
    for (uint8_t i=0; i<AXON_OUI_COUNT; i++)
        if (bssid[0]==AXON_OUIS[i][0] && bssid[1]==AXON_OUIS[i][1] && bssid[2]==AXON_OUIS[i][2])
            { typeOut=1; return true; }
    return false;
}

static void _addHit(const char* ssid, const uint8_t* bssid, int8_t rssi, uint8_t type) {
    for (uint8_t i=0; i<_hitCount; i++) {
        if (memcmp(_hits[i].bssid, bssid, 6)==0) {
            _hits[i].rssi = rssi;
            _hits[i].lastSeen = millis();
            _hits[i].hitCount++;
            return;
        }
    }
    if (_hitCount >= 8) return;
    PorkPatrolDetection& d = _hits[_hitCount++];
    strncpy(d.ssid, ssid, 32); d.ssid[32]=0;
    memcpy(d.bssid, bssid, 6);
    d.rssi = rssi;
    d.firstSeen = d.lastSeen = millis();
    d.hitCount = 1;
    d.fresh = true;
    d.type = type;
    _totalDetected++;
    SFX::play(SFX::ACHIEVEMENT);
    const char* label = (type==1) ? "BODYCAM" : "FLOCK CAM";
    char msg[24]; snprintf(msg,sizeof(msg),"%s SPOTTED",label);
    Mood::setStatusMessage(msg);
    Serial.printf("[PATROL] %s: %s [%02X:%02X:%02X:%02X:%02X:%02X] rssi=%d\r\n",
        label, ssid, bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5], rssi);
    auto gd = GPS::getData();
    if (gd.fix && gd.valid)
        Serial.printf("[PATROL] GPS: %.6f,%.6f\r\n", gd.latitude, gd.longitude);
    Wartales::logDetection(label, ssid[0] ? ssid : "hidden");
}

static void _scanNetworks() {
    NetworkRecon::enterCritical();
    const auto& nets = NetworkRecon::getNetworks();
    for (const auto& n : nets) {
        uint8_t type = 0;
        bool hit = _ssidMatch(n.ssid, type) || _ouiMatch(n.bssid, type);
        if (hit) _addHit(n.ssid, n.bssid, n.rssi, type);
    }
    NetworkRecon::exitCritical();
}

void PorkPatrolMode::start() {
    if (_running) return;
    _running = true;
    _hitCount = 0;
    _totalDetected = 0;
    _lastScan = 0;
    if (!NetworkRecon::isRunning()) NetworkRecon::start();
    Avatar::setState(AvatarState::HAPPY);
    Avatar::setGrassMoving(true);
    Avatar::setGrassSpeed(80);
    Mood::setStatusMessage("oink oink oink");
    Display::showToast("PORK PATROL ON", 1500);
    Serial.println("[PATROL] started — sniffing for fed cams");
}

void PorkPatrolMode::stop() {
    if (!_running) return;
    _running = false;
    Avatar::setGrassMoving(false);
    Avatar::setState(AvatarState::NEUTRAL);
    Display::showToast("PATROL ENDED", 1500);
    Serial.printf("[PATROL] stopped. %lu flock cams detected\r\n", _totalDetected);
}

void PorkPatrolMode::update() {
    if (!_running) return;
    uint32_t now = millis();
    if (now - _lastScan > 2000) { _scanNetworks(); _lastScan = now; }
    static uint32_t lm=0;
    if (now-lm > 5000) {
        char buf[32];
        if (_hitCount > 0) snprintf(buf,sizeof(buf),"FEDS:%u spotted",_hitCount);
        else                strncpy(buf,"oink oink oink...",sizeof(buf));
        Mood::setStatusMessage(buf); lm=now;
    }
}

void PorkPatrolMode::draw(DisplayCanvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString("P0RK PATR0L", canvas.width()/2, 2);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawLine(0, 14, canvas.width(), 14, COLOR_FG);

    uint8_t flockCount=0, bwcCount=0;
    for (uint8_t i=0; i<_hitCount; i++)
        (_hits[i].type==1 ? bwcCount : flockCount)++;

    char status[48];
    int y = 18; const int dy = 12;
    if (_hitCount==0)
        snprintf(status,sizeof(status),"sniffing... [%u nets]", NetworkRecon::getNetworkCount());
    else
        snprintf(status,sizeof(status),"FLOCK:%u  BODYCAM:%u  TOTAL:%u", flockCount, bwcCount, _hitCount);
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString(status, canvas.width()/2, y); y+=dy+4;
    canvas.setTextDatum(TL_DATUM);
    canvas.drawLine(0, y, canvas.width(), y, COLOR_FG); y+=4;

    if (_hitCount==0) {
        canvas.setTextDatum(TC_DATUM);
        canvas.drawString("the pig is watching", canvas.width()/2, y+2); y+=dy;
        canvas.drawString("no feds detected nearby", canvas.width()/2, y+2); y+=dy;
        canvas.drawString("oink oink oink...", canvas.width()/2, y+2);
    } else {
        for (uint8_t i=0; i<_hitCount && i<5; i++) {
            char _ssid[33]={0}; uint8_t _mac[6]={0};
            PorkPatrolMode::getHitSSID(i,_ssid,32); PorkPatrolMode::getHitMAC(i,_mac);
            uint8_t _type=PorkPatrolMode::getHitType(i);
            int8_t _rssi=PorkPatrolMode::getHitRssi(i);
            char mac[10]; snprintf(mac,sizeof(mac),"%02X%02X%02X",_mac[3],_mac[4],_mac[5]);
            const char* tag = (_type==1)?"[BWC]":"[CAM]";
            char line[40]; snprintf(line,sizeof(line),"%s %.12s [%s] %ddBm",tag,_ssid[0]?_ssid:"??",mac,_rssi);
            canvas.drawString(line, 4, y); y+=dy;
        }
        auto gd = GPS::getData();
        if (gd.fix && gd.valid) {
            char gl[40]; snprintf(gl,sizeof(gl),"GPS:%.4f,%.4f",gd.latitude,gd.longitude);
            canvas.setTextDatum(TC_DATUM);
            canvas.drawString(gl, canvas.width()/2, canvas.height()-10);
        }
    }
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(COLOR_FG);
}

bool PorkPatrolMode::isRunning()          { return _running; }
uint32_t PorkPatrolMode::getTotalDetected() { return _totalDetected; }
uint8_t PorkPatrolMode::getDetectionCount() { return _hitCount; }
uint8_t PorkPatrolMode::getHitType(uint8_t i) { if(i>=_hitCount)return 0; return _hits[i].type; }
int8_t  PorkPatrolMode::getHitRssi(uint8_t i) { if(i>=_hitCount)return 0; return _hits[i].rssi; }
void PorkPatrolMode::getHitSSID(uint8_t i, char* buf, uint8_t len) { if(i<_hitCount) strncpy(buf,_hits[i].ssid,len); else buf[0]=0; }
void PorkPatrolMode::getHitMAC(uint8_t i, uint8_t* out) { if(i<_hitCount) memcpy(out,_hits[i].bssid,6); else memset(out,0,6); }
