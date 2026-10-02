#ifndef GW_SKE02_H
#define GW_SKE02_H

#include <Arduino.h>

/*
 * Client of the SKE-02 service console (USART0 of the flow meter,
 * 115200 8N1). Frame format and opcodes mirror include/console.h of the
 * firmware repository:
 *
 *   request:  [0xAA][cmd][id lo][id hi][len][payload...][crc8]
 *   reply:    [0xAA][cmd|0x80][id lo][id hi][len][payload...][crc8]
 *   crc8 over cmd..payload, poly 0x07, MSB first
 *
 * The module keeps a RAM cache of all device parameters:
 *   - types and values come from the binary READ / READ_MANY sweep,
 *   - names / sections / read-only flags come from the text 'l' listing
 *     (its type tokens are ambiguous: H8 prints as "U8", H16 as "U16").
 */

/* property type codes = prop_type_t of the firmware (+CMD for menu
 * command items, which have no value and are run, not set) */
enum
{
    SKT_FLOAT = 0, SKT_U32, SKT_TIME, SKT_DATE, SKT_U8, SKT_I8, SKT_H8,
    SKT_U16, SKT_H16, SKT_I16, SKT_BOOL, SKT_ENUM, SKT_STRING, SKT_STRINGP,
    SKT_CMD
};

/* console opcodes and status bytes (internal result codes) */
enum
{
    BS_OK = 0, BS_BAD_ID, BS_BAD_TYPE, BS_READONLY, BS_ACCESS, BS_WAIT
};

/* skeCommitRaw() result when the UART transaction itself failed */
#define SKE_ERR_TRANSPORT (-1)

/* Sized from a real device listing (177 params, 33 submenus, 2.5 KB of
 * option labels) plus headroom; the RAM budget is tight enough that
 * oversizing these kills the WiFi stack heap. */
#define SKE_MAX_PARAMS    192
#define SKE_NAME_LEN      44   /* menu texts are up to 20 CHARS = 41 UTF-8
                                * bytes (Cyrillic) + NUL */
#define SKE_MENU_LEN      44   /* one pool for L1 sections and submenus */
#define SKE_MAX_MENUS     40
#define SKE_MAX_SECTIONS  12   /* L1 sections tracked in order */
#define SKE_UART_BAUD     115200
#define SKE_TIMEOUT_MS    350
#define SKE_CHUNK         16   /* max READ_MANY records per frame (firmware limit) */
#define SKE_OPT_POOL      3584 /* NUL-separated enum/bool option labels
                                * (rusified device data needs 3132) */
#define SKE_OPT_MAX       16   /* options per parameter */

struct SkeParam
{
    char     name[SKE_NAME_LEN];
    uint32_t value;    /* raw LE payload, IEEE-754 bits for SKT_FLOAT */
    uint32_t minv;     /* raw numeric range, both 0 when not reported */
    uint32_t maxv;
    uint32_t updated;  /* millis() of the last refresh, 0 = never */
    uint16_t optPool;  /* offset into the option pool + 1, 0 = none */
    uint8_t  optCnt;   /* enum/bool option count */
    uint8_t  type;     /* SKT_*, 0 if unknown */
    uint8_t  section;  /* L1 submenu index (the web UI buttons) */
    uint8_t  group;    /* nearest submenu of any depth, for sub-grouping */
    uint8_t  groupLvl; /* level of the owning submenu (L1=1, L2=2, ...) */
    uint8_t  tab2;     /* owning L2 submenu (the tab), 0 = section root */
    bool     present;  /* readable through the binary protocol */
    bool     readOnly; /* MITEM_VIEW, marked '*' in the text listing */
    bool     masked;   /* password U32: shown as ****** by the console */
};

void        skeBegin(void);
void        skePoll(void);          /* state machine step, call from loop() */

bool        skeLinkUp(void);        /* console answered recently */
bool        skeReady(void);         /* parameter table fully built */
bool        skeCapturing(void);     /* text listing capture in progress */
uint16_t    skeCount(void);         /* parameter count reported by PING */
const char *skeVersion(void);       /* "SKE02/<fw>" from PING */
uint32_t    skeLastOkMs(void);      /* millis() of the last good transaction */

SkeParam   *skeGet(uint16_t id);    /* NULL when out of range */
const char *skeSectionName(uint8_t idx); /* 0-based position, "" if none */
int8_t      skeSectionIndexOf(uint8_t poolId); /* -1 when not a section */
const char *skeGroupName(uint8_t idx);   /* idx from SkeParam.group, "" if none */
uint16_t    skeSectionCount(void);
uint8_t     skeTypeSize(uint8_t type);
String      skeValueText(uint16_t id);
String      skeBoundText(const SkeParam *p, bool max); /* formatted range bound */
uint8_t     skeOptCount(const SkeParam *p);
const char *skeOptText(const SkeParam *p, uint8_t idx); /* NULL when out of range */

int         skeCommitRaw(uint16_t id, uint32_t raw); /* 0 ok, -1 transport, >0 console status */
bool        skeSetFromText(uint16_t id, const char *text, String &err);

/* access control: 'p' unlock / 'x' run a menu command item */
bool        skeUnlock(const char *pass, String &err); /* 'p <pass>' */
int         skeRunCommand(uint16_t id);               /* 'x <id>' */

/* live measurement values: the console 'm' command dumps MeterData_t
 * (meter.h of the firmware) as three CSV lines; bitwise members carry
 * the FSETPOINT_/FSTATUS_/FISR_ flag bits */
struct SkeValues
{
    float frequency, rate_raw, rate_fast, rateMLPM, rate;
    float total_plus, total_minus, total, total_sum;
    float totalml_plus, totalml_minus, gtotal, gtotalml;
    float kf_value, batch;
    uint32_t pulses_packet, pulses;
    uint8_t status, setpoint, isr;
    uint32_t updated; /* millis() of the last good frame, 0 = none */
};

const SkeValues *skeValues(void);  /* NULL until the first frame */
void        skeRescan(void);        /* wipe the cache, rediscover the device */
void        skeRebootDevice(void);  /* BC_REBOOT */
bool        skeRefresh(void);       /* on-demand full 'l' (the "Обновить" button) */

#endif /* GW_SKE02_H */
