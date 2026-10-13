/*
 * Хранилище .hex в выделенном разделе flash ("avrhex").
 * Формат: 4 байта длины текста (LE) + сам текст Intel HEX.
 * Перезаписывается целиком при каждой загрузке новой прошивки.
 */
#include <string.h>

#include "esp_partition.h"

#include "debug.h"
#include "hex_store.h"

static const esp_partition_t *sPart;
static uint32_t sLen;      /* длина hex-текста */
static uint32_t sPos;      /* позиция чтения */

bool hex_store_init(void)
{
    sPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_ANY, "avrhex");
    return sPart != NULL;
}

uint32_t hex_store_stored(void)
{
    uint8_t hdr[4];
    if (!sPart ||
        esp_partition_read(sPart, 0, hdr, sizeof(hdr)) != ESP_OK)
        return 0;
    return (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
           ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
}

/* Потоковая запись (из httpd чанками): стереть, записать заголовок
 * с полной длиной, потом дописывать тело. Вызывается редко - износ
 * раздела не проблема. */
static uint32_t sWrPos;

bool hex_store_begin(uint32_t len)
{
    uint8_t hdr[4];
    uint32_t erase;

    sWrPos = 0;
    if (!sPart || !len)
        return false;
    erase = (len + 4096 + 4095) & ~4095UL;
    if (erase > sPart->size)
    {
        DBG("hex: не влезает (%u байт текста)", (unsigned)len);
        return false;
    }
    if (esp_partition_erase_range(sPart, 0, erase) != ESP_OK)
        return false;
    hdr[0] = (uint8_t)len;
    hdr[1] = (uint8_t)(len >> 8);
    hdr[2] = (uint8_t)(len >> 16);
    hdr[3] = (uint8_t)(len >> 24);
    if (esp_partition_write(sPart, 0, hdr, sizeof(hdr)) != ESP_OK)
        return false;
    return true;
}

bool hex_store_chunk(const char *data, uint32_t n)
{
    if (!sPart || 4 + sWrPos + n > sPart->size)
        return false;
    if (esp_partition_write(sPart, 4 + sWrPos, data, n) != ESP_OK)
        return false;
    sWrPos += n;
    return true;
}

bool hex_store_write(const char *hex, uint32_t len)
{
    return hex_store_begin(len) && hex_store_chunk(hex, len);
}

/* последовательное чтение */
bool hex_store_open(void)
{
    sLen = hex_store_stored();
    sPos = 0;
    if (!sLen || sLen >= sPart->size)
        return false;
    return true;
}

uint32_t hex_store_read(char *buf, uint32_t cap)
{
    uint32_t n = sLen - sPos;
    if (n > cap)
        n = cap;
    if (!n)
        return 0;
    if (esp_partition_read(sPart, 4 + sPos, buf, n) != ESP_OK)
        return 0;
    sPos += n;
    return n;
}
