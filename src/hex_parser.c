#include "hex_parser.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "avr_conf.h"
#include "hex_store.h"
#include "logger.h"

#define MAX_LINE 544     /* ":llaaaatt" + 255*2 данных + cc + CRLF + запас */

static const char *TAG = "hex_parser";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Разобрать hex-строку в байты; возврат - число байтов или -1 (не hex). */
static int parse_hex_bytes(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s[0] && s[1])
    {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0)
            break;
        if (n >= max)
            return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

/* Текущая страница образа: addr - её база, buf заполнен 0xFF. */
typedef struct
{
    uint32_t    base;               /* адрес текущей страницы или UINT32_MAX */
    bool        warned_back;        /* уже ругнулись на назад-идущие адреса */
    uint8_t     buf[AVR_PAGE_SIZE];
    hex_page_cb cb;
    void       *user;
    hex_stats_t *stats;
} image_t;

static void image_flush(image_t *img)
{
    if (img->base == UINT32_MAX)
        return;
    if (img->cb)
        img->cb(img->base, img->buf, img->user);
    img->stats->pages++;
    img->base = UINT32_MAX;
}

static void image_write(image_t *img, uint32_t addr, const uint8_t *data, int len)
{
    while (len > 0)
    {
        uint32_t page = addr & ~(uint32_t)(AVR_PAGE_SIZE - 1);
        if (page != img->base)
        {
            /* записи должны идти по возрастанию (так печатает avr-objcopy):
               возврат к уже отданной странице затёр бы её частичным образом.
               Проверка - только когда страница уже открыта (база отлична от
               начального UINT32_MAX), иначе ПЕРВАЯ запись считалась бы
               идущей назад и пропускалась */
            if (img->base != UINT32_MAX && page < img->base)
            {
                if (!img->warned_back)
                {
                    img->warned_back = true;
                    logW(TAG, "адрес 0x%X идёт назад (страница 0x%X отдана) - "
                              "дальнейшие назад-записи пропускаются молча",
                         addr, img->base);
                }
                return;
            }
            image_flush(img);
            img->base = page;
            memset(img->buf, 0xFF, AVR_PAGE_SIZE);
        }
        int off = addr - page;
        int chunk = AVR_PAGE_SIZE - off;
        if (chunk > len)
            chunk = len;
        memcpy(img->buf + off, data, chunk);

        if (addr < img->stats->min_addr) img->stats->min_addr = addr;
        if (addr + chunk - 1 > img->stats->max_addr) img->stats->max_addr = addr + chunk - 1;
        img->stats->total_bytes += chunk;

        addr += chunk;
        data += chunk;
        len -= chunk;
    }
}

/* построчное чтение из раздела avrhex вместо fgets */
static char *part_fgets(char *line, int cap)
{
    static char rbuf[128];
    static uint32_t rlen = 0, rpos = 0;
    int i = 0;
    int c;

    for (;;)
    {
        if (rpos >= rlen)
        {
            rlen = hex_store_read(rbuf, sizeof(rbuf));
            rpos = 0;
            if (!rlen)
                return i ? NULL : NULL;
        }
        c = rbuf[rpos++];
        if (c == 10 || i >= cap - 1) /* 
 */
        {
            line[i] = 0;
            return line;
        }
        if (c != 13) /* 
 пропускаем */
            line[i++] = (char)c;
    }
}

esp_err_t hex_file_parse(const char *path, hex_page_cb cb, void *user,
                         hex_stats_t *stats)
{
    hex_stats_t local;
    char line[MAX_LINE];
    int lineno = 0;
    uint32_t ext_base = 0;         /* база от записей типа 02/04 */
    int eof_seen = 0;
    esp_err_t rc = ESP_FAIL;

    (void)path;                    /* источник - раздел avrhex */
    if (!hex_store_open())
    {
        logE(TAG, "в avrhex нет загруженного образа");
        return ESP_FAIL;
    }

    hex_stats_t zero = {0};
    zero.min_addr = UINT32_MAX;
    if (!stats)
        stats = &local;
    *stats = zero;

    image_t img = {.base = UINT32_MAX, .warned_back = false, .cb = cb,
                   .user = user, .stats = stats};

    while (part_fgets(line, sizeof(line)))
    {
        uint8_t rec[260];
        int n, i, len, type;
        uint32_t addr;
        uint8_t sum = 0;

        lineno++;
        if (line[0] != ':')
        {
            if (line[strspn(line, " \r\n")] == '\0')   /* пустая строка */
                continue;
            logE(TAG, "строка %d: не начинается с ':'", lineno);
            goto out;
        }

        n = parse_hex_bytes(line + 1, rec, sizeof(rec));
        if (n < 5)
        {
            logE(TAG, "строка %d: короткая запись", lineno);
            goto out;
        }

        for (i = 0; i < n; i++)
            sum += rec[i];
        if (sum != 0)
        {
            logE(TAG, "строка %d: неверная контрольная сумма", lineno);
            goto out;
        }

        len  = rec[0];
        type = rec[3];
        addr = ((uint32_t)rec[1] << 8) | rec[2];
        if (5 + len != n)
        {
            logE(TAG, "строка %d: длина не сходится с данными", lineno);
            goto out;
        }

        switch (type)
        {
        case 0:                                     /* данные */
        {
            uint32_t abs_addr = ext_base + addr;
            if (abs_addr + len > AVR_FLASH_SIZE)
            {
                logE(TAG, "строка %d: адрес 0x%X за пределами flash МК",
                     lineno, abs_addr + len - 1);
                goto out;
            }
            image_write(&img, abs_addr, &rec[4], len);
            break;
        }
        case 1:                                     /* конец файла */
            eof_seen = 1;
            goto done;
        case 2:                                     /* extended segment */
            if (len == 2)
                ext_base = (((uint32_t)rec[4] << 8) | rec[5]) << 4;
            break;
        case 4:                                     /* extended linear */
            if (len == 2)
                ext_base = (((uint32_t)rec[4] << 8) | rec[5]) << 16;
            break;
        case 5:                                     /* start address - пропуск */
            break;
        default:
            logE(TAG, "строка %d: неизвестный тип записи %d", lineno, type);
            goto out;
        }

        if (eof_seen)
            break;
    }

    if (!eof_seen)
        logW(TAG, "файл без записи EOF - разбираю что есть");

done:
    image_flush(&img);
    rc = ESP_OK;
    logI(TAG, "строк %d, байт %u, адреса 0x%X..0x%X, страниц %d",
         lineno, (unsigned)stats->total_bytes, stats->min_addr,
         stats->max_addr, stats->pages);

out:
    return rc;
}
