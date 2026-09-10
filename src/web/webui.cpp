#include "webui.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>
#include "../ui/display.h"
#include "../core/porkchop.h"
#include "../core/network_recon.h"

extern Porkchop porkchop;

#define WEBUI_AP_SSID "PORKCHOP"
#define WEBUI_PORT    80

static bool _active = false;

static int  _listenSock = -1;
static char _rbuf[512];
static char _chunk[64];
static uint8_t _row[160];  // 80 pixels × 2 bytes
static char _cmdArg[32];
static char _resp[128];

static const char INDEX_HTML[] PROGMEM = R"RAW(<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>PCH</title></head><body style="background:#111;color:#f92a;font:9px monospace;text-align:center;margin:0;padding:4px">
<canvas id=c width=80 height=35 style="width:320px;height:140px;image-rendering:pixelated;border:2px solid #f92a;display:block;margin:2px auto"></canvas>
<div id=s style="color:#555;font-size:8px">connecting</div>
<div style="margin:2px"><button onclick=g('oink') style=color:#f44>OINK</button><button onclick=g('dnh')>DNH</button><button onclick=g('warhog')>HOG</button><button onclick=g('bacon')>BCN</button><button onclick=g('mode_idle')>IDLE</button><button onclick=g('menu')>MENU</button></div>
<div style="margin:2px"><button onclick=g('captures')>CAPS</button><button onclick=g('settings')>CFG</button><button onclick=g('stats')>STATS</button><button onclick=g('diag')>DIAG</button></div>
<script>
var H='http://192.168.4.1',c=document.getElementById('c'),x=c.getContext('2d'),s=document.getElementById('s');
function p(){fetch(H+'/screen',{cache:'no-store'}).then(function(r){return r.arrayBuffer();}).then(function(b){var h=b.byteLength/160,v=new DataView(b),img=x.createImageData(80,h),px=img.data;for(var i=0;i<80*h;i++){var pv=v.getUint16(i*2,false);px[i*4]=(pv>>8)&248;px[i*4+1]=(pv>>3)&252;px[i*4+2]=(pv<<3)&248;px[i*4+3]=255;}x.putImageData(img,0,0);s.textContent='live';setTimeout(p,600);}).catch(function(e){s.textContent='err:'+e.message;setTimeout(p,2000);});}
function g(cmd){fetch(H+'/cmd?c='+cmd,{cache:'no-store'}).catch(function(){});}
setTimeout(p,1000);
</script></body></html>)RAW";

static void _serveClient(int cfd) {
    for (int req = 0; req < 8; req++) {
        fd_set fds; FD_ZERO(&fds); FD_SET(cfd, &fds);
        struct timeval tv = {req == 0 ? 2 : 1, 0};
        int r = select(cfd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) break;

        memset(_rbuf, 0, sizeof(_rbuf));
        int n = recv(cfd, _rbuf, sizeof(_rbuf) - 1, 0);
        if (n <= 0) break;
        _rbuf[n] = 0;

        Serial.printf("[WEBUI] req%d(%d): %.50s\r\n", req, n, _rbuf);

        if ((uint8_t)_rbuf[0] == 0x16) break;  // TLS probe

        bool isRoot    = (strncmp(_rbuf, "GET / ", 6) == 0 || strncmp(_rbuf, "GET /\r", 6) == 0);
        bool isScreen  = (strncmp(_rbuf, "GET /screen", 11) == 0);
        bool isCmd     = (strncmp(_rbuf, "GET /cmd", 8) == 0);
        bool isFavicon = (strncmp(_rbuf, "GET /favicon", 12) == 0);
        bool isOptions = (strncmp(_rbuf, "OPTIONS", 7) == 0);

        if (isOptions) {
            const char* cors =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Access-Control-Allow-Private-Network: true\r\n"
                "Access-Control-Max-Age: 86400\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n\r\n";
            send(cfd, cors, strlen(cors), 0);
            continue;

        } else if (isRoot) {
            const char* hdr =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Private-Network: true\r\n"
                "Cache-Control: no-store\r\n\r\n";
            send(cfd, hdr, strlen(hdr), 0);
            const char* p = INDEX_HTML; uint8_t nb = 0;
            while (true) {
                uint8_t b = pgm_read_byte(p++);
                if (!b) { if (nb) send(cfd, _chunk, nb, 0); break; }
                _chunk[nb++] = b;
                if (nb == 64) { int s2 = 0, rem = 64; while (rem > 0) { int rv = send(cfd, _chunk + s2, rem, 0); if (rv <= 0) break; s2 += rv; rem -= rv; } nb = 0; }
            }

        } else if (isScreen) {
            TFT_eSprite* spr = Display::getMain().getSprite();
            const uint16_t outW = 80, outH = 35;  // 142/4 ≈ 35
            uint32_t bodyLen = (uint32_t)outW * outH * 2;
            char hdr[200];
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Private-Network: true\r\n"
                "Connection: keep-alive\r\n"
                "Content-Length: %lu\r\n\r\n", (unsigned long)bodyLen);
            send(cfd, hdr, strlen(hdr), 0);
            const uint8_t* buf = spr ? (const uint8_t*)spr->getPointer() : nullptr;
            uint32_t sent = 0;
            for (uint16_t y = 0; y < outH; y++) {
                if (buf) {
                    const uint8_t* src = buf + (y * 4) * 320;
                    for (uint16_t x = 0; x < outW; x++) {
                        uint8_t c = src[x * 4];
                        uint16_t r = ((c >> 5) & 7) * 31 / 7;
                        uint16_t g = ((c >> 2) & 7) * 63 / 7;
                        uint16_t b = (c & 3) * 31 / 3;
                        uint16_t px = (r << 11) | (g << 5) | b;
                        _row[x * 2] = px >> 8;
                        _row[x * 2 + 1] = px & 0xFF;
                    }
                } else {
                    memset(_row, 0, outW * 2);
                }
                int rv = send(cfd, (char*)_row, outW * 2, 0);
                if (rv > 0) sent += rv;
            }
            Serial.printf("[WEBUI] screen: %ux%u sent=%lu/%lu\r\n", outW, outH, sent, bodyLen);

        } else if (isCmd) {
            memset(_cmdArg, 0, sizeof(_cmdArg));
            char* q = strchr(_rbuf, '?');
            if (q) { char* cv = strstr(q, "c="); if (cv) { cv += 2; uint8_t i = 0; while (*cv && *cv != ' ' && *cv != '&' && i < 31) _cmdArg[i++] = *cv++; } }
            const char* reply = "OK";
            if      (!strcmp(_cmdArg, "mode_idle"))  porkchop.setMode(PorkchopMode::IDLE);
            else if (!strcmp(_cmdArg, "menu"))       porkchop.setMode(PorkchopMode::MENU);
            else if (!strcmp(_cmdArg, "captures"))   porkchop.setMode(PorkchopMode::CAPTURES);
            else if (!strcmp(_cmdArg, "stats"))      porkchop.setMode(PorkchopMode::SWINE_STATS);
            else if (!strcmp(_cmdArg, "diag"))       porkchop.setMode(PorkchopMode::DIAGNOSTICS);
            else if (!strcmp(_cmdArg, "settings"))   porkchop.setMode(PorkchopMode::SETTINGS);
            else if (!strcmp(_cmdArg, "oink"))       porkchop.setMode(PorkchopMode::OINK_MODE);
            else if (!strcmp(_cmdArg, "dnh"))        porkchop.setMode(PorkchopMode::DNH_MODE);
            else if (!strcmp(_cmdArg, "warhog"))     porkchop.setMode(PorkchopMode::WARHOG_MODE);
            else if (!strcmp(_cmdArg, "spectrum"))   porkchop.setMode(PorkchopMode::SPECTRUM_MODE);
            else if (!strcmp(_cmdArg, "bacon"))      porkchop.setMode(PorkchopMode::BACON_MODE);
            else if (!strcmp(_cmdArg, "piggyblues")) porkchop.setMode(PorkchopMode::PIGGYBLUES_MODE);
            else if (!strcmp(_cmdArg, "patrol"))     porkchop.setMode(PorkchopMode::PORK_PATROL);
            else if (!strcmp(_cmdArg, "radar"))      porkchop.setMode(PorkchopMode::SWINE_RADAR);
            else reply = "UNKNOWN";
            snprintf(_resp, sizeof(_resp),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: keep-alive\r\n"
                "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(reply), reply);
            send(cfd, _resp, strlen(_resp), 0);

        } else if (isFavicon) {
            const char* r204 = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
            send(cfd, r204, strlen(r204), 0);
        } else {
            const char* r404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(cfd, r404, strlen(r404), 0);
            break;
        }
    }
    shutdown(cfd, SHUT_WR);
    char drain[64]; while (recv(cfd, drain, sizeof(drain), 0) > 0) {}
    close(cfd);
}

void WebUI::start() {
    if (_active) return;

    // Fully stop NetworkRecon — cleaner WiFi state than pause()
    NetworkRecon::stop();
    
    WiFi.persistent(false);
    WiFi.scanDelete();
    
    // Soft disconnect — no driver teardown
    WiFi.disconnect(false, false);
    delay(200);
    
    // AP_STA mode — ESP32-S3 radio handles AP beacons better
    // when STA interface stays initialized
    WiFi.mode(WIFI_AP_STA);
    delay(500);
    
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    bool apOk = WiFi.softAP(WEBUI_AP_SSID);
    Serial.printf("[WEBUI] softAP: %s\r\n", apOk ? "OK" : "FAILED");
    
    delay(1000);  // beacon stabilization
    
    uint32_t t0 = millis();
    while (WiFi.softAPIP().toString() == "0.0.0.0" && millis() - t0 < 3000) delay(100);
    Serial.printf("[WEBUI] AP IP: %s  mode=%d  heap=%u\r\n",
        WiFi.softAPIP().toString().c_str(), (int)WiFi.getMode(), ESP.getFreeHeap());

    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(WEBUI_PORT);
    _listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1; setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int nd = 1;  setsockopt(_listenSock, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    fcntl(_listenSock, F_SETFL, O_NONBLOCK);
    bind(_listenSock, (struct sockaddr*)&sa, sizeof(sa));
    listen(_listenSock, 1);
    _active = true;
    Serial.printf("[WEBUI] listening on port %d  heap=%u\r\n", WEBUI_PORT, ESP.getFreeHeap());
    Display::showToast("WEBUI: 192.168.4.1", 4000);
}

void WebUI::stop() {
    if (!_active) return;
    if (_listenSock >= 0) { close(_listenSock); _listenSock = -1; }
    
    // Soft AP disconnect — don't power off radio (causes rxcb errors)
    WiFi.softAPdisconnect(false);
    delay(100);
    
    // Switch to STA for NetworkRecon
    WiFi.mode(WIFI_STA);
    delay(200);
    
    // Restart NetworkRecon (we fully stopped it in start())
    NetworkRecon::start();
    
    _active = false;
    Serial.println("[WEBUI] stopped — STA restored");
    Display::showToast("WEBUI OFF", 2000);
}

void WebUI::update() {
    if (!_active || _listenSock < 0) return;
    if (ESP.getMinFreeHeap() < 10000) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cfd = accept(_listenSock, (struct sockaddr*)&ca, &cl);
        if (cfd >= 0) { const char* busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n"; send(cfd, busy, strlen(busy), 0); close(cfd); }
        return;
    }
    struct sockaddr_in ca; socklen_t cl = sizeof(ca);
    int cfd = accept(_listenSock, (struct sockaddr*)&ca, &cl);
    if (cfd < 0) return;
    Serial.printf("[WEBUI] client %s heap=%u\r\n", inet_ntoa(ca.sin_addr), ESP.getFreeHeap());
    int sndbuf = 512; setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int nd = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    _serveClient(cfd);
    Serial.printf("[WEBUI] served heap=%u\r\n", ESP.getFreeHeap());
}

bool WebUI::isActive() { return _active; }
