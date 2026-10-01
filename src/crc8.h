#ifndef GW_CRC8_H
#define GW_CRC8_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-8, poly 0x07, init 0, MSB first - the same function the firmware
 * uses (_crc8_ccitt_update of avr-libc), see include/console.h.
 * The Init variant allows chaining over several buffers.
 */

static inline uint8_t gwCrc8Init(uint8_t crc, const uint8_t *p, size_t n)
{
    while (n--)
    {
        uint8_t b = crc ^ *p++;
        for (uint8_t i = 0; i < 8; i++)
            b = (b & 0x80) ? (uint8_t)((b << 1) ^ 0x07) : (uint8_t)(b << 1);
        crc = b;
    }
    return crc;
}

static inline uint8_t gwCrc8(const uint8_t *p, size_t n)
{
    return gwCrc8Init(0, p, n);
}

#endif /* GW_CRC8_H */
