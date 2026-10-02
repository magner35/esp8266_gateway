#include "storage.h"
#include "crc8.h"
#include "debug.h"
#include <EEPROM.h>

/*
 * One struct in the EEPROM flash sector, protected by magic + CRC8 so a
 * blank/foreign sector falls back to defaults instead of garbage.
 */

#define SETTINGS_MAGIC  0x534BU /* "SK" */
#define SETTINGS_POLL_DEFAULT 1U /* g poll step: 1 x 100 ms per param */

struct Settings
{
    uint16_t magic;
    uint8_t  poll100ms;   /* UART poll period, 100 ms units */
    uint8_t  rfu;         /* keep the chars aligned */
    char     ssid[33];
    char     pass[65];
    uint8_t  crc;         /* CRC8 of everything above */
};

static Settings s;

static uint8_t settingsCrc(const Settings *st)
{
    /* CRC over the payload only: everything BEFORE the crc member.
       offsetof is a must here - the struct has a trailing alignment pad
       byte, so sizeof-1 would wrongly cover the crc byte itself and the
       check could never pass after a power cycle. */
    return gwCrc8((const uint8_t *)st, offsetof(Settings, crc));
}

void storageLoad(void)
{
    EEPROM.begin(sizeof(Settings) + 8);
    EEPROM.get(0, s);
    if (s.magic != SETTINGS_MAGIC || settingsCrc(&s) != s.crc)
    {
        s.magic = SETTINGS_MAGIC;
        s.poll100ms = SETTINGS_POLL_DEFAULT;
        s.rfu = 0;
        s.ssid[0] = 0;
        s.pass[0] = 0;
        DBG("storage: blank/invalid, defaults loaded\n");
    }
    DBG("storage: ssid \"%s\", poll %u00 ms\n", s.ssid,
        (unsigned)s.poll100ms);
}

bool storageSave(void)
{
    s.crc = settingsCrc(&s);
    EEPROM.put(0, s);
    bool ok = EEPROM.commit();
    if (!ok)
        DBG("storage: EEPROM commit FAILED\n");
    return ok;
}

const char *storageSsid(void) { return s.ssid; }
const char *storagePass(void) { return s.pass; }

void storageSetWifi(const char *ssid, const char *pass)
{
    strncpy(s.ssid, ssid, sizeof(s.ssid) - 1);
    s.ssid[sizeof(s.ssid) - 1] = 0;
    strncpy(s.pass, pass, sizeof(s.pass) - 1);
    s.pass[sizeof(s.pass) - 1] = 0;
}

uint8_t storagePoll100ms(void) { return s.poll100ms ? s.poll100ms : SETTINGS_POLL_DEFAULT; }

void storageSetPoll100ms(uint8_t v)
{
    s.poll100ms = (v >= 1 && v <= 255) ? v : SETTINGS_POLL_DEFAULT;
}
