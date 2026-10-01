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

/* property type codes = prop_type_t of the firmware */
enum
{
    SKT_FLOAT = 0, SKT_U32, SKT_TIME, SKT_DATE, SKT_U8, SKT_I8, SKT_H8,
    SKT_U16, SKT_H16, SKT_I16, SKT_BOOL, SKT_ENUM, SKT_STRING, SKT_STRINGP
};

/* console opcodes and status bytes */
enum
{
    BC_PING = 1, BC_READ, BC_WRITE, BC_READ_MANY, BC_REBOOT
};
enum
{
    BS_OK = 0, BS_BAD_ID, BS_BAD_TYPE, BS_READONLY
};

/* skeCommitRaw() result when the UART transaction itself failed */
#define SKE_ERR_TRANSPORT (-1)

#define SKE_MAX_PARAMS    240
#define SKE_NAME_LEN      42   /* 20 LCD chars -> up to 40 UTF-8 bytes + NUL */
#define SKE_SECTION_LEN   42
#define SKE_MAX_SECTIONS  24
#define SKE_UART_BAUD     115200
#define SKE_TIMEOUT_MS    350
#define SKE_CHUNK         16   /* max READ_MANY records per frame (firmware limit) */

struct SkeParam
{
    char     name[SKE_NAME_LEN];
    uint32_t value;    /* raw LE payload, IEEE-754 bits for SKT_FLOAT */
    uint32_t updated;  /* millis() of the last refresh, 0 = never */
    uint8_t  type;     /* SKT_*, 0 if unknown */
    uint8_t  section;  /* index into the section pool, 0 = none */
    bool     present;  /* readable through the binary protocol */
    bool     readOnly; /* MITEM_VIEW, marked '*' in the text listing */
};

void        skeBegin(void);
void        skePoll(void);          /* state machine step, call from loop() */

bool        skeLinkUp(void);        /* console answered recently */
bool        skeReady(void);         /* parameter table fully built */
uint16_t    skeCount(void);         /* parameter count reported by PING */
const char *skeVersion(void);       /* "SKE02/<fw>" from PING */
uint32_t    skeLastOkMs(void);      /* millis() of the last good transaction */

SkeParam   *skeGet(uint16_t id);    /* NULL when out of range */
const char *skeSectionName(uint8_t idx); /* idx from SkeParam.section, "" if none */
uint8_t     skeTypeSize(uint8_t type);
String      skeValueText(uint16_t id);

int         skeCommitRaw(uint16_t id, uint32_t raw); /* 0 ok, -1 transport, >0 console status */
bool        skeSetFromText(uint16_t id, const char *text, String &err);

void        skeRescan(void);        /* wipe the cache, rediscover the device */
void        skeRebootDevice(void);  /* BC_REBOOT */

#endif /* GW_SKE02_H */
