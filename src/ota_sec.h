#ifndef GW_OTA_SEC_H
#define GW_OTA_SEC_H

/*
 * Проверка OTA-пакетов .skpak (tools/ota_pack.py): Ed25519-подпись
 * заголовка + AES-128-CTR расшифровка потока + SHA-256 открытых данных.
 * Контекст живёт на стеке вызывающего (~470 Б), образ целиком в ОЗУ
 * не нужен: feed() расшифровывает in-place чанками.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mbedtls/aes.h>
#include <sodium.h>

#define OTA_SEC_HDR 152
#define OTA_SEC_ESP 1 /* образ ESP (firmware.bin) */
#define OTA_SEC_AVR 2 /* hex прибора */

enum
{
    OTA_SEC_OK = 0,
    OTA_SEC_MAGIC = -1,   /* не .skpak */
    OTA_SEC_TYPE = -2,    /* неизвестный тип пакета */
    OTA_SEC_LEN = -3,     /* размер открытых данных несуразен */
    OTA_SEC_OVERRUN = -4, /* пришло больше байт, чем заявлено */
    OTA_SEC_SHORT = -5,   /* пришло меньше байт, чем заявлено */
    OTA_SEC_HASH = -6,    /* расшифрованные данные не совпали с хешем */
    OTA_SEC_SIGN = -7,    /* подпись неверна */
    OTA_SEC_NOTIMG = -8   /* расшифрованное - не образ ESP / не hex */
};

typedef struct
{
    /* заголовок */
    uint8_t type;
    uint32_t plain_len;
    char note[25];
    /* потоковое состояние */
    mbedtls_aes_context aes;
    uint8_t ctr[16];  /* текущий счётчик CTR (инициал = nonce) */
    uint8_t stream[16];
    size_t nc_off;
    crypto_hash_sha256_state sha;
    uint8_t hdr0[64];  /* подписанные поля (magic..sha256) */
    uint8_t sig[64];   /* подпись Ed25519 */
    uint32_t fed;      /* скормлено байт открытых данных */
    bool active;
} ota_sec_t;

/* Разобрать заголовок (OTA_SEC_HDR байт) и запустить потоки.
 * max_plain = верхний предел открытых данных (размер ota-слота/раздела).
 * Ошибки OTA_SEC_MAGIC/TYPE/LEN возвращаются ДО начала расшифровки. */
int ota_sec_begin(ota_sec_t *s, const uint8_t *hdr, uint32_t max_plain);

/* Расшифровать чанк in-place и накормить SHA-256.
 * Сверх plain_len - ошибка OTA_SEC_OVERRUN (данные отбрасываются). */
int ota_sec_feed(ota_sec_t *s, uint8_t *buf, uint32_t len);

/* Финал: полнота + хеш + подпись Ed25519. */
int ota_sec_finish(ota_sec_t *s);

const char *ota_sec_str(int rc); /* текст ошибки для веб-ответа */

#endif /* GW_OTA_SEC_H */
