#ifndef GW_HEX_STORE_H
#define GW_HEX_STORE_H

#include <stdint.h>
#include <stdbool.h>

/* .hex в разделе avrhex: [4 байта длины][текст] */

bool hex_store_init(void);                 /* найти раздел (при старте) */
uint32_t hex_store_stored(void);           /* длина текста или 0 */
bool hex_store_write(const char *h, uint32_t len);
bool hex_store_begin(uint32_t len);       /* потоковая запись */
bool hex_store_chunk(const char *d, uint32_t n);
bool hex_store_open(void);                 /* начать чтение */
uint32_t hex_store_read(char *buf, uint32_t cap);

#endif
