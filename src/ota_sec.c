/*
 * Проверка OTA-пакетов .skpak. Формат и криптосхема - tools/ota_pack.py:
 * подпись Ed25519 стоит над полями magic..sha256 заголовка (64 Б),
 * полезная нагрузка зашифрована AES-128-CTR (nonce из заголовка,
 * big-endian инкремент всего 128-битного счётчика - совместимо и с
 * mbedTLS, и с python-cryptography). Порядок обработки: сначала
 * расшифровываем поток и копим SHA-256, в конце сверяем хеш и подпись;
 * подмена любого байта шифротекста меняет открытый текст и ломает хеш.
 *
 * Ключи сгенерированы tools/ota_pack.py genkeys (tools/keys/, вне git).
 * Публичный ключ и AES-ключ прошиты здесь: извлечение их с шлюза даёт
 * только возможность ЧИТАТЬ образы и НЕ даёт подделать подпись -
 * приватный ключ не покидает хост упаковки.
 */
#include <string.h>

#include "ota_sec.h"

static const uint8_t kEdPub[] = {
    0x44, 0xCB, 0x76, 0xCF, 0xB1, 0x45, 0xED, 0x60,
    0xC4, 0xBA, 0x5B, 0xB9, 0x51, 0x3E, 0xF4, 0x50,
    0x6E, 0xDE, 0x5F, 0xF8, 0x97, 0x06, 0xDC, 0x60,
    0x65, 0x00, 0xFA, 0xF7, 0x8A, 0xBC, 0x84, 0x0D,
};
static const uint8_t kAesKey[] = {
    0x68, 0x2A, 0xDB, 0x14, 0xCA, 0x99, 0x66, 0x33,
    0xA3, 0xA8, 0x19, 0x29, 0x69, 0xF0, 0xF3, 0x92,
};

static const char kMagic[8] = "SKE-OTA1";

int ota_sec_begin(ota_sec_t *s, const uint8_t *hdr, uint32_t max_plain)
{
    uint32_t len;

    memset(s, 0, sizeof(*s));
    if (memcmp(hdr, kMagic, 8) != 0)
        return OTA_SEC_MAGIC;
    if (hdr[8] != OTA_SEC_ESP && hdr[8] != OTA_SEC_AVR)
        return OTA_SEC_TYPE;
    memcpy(&len, hdr + 12, 4); /* little-endian */
    if (len == 0 || len > max_plain)
        return OTA_SEC_LEN;

    s->type = hdr[8];
    s->plain_len = len;
    memcpy(s->note, hdr + 64, 24);
    s->note[24] = 0;
    memcpy(s->hdr0, hdr, 64);   /* подписанные поля */
    memcpy(s->sig, hdr + 88, 64);

    sodium_init();
    crypto_hash_sha256_init(&s->sha);
    mbedtls_aes_init(&s->aes);
    if (mbedtls_aes_setkey_enc(&s->aes, kAesKey, 128) != 0)
        return OTA_SEC_LEN; /* невозможно при живом ключе */
    memcpy(s->ctr, hdr + 16, 16); /* nonce = начальный счётчик */
    s->nc_off = 0;
    s->fed = 0;
    s->active = true;
    return OTA_SEC_OK;
}

int ota_sec_feed(ota_sec_t *s, uint8_t *buf, uint32_t len)
{
    if (!s->active)
        return OTA_SEC_OVERRUN;
    if (s->fed + len > s->plain_len)
    {
        s->active = false;
        return OTA_SEC_OVERRUN;
    }
    if (len)
    {
        mbedtls_aes_crypt_ctr(&s->aes, len, &s->nc_off,
                              s->ctr, s->stream, buf, buf);
        crypto_hash_sha256_update(&s->sha, buf, len);
    }
    s->fed += len;
    return OTA_SEC_OK;
}

int ota_sec_finish(ota_sec_t *s)
{
    uint8_t sha[crypto_hash_sha256_BYTES];

    if (!s->active)
        return OTA_SEC_OVERRUN;
    s->active = false;
    mbedtls_aes_free(&s->aes);
    if (s->fed != s->plain_len)
        return OTA_SEC_SHORT;
    crypto_hash_sha256_final(&s->sha, sha);
    if (memcmp(sha, s->hdr0 + 32, 32) != 0)
        return OTA_SEC_HASH;
    if (crypto_sign_verify_detached(s->sig, s->hdr0, 64, kEdPub) != 0)
        return OTA_SEC_SIGN;
    return OTA_SEC_OK;
}

const char *ota_sec_str(int rc)
{
    switch (rc)
    {
    case OTA_SEC_OK:
        return "ok";
    case OTA_SEC_MAGIC:
        return "не .skpak пакет";
    case OTA_SEC_TYPE:
        return "чужой тип пакета";
    case OTA_SEC_LEN:
    case OTA_SEC_OVERRUN:
        return "размер пакета";
    case OTA_SEC_SHORT:
        return "пакет неполный";
    case OTA_SEC_HASH:
        return "файл повреждён (хеш)";
    case OTA_SEC_SIGN:
        return "ПОДПИСЬ НЕВЕРНА";
    case OTA_SEC_NOTIMG:
        return "содержимое не прошивка";
    default:
        return "ошибка пакета";
    }
}
