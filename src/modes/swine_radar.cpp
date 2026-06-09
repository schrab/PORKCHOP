#include "swine_radar.h"
#include "oink.h"
#include "../core/network_recon.h"
#include "../core/wartales.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"
#include "../audio/sfx.h"
#include "../hal/hal_input.h"
#include "../ui/display.h"

static bool    _running = false;
static uint8_t _tab     = 0;

// ── Tab data arrays ───────
static DroneContact    _drones[4];    static uint8_t _droneCount = 0;
static TrackerContact  _tags[8];      static uint8_t _tagCount = 0;
static SuspectAP       _suspects[6];  static uint8_t _suspectCount = 0;
static SkimmerContact  _skimmers[6];  static uint8_t _skimmerCount = 0;
static HostileContact  _hostiles[8];  static uint8_t _hostileCount = 0;

// ── Stingray RSSI tracking ──
struct RssiSnapshot { uint8_t bssid[6]; int8_t rssi; };
static RssiSnapshot _prevScan[8];
static uint8_t      _prevCount = 0;

// ── Pwnagotchi MAC ──
static const uint8_t PWNA_MAC[6] = {0xDE,0xAD,0xBE,0xEF,0xDE,0xAD};

// ── Flipper Zero WiFi devboard OUIs ──
static const uint8_t FLIPPER_OUIS[][3] = {
    {0x24,0x6F,0x28},
    {0xA4,0xCF,0x12},
    {0x24,0xDC,0xC3},
    {0x08,0x3A,0xF2},
    {0x30,0xAE,0xA4},
};
static const uint8_t FLIPPER_OUI_COUNT = 5;

// ── Flipper / Marauder SSID patterns ──
static const char* FLIPPER_SSID_PATTERNS[] = {
    "BlackMagic", "marauder", "marlins", "flipper", "pwned",
};
static const uint8_t FLIPPER_SSID_COUNT = 5;

// ── Card skimmer OUIs ──
static const uint8_t SKIMMER_OUIS[][3] = {
    {0x00,0x1A,0x7D},
    {0x20,0x16,0x03},
    {0xE0,0xD0,0x83},
    {0x00,0x15,0x83},
};
static const uint8_t SKIMMER_OUI_COUNT = 4;

// ── Skimmer SSID patterns ──
static const char* SKIMMER_NAMES[] = {
    "HC-03","HC-05","HC-06","HC-08","HC-10",
    "FREE2MOVE","JDY-","MLT-BT05","BT04-","AT-09",
};
static const uint8_t SKIMMER_NAME_COUNT = 10;

// ── Tracker OUIs ──
static const uint8_t TRACKER_OUIS[][3] = {
    {0xF0,0x98,0x9D},
    {0x94,0x10,0x3E},
    {0xDC,0x56,0xE7},
    {0x3C,0xBD,0xD8},
    {0xFC,0x00,0x12},
    {0x88,0x36,0x6C},
    {0x00,0x17,0xC4},
};
static const uint8_t TRACKER_OUI_COUNT = 7;

static const char* TRACKER_VENDOR_NAMES[] = {
    "Apple AirTag", "Apple device",
    "Tile tracker", "Samsung SmartTag", "Samsung device",
    "Chipolo", "Pebblebee",
};

// ── Promiscuous callback for drone NAN frames + Pwnagotchi ──
static void IRAM_ATTR _radarCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    uint16_t len = pkt->rx_ctrl.sig_len; if (len>4) len-=4;
    if (len < 24) return;
    const uint8_t* p = pkt->payload;

    // Pwnagotchi: beacon from DE:AD:BE:EF:DE:AD
    if (p[0] == 0x80 && memcmp(p+10, PWNA_MAC, 6)==0) {
        if (_hostileCount < 8) {
            bool known = false;
            for (uint8_t i=0; i<_hostileCount; i++)
                if (memcmp(_hostiles[i].mac, PWNA_MAC, 6)==0) {
                    _hostiles[i].rssi=pkt->rx_ctrl.rssi;
                    _hostiles[i].lastSeen=millis();
                    _hostiles[i].seenCount++;
                    known=true; break;
                }
            if (!known) {
                HostileContact& h = _hostiles[_hostileCount++];
                memcpy(h.mac, PWNA_MAC, 6);
                strncpy(h.type, "PWNAGOTCHI", 15);
                if (len > 38 && p[36]==0x00) {
                    uint8_t slen = p[37];
                    if (slen>0 && slen<32 && 36+2+slen<len) { memcpy(h.detail,p+38,slen); h.detail[slen]=0; }
                    else strncpy(h.detail,"(no name)",47);
                } else strncpy(h.detail,"(no name)",47);
                h.rssi=pkt->rx_ctrl.rssi;
                h.firstSeen=h.lastSeen=millis();
                h.seenCount=1;
                Serial.printf("[RADAR] PWNAGOTCHI: %s rssi=%d\r\n", h.detail, h.rssi);
                Wartales::logDetection("PWNAGOTCHI", h.detail);
            }
        }
    }

    // NAN frame check
    if (p[0] != 0xD0) return;
    if (len < 32) return;
    const uint8_t* body = p + 24;
    if (body[0] != 0x04 || body[1] != 0x09) return;
    if (len < 30 || body[3] != 0x50 || body[4] != 0x6F || body[5] != 0x9A) return;
    // This is a NAN frame — likely OpenDroneID
    if (_droneCount < 4) {
        for (uint8_t i=0; i<_droneCount; i++)
            if (memcmp(_drones[i].mac, p+10, 6)==0) {
                _drones[i].rssi = pkt->rx_ctrl.rssi;
                _drones[i].lastSeen = millis();
                return;
            }
        DroneContact& d = _drones[_droneCount++];
        memcpy(d.mac, p+10, 6);
        snprintf(d.id, sizeof(d.id), "DRONE-%02X%02X%02X", d.mac[3], d.mac[4], d.mac[5]);
        d.lat = d.lon = d.alt = d.speed = 0;
        d.rssi = pkt->rx_ctrl.rssi;
        d.lastSeen = millis();
    }
}

// ── Tracker OUI check ──
static int8_t _trackerOUI(const uint8_t* mac) {
    for (uint8_t i=0; i<TRACKER_OUI_COUNT; i++)
        if (mac[0]==TRACKER_OUIS[i][0] && mac[1]==TRACKER_OUIS[i][1] && mac[2]==TRACKER_OUIS[i][2])
            return (int8_t)i;
    return -1;
}

// ── Scan threats ──
static void _scanThreats() {
    // -- Pass 1: Trackers + Stingray suspects --
    NetworkRecon::enterCritical();
    const auto& nets = NetworkRecon::getNetworks();

    uint8_t snapCount = 0;
    RssiSnapshot snap[8];
    for (const auto& n : nets) {
        if (snapCount >= 8) break;
        memcpy(snap[snapCount].bssid, n.bssid, 6);
        snap[snapCount].rssi = n.rssi;
        snapCount++;
    }

    for (const auto& n : nets) {
        int8_t tidx = _trackerOUI(n.bssid);
        if (tidx >= 0) {
            bool known = false;
            for (uint8_t i=0; i<_tagCount; i++)
                if (memcmp(_tags[i].mac, n.bssid, 6)==0) {
                    _tags[i].rssi = n.rssi; _tags[i].lastSeen = millis(); _tags[i].seenCount++;
                    known = true; break;
                }
            if (!known && _tagCount < 8) {
                TrackerContact& t = _tags[_tagCount++];
                memcpy(t.mac, n.bssid, 6);
                strncpy(t.vendor, TRACKER_VENDOR_NAMES[tidx], 15);
                t.rssi = n.rssi; t.firstSeen = t.lastSeen = millis(); t.seenCount = 1;
                Serial.printf("[RADAR] TRACKER: %s [%02X:%02X:%02X]\r\n",
                    t.vendor, n.bssid[3],n.bssid[4],n.bssid[5]);
                Wartales::logDetection("TRACKER", t.vendor);
            }
        }

        // Stingray heuristics
        bool suspect = false;
        char reason[32] = "";
        if (n.channel > 13) { suspect=true; strncpy(reason,"illegal channel",31); }
        else if (!n.ssid[0] && n.rssi > -45) { suspect=true; strncpy(reason,"hidden+strong sig",31); }
        else if (n.rssi > -40 && n.firstSeen == n.lastSeen) { suspect=true; strncpy(reason,"sudden strong AP",31); }
        else if (n.authmode == WIFI_AUTH_WEP && n.channel <= 13) { suspect=true; strncpy(reason,"WEP lure",31); }

        if (suspect) {
            bool known = false;
            for (uint8_t i=0; i<_suspectCount; i++)
                if (memcmp(_suspects[i].bssid, n.bssid, 6)==0) { known=true; break; }
            if (!known && _suspectCount < 6) {
                SuspectAP& s = _suspects[_suspectCount++];
                memcpy(s.bssid, n.bssid, 6);
                strncpy(s.reason, reason, 31);
                s.rssi = n.rssi; s.firstSeen = s.lastSeen = millis(); s.disappearCount = 0;
                Serial.printf("[RADAR] SUSPECT AP: [%02X:%02X:%02X] %s rssi=%d\r\n",
                    n.bssid[3],n.bssid[4],n.bssid[5], reason, n.rssi);
                Wartales::logDetection("SUSPECT AP", reason);
            }
        }
    }
    NetworkRecon::exitCritical();
    memcpy(_prevScan, snap, sizeof(snap));
    _prevCount = snapCount;

    // -- Pass 2: Skimmers --
    NetworkRecon::enterCritical();
    const auto& nets2 = NetworkRecon::getNetworks();
    for (const auto& n : nets2) {
        bool ouiHit = false;
        for (uint8_t i=0; i<SKIMMER_OUI_COUNT; i++)
            if (n.bssid[0]==SKIMMER_OUIS[i][0] && n.bssid[1]==SKIMMER_OUIS[i][1] && n.bssid[2]==SKIMMER_OUIS[i][2])
                { ouiHit=true; break; }
        bool nameHit = false;
        char lssid[33]={0}; int si=0;
        while(n.ssid[si] && si<32){ lssid[si]=(char)tolower((uint8_t)n.ssid[si]); si++; }
        for (uint8_t i=0; i<SKIMMER_NAME_COUNT && !nameHit; i++) {
            char lpat[16]={0}; int pi=0;
            while(SKIMMER_NAMES[i][pi] && pi<15){ lpat[pi]=(char)tolower((uint8_t)SKIMMER_NAMES[i][pi]); pi++; }
            if (strstr(lssid, lpat)) nameHit=true;
        }
        if (!ouiHit && !nameHit) continue;
        bool known = false;
        for (uint8_t i=0; i<_skimmerCount; i++)
            if (memcmp(_skimmers[i].mac, n.bssid, 6)==0) {
                _skimmers[i].rssi=n.rssi; _skimmers[i].lastSeen=millis(); _skimmers[i].seenCount++;
                known=true; break;
            }
        if (!known && _skimmerCount < 6) {
            SkimmerContact& s = _skimmers[_skimmerCount++];
            memcpy(s.mac, n.bssid, 6);
            strncpy(s.name, n.ssid[0]?n.ssid:"HC-??", 23);
            s.rssi=n.rssi; s.firstSeen=s.lastSeen=millis(); s.seenCount=1;
            Serial.printf("[RADAR] SKIMMER?: %s [%02X:%02X:%02X] %ddBm\r\n",
                s.name, n.bssid[3],n.bssid[4],n.bssid[5], n.rssi);
            SFX::play(SFX::DEAUTH);
            Mood::setStatusMessage("SKIMMER ALERT!");
            Wartales::logDetection("SKIMMER", s.name);
        }
    }

    // -- Pass 3: Flipper Zero --
    for (const auto& n : nets2) {
        bool flipperOUI = false;
        for (uint8_t i=0; i<FLIPPER_OUI_COUNT && !flipperOUI; i++)
            if (n.bssid[0]==FLIPPER_OUIS[i][0] && n.bssid[1]==FLIPPER_OUIS[i][1] && n.bssid[2]==FLIPPER_OUIS[i][2])
                flipperOUI = true;
        bool flipperSSID = false;
        char lssid[33]={0}; int si2=0;
        while(n.ssid[si2] && si2<32){ lssid[si2]=(char)tolower((uint8_t)n.ssid[si2]); si2++; }
        for (uint8_t i=0; i<FLIPPER_SSID_COUNT && !flipperSSID; i++) {
            char lpat[16]={0}; int pi=0;
            while(FLIPPER_SSID_PATTERNS[i][pi] && pi<15){ lpat[pi]=(char)tolower((uint8_t)FLIPPER_SSID_PATTERNS[i][pi]); pi++; }
            if (strstr(lssid, lpat)) flipperSSID = true;
        }
        if (!flipperOUI && !flipperSSID) continue;
        bool known = false;
        for (uint8_t i=0; i<_hostileCount; i++)
            if (memcmp(_hostiles[i].mac, n.bssid, 6)==0) {
                _hostiles[i].rssi=n.rssi; _hostiles[i].lastSeen=millis(); _hostiles[i].seenCount++;
                known=true; break;
            }
        if (!known && _hostileCount < 8) {
            HostileContact& h = _hostiles[_hostileCount++];
            memcpy(h.mac, n.bssid, 6);
            strncpy(h.type, "FLIPPER", 15);
            snprintf(h.detail, sizeof(h.detail), "%.32s", n.ssid[0]?n.ssid:"(no ssid)");
            h.rssi=n.rssi; h.firstSeen=h.lastSeen=millis(); h.seenCount=1;
            Serial.printf("[RADAR] FLIPPER ZERO: %s [%02X:%02X:%02X] %ddBm\r\n",
                h.detail, n.bssid[3],n.bssid[4],n.bssid[5], n.rssi);
            SFX::play(SFX::DEAUTH);
            Mood::setStatusMessage("FLIPPER DETECTED!");
            Wartales::logDetection("FLIPPER", h.detail);
        }
    }
    NetworkRecon::exitCritical();
}

// ── Public API ──

void SwineRadarMode::start() {
    if (_running) return;
    _running = true; _tab = 0;
    _droneCount = _tagCount = _suspectCount = _skimmerCount = _hostileCount = 0;
    memset(_drones,   0, sizeof(_drones));
    memset(_tags,     0, sizeof(_tags));
    memset(_suspects, 0, sizeof(_suspects));
    memset(_skimmers, 0, sizeof(_skimmers));
    memset(_hostiles, 0, sizeof(_hostiles));
    if (!NetworkRecon::isRunning()) NetworkRecon::start();
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    NetworkRecon::setPacketCallback(_radarCallback);
    Avatar::setState(AvatarState::HAPPY);
    Mood::setStatusMessage("radar is up");
    Display::showToast("SWINE RADAR ON", 1500);
    Serial.println("[RADAR] started");
}

void SwineRadarMode::stop() {
    if (!_running) return;
    _running = false;
    NetworkRecon::setPacketCallback(nullptr);
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    Avatar::setState(AvatarState::NEUTRAL);
    Display::showToast("RADAR OFF", 1200);
    Serial.printf("[RADAR] stopped. drones=%u tags=%u suspects=%u skimmers=%u hostiles=%u\r\n",
        _droneCount, _tagCount, _suspectCount, _skimmerCount, _hostileCount);
}

void SwineRadarMode::update() {
    if (!_running) return;

    // Tab switching via LEFT/RIGHT
    if (hal_input_wasPressed(KEY_LEFT))  setTab(_tab ? _tab - 1 : SWINE_RADAR_TAB_COUNT - 1);
    if (hal_input_wasPressed(KEY_RIGHT)) setTab((_tab + 1) % SWINE_RADAR_TAB_COUNT);

    static uint32_t _lastScan = 0;
    uint32_t now = millis();
    if (now - _lastScan > 3000) { _scanThreats(); _lastScan = now; }
    static uint32_t lm=0;
    if (now-lm > 5000) {
        char buf[32];
        snprintf(buf,sizeof(buf),"D:%u T:%u S:%u K:%u H:%u",
            _droneCount,_tagCount,_suspectCount,_skimmerCount,_hostileCount);
        Mood::setStatusMessage(buf); lm=now;
    }
}

void SwineRadarMode::draw(DisplayCanvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);

    // Tab bar
    static const char* tabs[] = {"DR0NE","TAGS","SUSPECT","SK1MMR","H0STILE"};
    int tw = canvas.width() / 5;
    for (uint8_t t=0; t<5; t++) {
        if (t==_tab) canvas.fillRect(t*tw, 0, tw, 13, COLOR_FG);
        canvas.setTextColor(t==_tab?COLOR_BG:COLOR_FG, t==_tab?COLOR_FG:COLOR_BG);
        canvas.setTextDatum(TC_DATUM);
        canvas.drawString(tabs[t], t*tw+tw/2, 3);
    }
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(TL_DATUM);
    int y = 16; const int dy = 11;

    if (_tab == 0) {
        if (_droneCount==0) {
            canvas.setTextDatum(TC_DATUM);
            canvas.drawString("skies are clear", canvas.width()/2, y+6); y+=dy;
            canvas.drawString("watching for RemoteID...", canvas.width()/2, y+6);
        } else {
            for (uint8_t i=0; i<_droneCount && i<4; i++) {
                char id[21]={0}; float la,lo,al,sp;
                SwineRadarMode::getDroneInfo(i,id,&la,&lo,&al,&sp);
                char line[40]; snprintf(line,sizeof(line),"!! %s",id);
                canvas.drawString(line, 4, y); y+=dy;
                if (la != 0 || lo != 0) {
                    snprintf(line,sizeof(line),"   %.4f,%.4f @%.0fm",la,lo,al);
                    canvas.drawString(line, 4, y); y+=dy;
                }
            }
        }
    } else if (_tab == 1) {
        if (_tagCount==0) {
            canvas.setTextDatum(TC_DATUM);
            canvas.drawString("no trackers detected", canvas.width()/2, y+6); y+=dy;
            canvas.drawString("AirTag/Tile/SmartTag...", canvas.width()/2, y+6);
        } else {
            for (uint8_t i=0; i<_tagCount && i<5; i++) {
                uint8_t mac[6]; char vnd[16]={0}; int8_t rssi;
                SwineRadarMode::getTagInfo(i,mac,vnd,&rssi);
                char line[40];
                snprintf(line,sizeof(line),"!! %s",vnd);
                canvas.drawString(line, 4, y); y+=dy;
                snprintf(line,sizeof(line),"   %02X:%02X:%02X %ddBm",mac[3],mac[4],mac[5],rssi);
                canvas.drawString(line, 4, y); y+=dy;
            }
        }
    } else if (_tab == 2) {
        if (_suspectCount==0) {
            canvas.setTextDatum(TC_DATUM);
            canvas.drawString("no suspect APs", canvas.width()/2, y+6); y+=dy;
            canvas.drawString("watching for IMSI bait...", canvas.width()/2, y+6);
        } else {
            for (uint8_t i=0; i<_suspectCount && i<5; i++) {
                uint8_t bssid[6]; char reason[32]={0}; int8_t rssi;
                SwineRadarMode::getSuspectInfo(i,bssid,reason,&rssi);
                char line[40];
                snprintf(line,sizeof(line),"!! %02X:%02X:%02X %ddBm",bssid[3],bssid[4],bssid[5],rssi);
                canvas.drawString(line, 4, y); y+=dy;
                snprintf(line,sizeof(line),"   %s",reason);
                canvas.drawString(line, 4, y); y+=dy;
            }
        }
    }
    if (_tab == 3) {
        if (_skimmerCount==0) {
            canvas.setTextDatum(TC_DATUM);
            canvas.drawString("no skimmers detected", canvas.width()/2, y+6); y+=dy;
            canvas.drawString("HC-05/06/08 FREE2MOVE...", canvas.width()/2, y+6);
        } else {
            for (uint8_t i=0; i<_skimmerCount && i<5; i++) {
                uint8_t mac[6]; char name[24]={0}; int8_t rssi;
                SwineRadarMode::getSkimmerInfo(i,mac,name,&rssi);
                char line[40];
                snprintf(line,sizeof(line),"!! %s %ddBm",name,rssi);
                canvas.drawString(line, 4, y); y+=dy;
                snprintf(line,sizeof(line),"   %02X:%02X:%02X",mac[3],mac[4],mac[5]);
                canvas.drawString(line, 4, y); y+=dy;
            }
        }
    }
    if (_tab == 4) {
        if (_hostileCount==0) {
            canvas.setTextDatum(TC_DATUM);
            canvas.drawString("no hostiles in range", canvas.width()/2, y+6); y+=dy;
            canvas.drawString("pwna/flipper/pineapple...", canvas.width()/2, y+6);
        } else {
            for (uint8_t i=0; i<_hostileCount && i<4; i++) {
                uint8_t mac[6]; char type[16]={0}; char detail[48]={0}; int8_t rssi;
                SwineRadarMode::getHostileInfo(i,mac,type,detail,&rssi);
                char line[48];
                snprintf(line,sizeof(line),"!! %s %ddBm", type, rssi);
                canvas.drawString(line, 4, y); y+=dy;
                if (detail[0]) {
                    snprintf(line,sizeof(line),"   %.28s", detail);
                    canvas.drawString(line, 4, y); y+=dy;
                }
                snprintf(line,sizeof(line),"   %02X:%02X:%02X:%02X:%02X:%02X",
                    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                canvas.drawString(line, 4, y); y+=dy;
            }
        }
    }
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString("</> TABS  SELECT=exit", canvas.width()/2, canvas.height()-10);
    canvas.setTextDatum(TL_DATUM);
}

bool SwineRadarMode::isRunning()              { return _running; }
uint8_t SwineRadarMode::getTab()              { return _tab; }
void    SwineRadarMode::setTab(uint8_t t)     { _tab = t % 5; }
uint8_t SwineRadarMode::getDroneCount()       { return _droneCount; }
uint8_t SwineRadarMode::getTagCount()         { return _tagCount; }
uint8_t SwineRadarMode::getSuspectCount()     { return _suspectCount; }
uint8_t SwineRadarMode::getSkimmerCount()     { return _skimmerCount; }
uint8_t SwineRadarMode::getHostileCount()     { return _hostileCount; }

void SwineRadarMode::getHostileInfo(uint8_t i, uint8_t* mac, char* type, char* detail, int8_t* rssi) {
    if (i>=_hostileCount) return;
    memcpy(mac, _hostiles[i].mac, 6);
    strncpy(type,   _hostiles[i].type,   15);
    strncpy(detail, _hostiles[i].detail, 47);
    *rssi = _hostiles[i].rssi;
}

void SwineRadarMode::getSkimmerInfo(uint8_t i, uint8_t* mac, char* name, int8_t* rssi) {
    if (i>=_skimmerCount) return;
    memcpy(mac, _skimmers[i].mac, 6);
    strncpy(name, _skimmers[i].name, 23);
    *rssi = _skimmers[i].rssi;
}

void SwineRadarMode::getDroneInfo(uint8_t i, char* id, float* lat, float* lon, float* alt, float* spd) {
    if (i>=_droneCount) return;
    strncpy(id, _drones[i].id, 20);
    *lat=_drones[i].lat; *lon=_drones[i].lon;
    *alt=_drones[i].alt; *spd=_drones[i].speed;
}

void SwineRadarMode::getTagInfo(uint8_t i, uint8_t* mac, char* vendor, int8_t* rssi) {
    if (i>=_tagCount) return;
    memcpy(mac, _tags[i].mac, 6);
    strncpy(vendor, _tags[i].vendor, 15);
    *rssi = _tags[i].rssi;
}

void SwineRadarMode::getSuspectInfo(uint8_t i, uint8_t* bssid, char* reason, int8_t* rssi) {
    if (i>=_suspectCount) return;
    memcpy(bssid, _suspects[i].bssid, 6);
    strncpy(reason, _suspects[i].reason, 31);
    *rssi = _suspects[i].rssi;
}
