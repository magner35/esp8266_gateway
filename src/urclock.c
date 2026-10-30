#include "urclock.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "avr_conf.h"
#include "espcons.h"
#include "hex_parser.h"
#include "logger.h"
#include "uart_avr.h"

static const char *TAG = "urclock";

/* Команды urprotocol / STK500v1 (urboot/src/urboot.c) */
#define STK_GET_SYNC       0x30
#define STK_LEAVE_PROGMODE 0x51
#define STK_CHIP_ERASE     0x52
#define STK_LOAD_ADDRESS   0x55
#define STK_PROG_PAGE      0x64
#define STK_READ_PAGE      0x74
#define STK_READ_SIGN      0x75
#define UR_PROG_PAGE_FL    0x02
#define UR_READ_PAGE_FL    0x03
#define UR_EOP             0x20
#define STK_INSYNC_C       0x14
#define STK_OK_C           0x10

#define UB_N_MCU           2040

static urboot_info_t g_info;
/* живой прогресс для веб-статуса: фаза и страницы */
static volatile int sProgPhase;  /* 0-нет,1-стирание,2-запись,3-verify */
static volatile int sProgDone, sProgTotal;

const urboot_info_t *urclock_info(void)
{
    return &g_info;
}

void urclock_progress(int *phase, int *done, int *total)
{
    if (phase)
        *phase = sProgPhase;
    if (done)
        *done = sProgDone;
    if (total)
        *total = sProgTotal;
}

/* ---------- приём ответов ---------- */

/* Прочитать [INSYNC] payload [OK]; 0 = успех, -1 таймаут, -2 протокол. */
static int ur_reply(uint8_t *payload, int want, uint32_t timeout_ms)
{
    uint8_t s, e;

    if (avr_recv(&s, 1, timeout_ms) != 1)
        return -1;
    if (s != g_info.insync)
    {
        avr_drain();
        return -2;
    }
    if (want > 0 && avr_recv(payload, want, timeout_ms) != want)
        return -1;
    if (avr_recv(&e, 1, timeout_ms) != 1)
        return -1;
    if (e != g_info.ok)
    {
        avr_drain();
        return -2;
    }
    return 0;
}

/* ---------- команды ---------- */

static int ur_chip_erase(void)
{
    uint8_t f[2] = {STK_CHIP_ERASE, UR_EOP};
    avr_send(f, 2);
    /* INSYNC приходит сразу, OK - по завершении стирания всего flash */
    return ur_reply(NULL, 0, UR_ERASE_TIMEOUT_MS);
}

/* Запись одной страницы flash (addr выровнен на AVR_PAGE_SIZE). */
static int ur_page_write(uint32_t addr, const uint8_t *data)
{
    uint8_t eop = UR_EOP;

    if (!g_info.compat)
    {
        /* urprotocol: cmd, адрес БАЙТОВЫЙ LE (24 бита у m128), длина
         * одним байтом (0 = 256), данные, EOP. Кадр ОДНИМ куском:
         * каждый лишний uart_write_bytes + wait_tx_done добавлял
         * накладные на каждой странице */
        uint8_t frame[5 + AVR_PAGE_SIZE + 1];
        frame[0] = UR_PROG_PAGE_FL;
        frame[1] = (uint8_t)(addr & 0xFF);
        frame[2] = (uint8_t)((addr >> 8) & 0xFF);
        frame[3] = (uint8_t)((addr >> 16) & 0xFF);
        frame[4] = (uint8_t)(AVR_PAGE_SIZE & 0xFF);
        memcpy(frame + 5, data, AVR_PAGE_SIZE);
        frame[5 + AVR_PAGE_SIZE] = eop;
        if (avr_send(frame, sizeof(frame)) <= 0)
            return -1;
        return ur_reply(NULL, 0, UR_CMD_TIMEOUT_MS);
    }
    else
    {
        /* STK500v1 (как в оригинальном проекте): load address со WORD-
         * адресом LE, затем PROG_PAGE с двухбайтовой BE длиной и 'F' */
        uint16_t wa = addr / 2;
        uint8_t la[4] = {STK_LOAD_ADDRESS, (uint8_t)(wa & 0xFF),
                         (uint8_t)(wa >> 8), UR_EOP};
        uint8_t head[4] = {STK_PROG_PAGE, (uint8_t)(AVR_PAGE_SIZE >> 8),
                           (uint8_t)(AVR_PAGE_SIZE & 0xFF), 'F'};
        int rc;

        avr_send(la, sizeof(la));
        if ((rc = ur_reply(NULL, 0, UR_CMD_TIMEOUT_MS)) != 0)
            return rc;

        avr_send(head, sizeof(head));
        avr_send(data, AVR_PAGE_SIZE);
        avr_send(&eop, 1);
        return ur_reply(NULL, 0, UR_CMD_TIMEOUT_MS);
    }
}

/* Чтение len байт flash с addr (len <= AVR_PAGE_SIZE).
 * urprotocol-ветка читает КУСКАМИ по 64 Б: непрерывный приём 256 Б
 * на 1 Мбауд длится 2.6 мс - RX FIFO ESP всего 128 Б, и всплеск
 * прерываний WiFi успевал уронить хвост кадра (верификация рвалась
 * посередине). Кадр по 128 Б = 1.3 мс (ёмкость FIFO); повтор - кусок. */
static int ur_read_chunk(uint32_t addr, uint8_t *data, int len)
{
    uint8_t f[6] = {
        UR_READ_PAGE_FL,
        (uint8_t)(addr & 0xFF),
        (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)len,
        UR_EOP,
    };
    /* до 3 попыток на кадр: потерянный из-за WiFi-всплеска кусок
     * перезапрашивается - скорость не падает, платим только паузами
     * в редких неудачных кадрах */
    int attempt;
    for (attempt = 0; attempt < 3; attempt++)
    {
        avr_send(f, sizeof(f));
        if (ur_reply(data, len, UR_CMD_TIMEOUT_MS) == 0)
            return 0;
        /* дренаж обязателен: хвост побитого кадра не должен попасть
         * в ответ повторного запроса */
        avr_drain();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    return -1;
}

static int ur_page_read(uint32_t addr, uint8_t *data, int len)
{
    if (!g_info.compat)
    {
        int off;
        for (off = 0; off < len; off += 128)
        {
            int n = len - off > 128 ? 128 : len - off;
            if (ur_read_chunk(addr + (uint32_t)off, data + off, n) != 0)
                return -1;
        }
        return 0;
    }
    else
    {
        uint16_t wa = addr / 2;
        uint8_t la[4] = {STK_LOAD_ADDRESS, (uint8_t)(wa & 0xFF),
                         (uint8_t)(wa >> 8), UR_EOP};
        uint8_t f[5] = {STK_READ_PAGE, (uint8_t)(len >> 8),
                        (uint8_t)(len & 0xFF), 'F', UR_EOP};
        int rc;

        avr_send(la, sizeof(la));
        if ((rc = ur_reply(NULL, 0, UR_CMD_TIMEOUT_MS)) != 0)
            return rc;

        avr_send(f, sizeof(f));
        return ur_reply(data, len, UR_CMD_TIMEOUT_MS);
    }
}

static int ur_leave_progmode(void)
{
    uint8_t f[2] = {STK_LEAVE_PROGMODE, UR_EOP};
    avr_send(f, 2);
    /* QEXITEND-сборки здесь укорачивают WDT и стартуют приложение,
     * остальные выходят по своему WDT - линию сброса не трогаем */
    return ur_reply(NULL, 0, UR_CMD_TIMEOUT_MS);
}

/* ---------- синхронизация и опознание ---------- */

static void sync_frame(void)
{
    uint8_t f[2] = {STK_GET_SYNC, UR_EOP};
    avr_drain();
    avr_send(f, 2);
}

static int sync_once(uint8_t *ins, uint8_t *ok)
{
    uint8_t r[2];
    sync_frame();
    if (avr_recv(r, 2, UR_SYNC_REPLY_MS) == 2)
    {
        *ins = r[0];
        *ok = r[1];
        /* совет urprotocol.md: дренаж после успешного ответа, чтобы не
           принять мусор работающего приложения за ответ протокола */
        avr_drain();
        return 1;
    }
    avr_drain();
    return 0;
}

static void decode_insync_ok(uint8_t ins, uint8_t ok)
{
    /* Обратное кодировке urboot.c: TMP_INSYNC = INFO/255, TMP_OK1 = INFO%255,
     * TMP_OK = TMP_OK1 < TMP_INSYNC ? TMP_OK1 : TMP_OK1+1 */
    uint32_t info = (uint32_t)ins * 255 + (ok > ins ? ok - 1 : ok);
    g_info.mcuid = info % UB_N_MCU;
    g_info.features = info / UB_N_MCU;
}

/* 6-байтовая таблица на вершине flash: [blpages][vblvecnum][rjmp x2][cap][ver] */
static void read_bl_table(void)
{
    uint8_t t[6];
    int major, minor;

    if (!(g_info.features & UB_READ_FLASH))
        return;

    if (ur_page_read(AVR_FLASH_SIZE - 6, t, 6) != 0)
    {
        logW(TAG, "таблица urboot не читается - работаем без защиты BL-области");
        return;
    }

    g_info.bl_pages = t[0];
    g_info.vbl_vecnum = t[1];
    g_info.bl_cap = t[4];
    g_info.bl_version = t[5];
    major = g_info.bl_version >> 3;
    minor = g_info.bl_version & 7;

    /* ждём осмысленные версию и размер (urboot u6..u9; blpages по протоколу
     * 1..127, uint8_t большего и не примет) */
    if (major < 5 || major > 9 || g_info.bl_pages == 0)
    {
        logW(TAG, "таблица на вершине flash мусорна (ver %d.%d, pages %d) - игнорирую",
             major, minor, g_info.bl_pages);
        g_info.bl_pages = 0;
        g_info.vbl_vecnum = 0;
        g_info.bl_cap = 0;
        g_info.bl_version = 0;
        return;
    }

    g_info.table_valid = true;
    g_info.bl_start = AVR_FLASH_SIZE - (uint32_t)g_info.bl_pages * AVR_PAGE_SIZE;
}

esp_err_t urclock_connect(void)
{
    return urclock_connect_baud(AVR_UART_BAUD);
}

/* подключение на заданной скорости: autobaud urboot тренируется на
 * первый sync-кадр, поэтому запись и верификация могут идти на разных */
esp_err_t urclock_connect_baud(uint32_t baud)
{
    uint8_t ins = 0, ok = 0, ins2 = 0, ok2 = 0;
    int attempt;

    memset(&g_info, 0, sizeof(g_info));
    g_info.bl_start = AVR_FLASH_SIZE;    /* пока границ не знаем - весь flash */

    avr_uart_set_baud(baud);
    if (avr_uart_init() != ESP_OK)
        return ESP_FAIL;

    /* urboot входит в загрузчик только по внешнему сбросу (EXTRF): на каждый
     * сброс - своё окно ожидания синхронизации (WDTO загрузчика ~1 c) */
    for (attempt = 1; attempt <= UR_RESET_ATTEMPTS; attempt++)
    {
        uint32_t spent = 0;

        avr_reset_pulse();
        while (spent < UR_SYNC_WINDOW_MS)
        {
            if (sync_once(&ins, &ok) &&
                sync_once(&ins2, &ok2) && ins2 == ins && ok2 == ok)
                goto found;
            vTaskDelay(pdMS_TO_TICKS(60));
            spent += 60;
        }
        logW(TAG, "попытка %d/%d: загрузчик не ответил", attempt, UR_RESET_ATTEMPTS);
    }
    logE(TAG, "синхронизация не удалась (нет urboot на UART0?)");
    return ESP_FAIL;

found:
    g_info.insync = ins;
    g_info.ok = ok;

    if (ins == STK_INSYNC_C && ok == STK_OK_C)
    {
        /* классический STK500v1-загрузчик (optiboot и др.) */
        uint8_t sig[3] = {0};
        const uint8_t expect[3] = AVR_SIGNATURE;

        g_info.compat = true;
        g_info.table_valid = true;      /* границы неизвестны - защита вся flash */
        logI(TAG, "ответ 14/10: STK500v1-совместимый загрузчик, читаем сигнатуру");

        uint8_t f[2] = {STK_READ_SIGN, UR_EOP};
        avr_send(f, 2);
        if (ur_reply(sig, 3, UR_CMD_TIMEOUT_MS) != 0 ||
            memcmp(sig, expect, 3) != 0)
        {
            logE(TAG, "сигнатура %02X %02X %02X != %02X %02X %02X (" AVR_MCUID_NAME ")",
                 sig[0], sig[1], sig[2], expect[0], expect[1], expect[2]);
            return ESP_FAIL;
        }
        logI(TAG, "сигнатура ATmega128 подтверждена");
        return ESP_OK;
    }

    decode_insync_ok(ins, ok);

    if (ins == 255 && ok == 254)
    {
        /* коллизия кодировки со константами STK500 - urboot перенёс пару в
           255/254, mcuid узнать нельзя (urboot.c: "Cannot work with this...") */
        logW(TAG, "urboot с коллизией кодировки (255/254): mcuid не проверяется");
    }
    else if (g_info.mcuid != AVR_MCUID && g_info.mcuid != AVR_MCUID_ALT)
    {
        logE(TAG, "mcuid %u не " AVR_MCUID_NAME " (%d/%d): на линии другой МК",
             g_info.mcuid, AVR_MCUID, AVR_MCUID_ALT);
        return ESP_FAIL;
    }

    read_bl_table();

    {
        bool ce   = (g_info.features & UB_CHIP_ERASE) != 0;
        bool nor  = (g_info.features & UB_FLASH_LL_NOR) != 0;
        const char *vbl = "";
        if (g_info.table_valid)
        {
            switch (g_info.bl_cap & URT_VBLMASK)
            {
            case URT_VBLJUMP:          vbl = ", VBL (вектора патчим мы)"; break;
            case URT_VBLPATCH:         vbl = ", VBL (патчит сам)"; break;
            case URT_VBLMASK:          vbl = ", VBL (патчит сам, verify чистый)"; break;
            default:                   vbl = ", обычный (BOOTRST)"; break;
            }
        }
        logI(TAG, "urboot: mcuid=%u " AVR_MCUID_NAME " ce=%d nor=%d%s",
             g_info.mcuid, ce, nor, vbl);
        if (g_info.table_valid)
            logI(TAG, "urboot v%d.%d: %d страниц BL с 0x%X, cap=0x%02X",
                 g_info.bl_version >> 3, g_info.bl_version & 7,
                 g_info.bl_pages, g_info.bl_start, g_info.bl_cap);
    }

    return ESP_OK;
}

/* ---------- VBL: патч векторов хостом ---------- */

/* Для векторного загрузчика (URT_VBLJUMP): вектор сброса приложения
 * заменяем на jmp bl_start, исходный вектор переносим в слот vblvecnum.
 * Работает с копией страницы,озвращает её через page. */
static void patch_page0(uint8_t *page)
{
    uint8_t orig[AVR_VECTOR_SIZE];
    uint32_t target = g_info.bl_start / 2;      /* jmp использует WORD-адрес */
    uint32_t dst = (uint32_t)g_info.vbl_vecnum * AVR_VECTOR_SIZE;

    memcpy(orig, page, AVR_VECTOR_SIZE);

    /* jmp opcode в little-endian: 0C 94 <lo> <hi> (см. комментарий в
       urboot.c про _vbl_jmp_to_application[] = {0x0C, 0x94}) */
    page[0] = 0x0C;
    page[1] = 0x94;
    page[2] = (uint8_t)(target & 0xFF);
    page[3] = (uint8_t)((target >> 8) & 0xFF);

    if (dst + AVR_VECTOR_SIZE <= AVR_PAGE_SIZE)
        memcpy(page + dst, orig, AVR_VECTOR_SIZE);

    if (orig[0] != 0x0C || orig[1] != 0x94)
        logW(TAG, "вектор сброса образца не jmp (0x%02X%02X) - приложение для m128?",
             orig[1], orig[0]);

    logI(TAG, "VBL: сброс -> jmp 0x%X, исходный вектор -> слот %u (байт %u)",
         g_info.bl_start, g_info.vbl_vecnum, dst);
}

/* ---------- конвейер программирования ---------- */

typedef struct
{
    bool     verify;            /* false = запись, true = верификация */
    int      consec_dead;       /* подряд нечитаемые страницы (verify) */
    bool     chip_erased;       /* можно пропускать страницы из 0xFF */
    uint32_t write_limit;       /* всё, что >= - область загрузчика */
    bool     host_vbl_patch;    /* патчить страницу 0 хостом */
    bool     bl_patches_self;   /* загрузчик сам патчит вектора */
    int      written, skipped, mismatches, errors;
    uint32_t last_report;
    uint32_t report_every;
} prog_ctx_t;

static bool page_is_ff(const uint8_t *p)
{
    for (int i = 0; i < AVR_PAGE_SIZE; i++)
        if (p[i] != 0xFF)
            return false;
    return true;
}

static void page_cb(uint32_t addr, const uint8_t *page, void *user)
{
    prog_ctx_t *c = user;
    if (!c)
        return; /* проход-предподсчёт: колбэка нет */
    uint8_t buf[AVR_PAGE_SIZE];
    const uint8_t *p = page;

    /* Ctrl+C из консоли шлюза: рвём сессию между страницами;
     * hex остаётся в разделе, UPDATE_METER можно повторить */
    if (espcons_abort_pending())
    {
        logW(TAG, "aborted by console (Ctrl+C), page 0x%X", addr);
        c->errors++;
        return;
    }

    /* стр.0 под VBL-загрузчиком патчит хост (в обоих проходах одинаково) */
    if (addr == 0 && c->host_vbl_patch)
    {
        memcpy(buf, page, AVR_PAGE_SIZE);
        patch_page0(buf);
        p = buf;
    }

    if (addr >= c->write_limit)
    {
        logW(TAG, "страница 0x%X в области загрузчика - пропущена", addr);
        c->skipped++;
        return;
    }

    if (c->verify)
    {
        uint8_t rd[AVR_PAGE_SIZE];
        /* потеря связи: 5 подряд нечитаемых страниц = дальше не
         * мучаем таймаутами, остаток страниц считается ошибочным
         * (иначе полный проход по 368 стр × 2 таймаута = минуты) */
        if (c->consec_dead >= 5)
        {
            c->errors++;
            return;
        }
        if (ur_page_read(addr, rd, AVR_PAGE_SIZE) != 0)
        {
            /* ПЕРВЫЙ read после пачки записей иногда ловит мусор в
             * линии (хвост ответа последней страницы): повтор после
             * паузы и дренажа почти всегда проходит */
            avr_drain();
            vTaskDelay(pdMS_TO_TICKS(120));
            if (ur_page_read(addr, rd, AVR_PAGE_SIZE) != 0)
            {
                if (++c->consec_dead == 5)
                    logE(TAG, "verify: связь с загрузчиком потеряна - "
                              "остаток пропускается");
                c->errors++;
                return;
            }
            logW(TAG, "verify 0x%X: чтение прошло со второго раза", addr);
        }
        c->consec_dead = 0;

        /* Стр.0: во flash закономерно отличаются от образа области, которые
         * трогает сам загрузчик:
         *  - URT_VBLPATCH: он патчит вектор сброса и слот сам;
         *  - URT_PROTECTRST (u7.7+): он заменяет 2 младших байта вектора
         *    сброса на rjmp "назад" на себя - у m128 счётчик команд
         *    закольцован (64К слов), и rjmp с отрицательным смещением с
         *    адреса 0 достаёт верх flash, где сидит загрузчик. */
        int off = 0;
        if (addr == 0 && c->bl_patches_self)
        {
            int seg_end = AVR_VECTOR_SIZE;
            int vbl_off = (int)g_info.vbl_vecnum * AVR_VECTOR_SIZE;

            if (memcmp(rd, p, seg_end) != 0)
                logW(TAG, "verify 0x0: вектор сброса патчен загрузчиком");

            if (vbl_off > seg_end)
            {
                if (memcmp(rd + seg_end, p + seg_end, vbl_off - seg_end) != 0)
                {
                    logE(TAG, "verify 0x%X: не совпало (векторная таблица)", addr);
                    c->mismatches++;
                    return;
                }
                seg_end = vbl_off + AVR_VECTOR_SIZE;
            }
            off = seg_end;
        }
        else if (addr == 0 && g_info.table_valid &&
                 (g_info.bl_cap & URT_PROTECTRST))
        {
            /* ожидаемый rjmp: (0 + 1 + k) mod 65536 = bl_start/2 */
            uint16_t blword = (uint16_t)(g_info.bl_start / 2);
            uint16_t rjmp = 0xC000 | (uint16_t)((blword - 1) & 0xFFF);

            if (rd[0] == (uint8_t)(rjmp & 0xFF) &&
                rd[1] == (uint8_t)(rjmp >> 8))
            {
                logI(TAG, "verify 0x0: вектор сброса заменён загрузчиком на "
                          "rjmp 0x%04X на BL (PROTECTRESET) - ок", rjmp);
                off = 2;       /* байты 2..3 вектора остаются нашими */
            }
            /* иначе - rjmp нет, сравниваем всё, включая наш jmp */
        }

        if (memcmp(rd + off, p + off, AVR_PAGE_SIZE - off) != 0)
        {
            int i, first = -1, last = -1, cnt = 0;
            for (i = off; i < AVR_PAGE_SIZE; i++)
                if (rd[i] != p[i])
                {
                    if (first < 0)
                        first = i;
                    last = i;
                    cnt++;
                }
            logE(TAG, "verify 0x%X: не совпало, отличается байт: %d, смещения %d..%d",
                 addr, cnt, first, last);

            /* дамп 16 байт вокруг первого расхождения */
            int d = first & ~15;
            char a[64], b[64];
            int pa = 0, pb = 0;
            for (i = d; i < d + 16; i++)
            {
                pa += snprintf(a + pa, sizeof(a) - pa, "%02X ", p[i]);
                pb += snprintf(b + pb, sizeof(b) - pb, "%02X ", rd[i]);
            }
            logE(TAG, "verify 0x%X @%03d: образ  %s", addr, d, a);
            logE(TAG, "verify 0x%X @%03d: flash  %s", addr, d, b);

            if (addr == 0)
            {
                int so = (int)g_info.vbl_vecnum * AVR_VECTOR_SIZE;
                logE(TAG, "verify 0x0: слот vbl[%d]: образ %02X %02X %02X %02X, "
                          "flash %02X %02X %02X %02X",
                     g_info.vbl_vecnum,
                     p[so], p[so + 1], p[so + 2], p[so + 3],
                     rd[so], rd[so + 1], rd[so + 2], rd[so + 3]);
            }
            c->mismatches++;
        }
        return;
    }

    /* после chip erase пустые страницы писать незачем */
    if (c->chip_erased && page_is_ff(p))
    {
        c->skipped++;
        return;
    }

    /* запись с одним повтором; 5 подряд неудач = связь потеряна,
     * остаток пропускаем (не молотим таймаутами все страницы) */
    if (c->consec_dead >= 5)
    {
        c->errors++;
        return;
    }
    int wrc = ur_page_write(addr, p);
    if (wrc != 0)
    {
        avr_drain();
        vTaskDelay(pdMS_TO_TICKS(120));
        wrc = ur_page_write(addr, p);
        if (wrc != 0)
        {
            if (++c->consec_dead == 5)
                logE(TAG, "запись: связь с загрузчиком потеряна - "
                          "остаток пропускается");
            logE(TAG, "запись 0x%X не удалась (%s)", addr,
                 wrc == -1 ? "таймаут ответа" : "чужой ответ");
            c->errors++;
            return;
        }
    }
    c->consec_dead = 0;
    c->written++;
    sProgDone = c->written + c->skipped;
    if (++c->last_report >= c->report_every)
    {
        c->last_report = 0;
        logI(TAG, "%s: 0x%X...", c->verify ? "verify" : "write", addr);
    }
}

esp_err_t urclock_program_file(const char *hex_path, urclock_result_t *res)
{
    prog_ctx_t ctx;
    hex_stats_t st;
    TickType_t t0;
    bool chip_erased = false;

    memset(res, 0, sizeof(*res));
    t0 = xTaskGetTickCount();

    sProgTotal = 0;
    if (urclock_connect() != ESP_OK)
    {
        sProgPhase = 0;
        return ESP_FAIL;
    }
    res->compat = g_info.compat;

    if (g_info.compat)
        snprintf(res->bl_desc, sizeof(res->bl_desc), "STK500v1 bootloader");
    else
        snprintf(res->bl_desc, sizeof(res->bl_desc),
                 "urboot u%d.%d mcuid=%u",
                 g_info.bl_version >> 3, g_info.bl_version & 7, g_info.mcuid);

    /* стирание: в urprotocol - только если загрузчик умеет; классические
       загрузчики стирают страницы по ходу записи */
    if (!g_info.compat && (g_info.features & UB_CHIP_ERASE))
    {
        logI(TAG, "chip erase...");
        if (ur_chip_erase() != 0)
        {
            logE(TAG, "chip erase не удался");
            return ESP_FAIL;
        }
        chip_erased = true;
    }
    else if (!g_info.compat && (g_info.features & UB_FLASH_LL_NOR))
    {
        logW(TAG, "загрузчик без chip erase и без стирания страниц: старые "
                  "данные могут 'просвечивать'; соберите urboot с CHIP_ERASE");
    }

    /* предподсчёт страниц образа (чистый разбор раздела, без AVR):
     * даёт знаменатель для полоски прогресса */
    {
        hex_stats_t st0;
        sProgPhase = 1;
        sProgDone = 0;
        if (hex_file_parse(NULL, NULL, NULL, &st0) == ESP_OK)
            sProgTotal = st0.pages;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.chip_erased = chip_erased;
    ctx.write_limit = g_info.bl_start;
    ctx.report_every = 64;
    ctx.host_vbl_patch = !g_info.compat && g_info.table_valid &&
                         (g_info.bl_cap & URT_VBLMASK) == URT_VBLJUMP;
    ctx.bl_patches_self = !g_info.compat && g_info.table_valid &&
                          (g_info.bl_cap & URT_VBLMASK) == URT_VBLPATCH;

    logI(TAG, "запись: %s", hex_path ? hex_path : "avrhex");
    sProgPhase = 2;
    sProgDone = 0;
    if (hex_file_parse(hex_path, page_cb, &ctx, &st) != ESP_OK || ctx.errors)
    {
        logE(TAG, "запись прервана (страниц с ошибками: %d)", ctx.errors);
        return ESP_FAIL;
    }
    if (st.pages == 0 || st.total_bytes == 0)
    {
        /* ноль страниц после разбора = либо битый/пустой hex, либо баг
           парсера - "успешно прошито 0 страниц" здесь недопустимо */
        logE(TAG, "разбор не дал ни одной страницы (байт данных: %u) - "
                  "файл пуст или повреждён", (unsigned)st.total_bytes);
        return ESP_FAIL;
    }
    res->written = ctx.written;
    res->skipped = ctx.skipped;
    res->image_bytes = st.total_bytes;

    /* верификация - второй проход по тому же файлу, та же сессия */
    logI(TAG, "верификация...");
    sProgPhase = 3;
    sProgDone = 0;
    ctx.verify = true;
    ctx.written = ctx.skipped = ctx.last_report = 0;
    if (hex_file_parse(hex_path, page_cb, &ctx, NULL) != ESP_OK || ctx.errors)
    {
        logE(TAG, "верификация прервана");
        return ESP_FAIL;
    }
    res->verify_fail = ctx.mismatches;

    ur_leave_progmode();
    sProgPhase = 0;

    res->ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
    logI(TAG, "готово за %u.%01u c: записано %d, пропущено %d, verify ошибок %d",
         res->ms / 1000, (res->ms % 1000) / 100,
         res->written, res->skipped, res->verify_fail);

    return (ctx.mismatches == 0) ? ESP_OK : ESP_FAIL;
}
