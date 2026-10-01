#ifndef GW_STORAGE_H
#define GW_STORAGE_H

#include <stdint.h>

/* Persistent gateway settings in the EEPROM flash sector. */

void        storageLoad(void);
bool        storageSave(void);

const char *storageSsid(void);
const char *storagePass(void);
void        storageSetWifi(const char *ssid, const char *pass);

uint8_t     storagePoll100ms(void);
void        storageSetPoll100ms(uint8_t v);

#endif /* GW_STORAGE_H */
