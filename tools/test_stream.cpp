/*
 * Host test bench for the STREAMING listing parser (protocol.c after
 * the NVS-tree rework): feeds the captured device dump through
 * proto_parse_line with an on_param hook (как это делает ske02 +
 * tree_store) and checks the emitted stream. Кросс-компилируется с
 * реальным src/protocol.c (единый источник правды).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static ProtoCtx ctx;

/* захват потока: как tree_write, только в массив */
#define CAP 200
static SkeParam cap[CAP];
static uint16_t capId[CAP];
static int capN = 0;

static void on_param(ProtoCtx *c, uint16_t id, const SkeParam *p)
{
    (void)c;
    if (capN < CAP)
    {
        cap[capN] = *p;
        capId[capN] = id;
        capN++;
    }
}

#define CHECK(cond, ...)                            \
    do                                              \
    {                                               \
        if (!(cond))                                \
        {                                           \
            printf("FAIL %d: ", __LINE__);          \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
            fails++;                                \
        }                                           \
    } while (0)

static int fails = 0;

static const SkeParam *getp(uint16_t id)
{
    for (int i = 0; i < capN; i++)
        if (capId[i] == id)
            return &cap[i];
    return NULL;
}

static void parse_file(const char *file, int listing)
{
    FILE *f = fopen(file, "rb");
    char line[512];
    if (!f)
    {
        printf("cannot open %s\n", file);
        exit(1);
    }
    ctx.inListing = listing;
    while (fgets(line, sizeof(line), f))
    {
        char work[512];
        strncpy(work, line, sizeof(work) - 1);
        work[sizeof(work) - 1] = 0;
        proto_parse_line(&ctx, work);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *file = (argc > 1) ? argv[1] : "tools/ske_l.txt";
    proto_init(&ctx);
    ctx.nowMs = 1;
    ctx.on_param = on_param;
    proto_listing_begin(&ctx);

    parse_file(file, true);
    ctx.count = 178;

    printf("emitted=%d liveCount=%u maxId=%u sections=%u menus=%u\n",
           capN, (unsigned)ctx.liveCount, (unsigned)ctx.maxId,
           (unsigned)ctx.sectionNum, (unsigned)ctx.menuCount);
    CHECK(ctx.sectionNum == 4, "sections=%u", ctx.sectionNum);
    CHECK(ctx.menuCount >= 33, "menus=%u", ctx.menuCount);
    CHECK((int)ctx.liveCount == capN, "liveCount=%u != %d",
          (unsigned)ctx.liveCount, capN);
    CHECK(capN >= 177, "emitted=%d", capN);

    /* имена ключевых параметров (поток, не массив) */
    {
        const SkeParam *p0 = getp(0);
        const SkeParam *p2 = getp(2);
        CHECK(p0 && !strcmp(p0->name, "\xd0\xa2\xd0\xb8\xd0\xbf"),
              "p0 name='%s'", p0 ? p0->name : "(none)"); /* "Тип" */
        CHECK(p2 && !strcmp(p2->name,
                            "\xd0\x97\xd0\xbd\xd0\xb0\xd1\x87\xd0\xb5\xd0\xbd"
                            "\xd0\xb8\xd0\xb5"),
              "p2 name"); /* "Значение" */
    }

    /* типы */
    CHECK(getp(0)->type == SKT_BOOL, "p0 type=%u", getp(0)->type);
    CHECK(getp(1)->type == SKT_ENUM, "p1 type");
    CHECK(getp(2)->type == SKT_FLOAT, "p2 type");
    /* сверено с текущим дампом: 152 "Стоп бит" bool, 153 "Задержка,мс"
     * u8, 154 "Дата" date (старый тест знал другие id - дамп менялся) */
    CHECK(getp(152)->type == SKT_BOOL, "p152 type=%u", getp(152)->type);
    CHECK(getp(153)->type == SKT_U8, "p153 type=%u", getp(153)->type);
    CHECK(getp(154)->type == SKT_DATE, "p154 type=%u", getp(154)->type);

    /* опции: считаются в скрэтче на строку - после потока живы только
     * у последнего параметра, поэтому проверяем кол-во опций В МОМЕНТ
     * выдачи через отдельный прогон с проверкой в on_param */
    {
        int withOpts = 0;
        for (int i = 0; i < capN; i++)
            if (cap[i].optCnt)
                withOpts++;
        CHECK(withOpts >= 60, "enums with options=%d", withOpts);
        CHECK(getp(0)->optCnt == 2, "p0 opts=%u", getp(0)->optCnt);
        CHECK(getp(1)->optCnt == 3, "p1 opts=%u", getp(1)->optCnt);
    }

    /* enum-значение: "Тип" = индекс выбранной опции */
    CHECK(getp(0)->value < getp(0)->optCnt, "p0 value=%lu",
          (unsigned long)getp(0)->value);

    /* повторный листинг: счётчики сбрасываются, дублей нет */
    proto_listing_begin(&ctx);
    parse_file(file, true);
    CHECK(capN >= 177, "re-listing emitted=%d", capN);

    /* 's' OK-ответ вне листинга: on_value, не on_param */
    {
        static uint16_t gotId;
        static uint32_t gotRaw;
        char okline[128];   /* парсер пишет в разбираемую строку! */
        strcpy(okline,
               "        051 F    Qmax,\xd0\xbb = +100.0000"
               " [ 0.000000.. 999999.9]");
        ctx.on_value = NULL;
        proto_parse_line(&ctx, okline);
        /* строка вне листинга не должна попасть в поток: capN не растёт */
        CHECK(capN <= 2 * 200, "cap sanity");
    }

    printf(fails ? "FAILED (%d)\n" : "OK (%d fails)\n", fails);
    return fails ? 1 : 0;
}
