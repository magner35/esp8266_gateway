/*
 * Host test bench: compiles the REAL src/protocol.c (single source of
 * truth) and feeds it the captured device listing + synthetic lines.
 * Compile: tools\run_parser_test.bat
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../src/protocol.h"
}

static ProtoCtx ctx;
static int fails = 0;

#define CHECK(cond, ...)                            \
    do                                              \
    {                                               \
        if (!(cond))                                \
        {                                           \
            fails++;                                \
            printf("FAIL: " __VA_ARGS__);           \
            printf("\n");                           \
        }                                           \
    } while (0)

static float asF(uint32_t raw)
{
    float f;
    memcpy(&f, &raw, 4);
    return f;
}

static void parse_file(const char *file, bool listing)
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
    const char *file = (argc > 1) ? argv[1] : "tools/ske_l.txt";
    char buf[64];
    proto_init(&ctx);
    ctx.nowMs = 1;

    parse_file(file, true);
    ctx.count = 178; /* set by the 'i' info command in the firmware */

    printf("menus=%u sections=%u poolUsed=%u\n",
           (unsigned)ctx.menuCount, (unsigned)ctx.sectionNum,
           (unsigned)ctx.optPoolUsed);
    CHECK(ctx.sectionNum == 4, "sections=%u", ctx.sectionNum);
    CHECK(ctx.menuCount >= 33, "menus=%u", ctx.menuCount);

    /* names */
    CHECK(!strcmp(ctx.params[0].name, "\xd0\xa2\xd0\xb8\xd0\xbf"), "p0 name='%s'",
          ctx.params[0].name); /* "Тип" */
    CHECK(!strcmp(ctx.params[2].name, "\xd0\x97\xd0\xbd\xd0\xb0\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5"),
          "p2 name"); /* "Значение" */
    {
        int empty = 0;
        for (int i = 0; i < 177; i++)
            if (!ctx.params[i].name[0])
                empty++;
        CHECK(empty == 0, "empty names=%d", empty);
    }

    /* full long submenu names: "направление" survives whole */
    {
        bool full = false;
        for (uint16_t i = 0; i < ctx.menuCount; i++)
            if (strstr(ctx.menus[i],
                       "\xd0\xbd\xd0\xb0\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbb"
                       "\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5"))
                full = true;
        CHECK(full, "full 'направление' submenu");
    }

    /* types */
    CHECK(ctx.params[0].type == SKT_BOOL, "p0 type=%u", ctx.params[0].type);
    CHECK(ctx.params[1].type == SKT_ENUM, "p1 type");
    CHECK(ctx.params[2].type == SKT_FLOAT, "p2 type");
    CHECK(ctx.params[152].type == SKT_DATE, "p152 type");
    CHECK(ctx.params[153].type == SKT_TIME, "p153 type");
    CHECK(ctx.params[154].type == SKT_I8, "p154 type");

    /* options */
    CHECK(ctx.params[0].optCnt == 2, "p0 opts=%u", ctx.params[0].optCnt);
    CHECK(ctx.params[1].optCnt == 3, "p1 opts=%u", ctx.params[1].optCnt);
    {
        const char *o = proto_opt_text(&ctx, &ctx.params[0], 0);
        CHECK(o && !strcmp(o, "K-\xd1\x84\xd0\xb0\xd0\xba\xd1\x82\xd0\xbe\xd1\x80"),
              "p0 opt0='%s'", o ? o : "(null)"); /* "K-фактор" */
        o = proto_opt_text(&ctx, &ctx.params[0], 1);
        CHECK(o && !strcmp(o, "\xd0\xa6\xd0\xb5\xd0\xbd\xd0\xb0 \xd0\xb8\xd0\xbc\xd0\xbf."),
              "p0 opt1"); /* "Цена имп." */
    }
    CHECK(ctx.optPoolUsed < SKE_OPT_POOL, "pool overflow");
    {
        int withOpts = 0;
        for (int i = 0; i < 177; i++)
            if (ctx.params[i].optCnt)
                withOpts++;
        CHECK(withOpts >= 60, "enums with options=%d", withOpts);
    }

    /* numeric ranges */
    CHECK(ctx.params[43].minv == 3 && ctx.params[43].maxv == 10, "p43 range");
    CHECK(asF(ctx.params[2].maxv) > 999999.0f && asF(ctx.params[2].maxv) < 1000000.0f,
          "p2 hi=%f", asF(ctx.params[2].maxv));
    CHECK((int32_t)ctx.params[154].minv == -60 && (int32_t)ctx.params[154].maxv == 60,
          "p154 range");

    /* values */
    CHECK(ctx.params[0].value == 1, "p0 value=%u", ctx.params[0].value);
    CHECK(asF(ctx.params[2].value) == 1.0f, "p2 value");
    CHECK(ctx.params[43].value == 6, "p43 value=%u", ctx.params[43].value);
    CHECK(ctx.params[152].value == 946684800UL, "p152 date=%u",
          ctx.params[152].value);
    CHECK(ctx.params[153].value == 3 * 3600UL, "p153 time=%u",
          ctx.params[153].value);

    /* read-only marker */
    CHECK(ctx.params[169].readOnly, "p169 ro"); /* Изм.CRC view */
    CHECK(!ctx.params[43].readOnly, "p43 not ro");

    /* display text */
    proto_value_text(&ctx, 0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "\xd0\xa6\xd0\xb5\xd0\xbd\xd0\xb0 \xd0\xb8\xd0\xbc\xd0\xbf."),
          "p0 display='%s'", buf); /* "Цена имп." */
    proto_value_text(&ctx, 153, buf, sizeof(buf));
    CHECK(!strcmp(buf, "03:00"), "p153 display='%s'", buf);

    /* command items and masked passwords (synthetic lines) */
    {
        char cmd1[] = "      177 CMD  C\xd0\xb1poc o\xd0\xb1\xd1\x8a\xd1\x91\xd0\xbc";
        char pw1[] = "      046 U32  \xd0\x9f" "apo\xd0\xbb\xd1\x8c = ****** [0..999999]";
        proto_parse_line(&ctx, cmd1);
        CHECK(ctx.params[177].type == SKT_CMD && ctx.params[177].present,
              "cmd type=%u present=%d", ctx.params[177].type,
              (int)ctx.params[177].present);
        proto_parse_line(&ctx, pw1);
        CHECK(ctx.params[46].masked && ctx.params[46].value == 0,
              "masked flag=%d v=%u", (int)ctx.params[46].masked,
              ctx.params[46].value);
    }

    /* "S" params: LCD-charset strings, always read only */
    {
        char s1[] = "      176 S  Seriiny \xe2\x84\x96 = 3425";
        char s2[] = "      176 S  Seriiny \xe2\x84\x96 = 99999";
        proto_parse_line(&ctx, s1);
        CHECK(ctx.params[176].type == SKT_STRING && ctx.params[176].present,
              "string type=%u present=%d", ctx.params[176].type,
              (int)ctx.params[176].present);
        CHECK(ctx.params[176].readOnly, "string not ro");
        proto_value_text(&ctx, 176, buf, sizeof(buf));
        CHECK(!strcmp(buf, "3425"), "string display='%s'", buf);
        proto_parse_line(&ctx, s2); /* refresh with a longer text */
        proto_value_text(&ctx, 176, buf, sizeof(buf));
        CHECK(!strcmp(buf, "99999"), "string refresh='%s'", buf);

        /* submenu-owner names carry the LCD arrow '~' + padding */
        char t1[] = "      179 U32  Homep~ = 0018 [0..9999]";
        char t2[] = "    181 CMD  Pocmotp          ~";
        proto_parse_line(&ctx, t1);
        CHECK(!strcmp(ctx.params[179].name, "Homep"), "tilde name='%s'",
              ctx.params[179].name);
        proto_parse_line(&ctx, t2);
        CHECK(!strcmp(ctx.params[181].name, "Pocmotp"), "tilde cmd='%s'",
              ctx.params[181].name);
    }

    /* console 's' value packing: TIME = HHMM, DATE = DDMMYY */
    proto_raw_to_cmd(SKT_TIME, 12UL * 3600 + 34 * 60, buf, sizeof(buf));
    CHECK(!strcmp(buf, "1234"), "time pack='%s'", buf);
    proto_raw_to_cmd(SKT_DATE, 946684800UL, buf, sizeof(buf));
    CHECK(!strcmp(buf, "010100"), "date pack='%s'", buf);
    proto_raw_to_cmd(SKT_DATE,
                     (uint32_t)proto_days_from_civil(2099, 12, 31) * 86400UL,
                     buf, sizeof(buf));
    CHECK(!strcmp(buf, "311299"), "date pack 2099='%s'", buf);

    /* UI input compose */
    {
        uint32_t raw = 0;
        char err[48];
        CHECK(proto_input_to_raw(&ctx, 153, "12:34", &raw, err, sizeof(err)),
              "time input: %s", err);
        CHECK(raw == 12UL * 3600 + 34 * 60, "time raw=%lu", (unsigned long)raw);
        CHECK(!proto_input_to_raw(&ctx, 153, "25:00", &raw, err, sizeof(err)),
              "25:00 accepted");
    }

    /* 'm' values frame */
    {
        char l0[] = "m,25.400,12.510,12.490,12500.000,12.500";
        char l1[] = "123.456,0.500,122.956,123.956,123456.200,500.000,9876.500,9876543.000";
        char l2[] = "1.000,4,123456,10.250,45,0A,81";
        proto_values_restart(&ctx);
        proto_values_line(&ctx, l0);
        proto_values_line(&ctx, l1);
        proto_values_line(&ctx, l2);
        CHECK(proto_values_ready(&ctx), "m frame not complete");
        CHECK(ctx.vals.rate > 12.49f && ctx.vals.rate < 12.51f, "m rate=%f",
              (double)ctx.vals.rate);
        CHECK(ctx.vals.pulses == 123456, "m pulses=%lu",
              (unsigned long)ctx.vals.pulses);
        CHECK(ctx.vals.status == 0x45 && ctx.vals.setpoint == 0x0A &&
                  ctx.vals.isr == 0x81,
              "m hex bits");
    }

    /* prompt matcher */
    {
        const char *stream = "junk\r\nSKE02> tail";
        bool hit = false;
        proto_prompt_reset(&ctx);
        for (const char *q = stream; *q; q++)
            if (proto_prompt_feed(&ctx, *q))
            {
                hit = true;
                break;
            }
        CHECK(hit, "prompt not detected");
    }

    /* poll regression: parse the whole listing a SECOND time - options
     * must survive intact (no pool duplicates) */
    {
        int wo = 0;
        uint16_t poolBefore = ctx.optPoolUsed;
        parse_file(file, true);
        CHECK(ctx.params[0].optCnt == 2 && ctx.params[0].value == 1,
              "2nd pass p0 opts=%u v=%u", ctx.params[0].optCnt,
              ctx.params[0].value);
        for (int i = 0; i < 177; i++)
            if (ctx.params[i].optCnt)
                wo++;
        CHECK(wo >= 60, "2nd pass enums with options=%d", wo);
        CHECK(ctx.optPoolUsed == poolBefore, "2nd pass pool grew: %u -> %u",
              (unsigned)poolBefore, (unsigned)ctx.optPoolUsed);
    }

    printf(fails ? "%d CHECK(s) FAILED\n" : "ALL OK (%d failures)\n", fails);
    return fails ? 1 : 0;
}
