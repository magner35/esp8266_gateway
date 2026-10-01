/*
 * ESP8266 gateway for the SKE-02 flow meter.
 *
 * UART0 (GPIO1 TX / GPIO3 RX, 115200 8N1) talks to the meter service
 * console; UART1 (GPIO2, TX only) carries the debug log. The gateway
 * serves a web UI with the device parameters and a Modbus TCP slave.
 *
 * Boot flow: with stored WiFi credentials it joins the network and runs
 * the dashboard; otherwise (or on failure) it raises an AP with a captive
 * portal to enter the credentials, which are kept in the EEPROM sector.
 */

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include "ske02.h"
#include "web.h"
#include "modbus_tcp.h"
#include "storage.h"
#include "debug.h"

#define GW_HOSTNAME "ske02-gw"
#define GW_AP_PASS "ske02setup" /* WPA2 key of the setup AP */
#define GW_CONNECT_MS 20000     /* STA wait before AP fallback */
#define GW_STA_RESCAN_MS 30000

bool gwApMode = false; /* read by modbus_tcp.cpp */

#if defined(LED_BUILTIN) && LED_BUILTIN != 2
#define GW_LED 1 /* GPIO2 is taken by the debug UART */
#else
#define GW_LED 0
#endif

static DNSServer sDns;
static uint32_t sStaDownSince;

#if GW_LED
static void ledTask(void)
{
    static uint32_t t;
    static bool on;
    /* portal: fast blink; discovering: 300 ms; ready: solid */
    uint16_t period = gwApMode ? 120 : (skeReady() ? 0 : 300);
    if (!period)
    {
        digitalWrite(LED_BUILTIN, LOW); /* active low, "ready" = lit */
        return;
    }
    if (millis() - t >= period)
    {
        t = millis();
        on = !on;
        digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
    }
}
#endif

static void startStaServices(void)
{
    gwApMode = false;
    WiFi.setAutoReconnect(true);
    webSetup(false);
    mbSetup();
    DBG("main: sta mode, ip %s\n", WiFi.localIP().toString().c_str());
}

static void startApPortal(void)
{
    gwApMode = true;
    WiFi.mode(WIFI_AP);
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "SKE02-GW-%06X", ESP.getChipId());
    /* a fixed portal address keeps the captive redirect predictable */
    WiFi.softAPConfig(IPAddress(10, 0, 0, 1), IPAddress(10, 0, 0, 1),
                      IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid, GW_AP_PASS);
    sDns.start(53, "*", IPAddress(10, 0, 0, 1));
    webSetup(true);
    mbSetup();
    DBG("main: ap %s pass %s ip 10.0.0.1\n", ssid, GW_AP_PASS);
}

void setup()
{
#if GW_SKE_ENABLED
    Serial1.begin(115200); /* debug on GPIO2, UART0 belongs to the meter */
#else
    Serial.begin(115200); /* bench build: debug on the USB port, no meter */
#endif
    WiFi.persistent(false); /* creds live in our EEPROM, not in the SDK flash */
    DBG("\nmain: %s build\n", GW_SKE_ENABLED ? "production" : "usb-debug");
    DBG("main: reset: %s\n", ESP.getResetInfo().c_str());
    DBG("main: boot, heap %u\n", (unsigned)ESP.getFreeHeap());

#if GW_LED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
#endif

    storageLoad();
    skeBegin();

    if (storageSsid()[0])
    {
        WiFi.mode(WIFI_STA);
        WiFi.hostname(GW_HOSTNAME);
        WiFi.begin(storageSsid(), storagePass());
        DBG("wifi: joining \"%s\"\n", storageSsid());
        uint32_t t = millis();
        uint8_t lastSt = 0xFF;
        while (WiFi.status() != WL_CONNECTED && millis() - t < GW_CONNECT_MS)
        {
            skePoll(); /* the meter link starts working already here */
            delay(50);
            uint8_t st = (uint8_t)WiFi.status();
            if (st != lastSt)
            {
                lastSt = st;
                /* 0 idle, 1 no ssid, 3 connected, 4 bad password, 6 disconnected */
                DBG("wifi: status %u after %u ms\n", (unsigned)st,
                    (unsigned)(millis() - t));
            }
        }
    }

    if (WiFi.status() == WL_CONNECTED)
        startStaServices();
    else
    {
        DBG("wifi: sta join failed, starting the portal\n");
        startApPortal();
    }

    if (MDNS.begin(GW_HOSTNAME))
        MDNS.addService("http", "tcp", 80);
}

void loop()
{
    if (gwApMode)
        sDns.processNextRequest();

    webLoop();
    MDNS.update();
    mbLoop();
    skePoll();

    if (mbConsumeRestartRequest())
    {
        DBG("main: restart via modbus CONTROL\n");
        delay(100);
        ESP.restart();
    }

    /* STA watchdog: retry joining if the AP drops us for a while */
    if (!gwApMode)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            if (!sStaDownSince)
                sStaDownSince = millis();
            else if (millis() - sStaDownSince > GW_STA_RESCAN_MS)
            {
                sStaDownSince = millis();
                WiFi.reconnect();
            }
        }
        else
        {
            sStaDownSince = 0;
        }
    }

#if GW_LED
    ledTask();
#endif
    delay(1);
}
