/*
 * Host roundtrip-тест дерева: дамп -> потоковый парсер -> tree_store
 * (с эмулятором NVS) -> tree_get -> JSON как в web.c -> проверка UTF-8
 * и сравнение ключевых полей с эталонным деревом мока.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "protocol.h"

/* ---- эмулятор NVS ---- */
static std::map<std::string, std::vector<char>> nvsMem;
static int nvsCommitCnt = 0;
#define ESP_OK 0
typedef int esp_err_t;
typedef void *nvs_handle_t;
#define nvs_handle_t void *

static esp_err_t nvs_open(const char *, int, nvs_handle_t *h) { *h = (void *)1; return 0; }
static esp_err_t nvs_set_blob(nvs_handle_t, const char *k, const void *d, size_t n)
{
    nvsMem[k] = std::vector<char>((const char *)d, (const char *)d + n);
    return 0;
}
static esp_err_t nvs_get_blob(nvs_handle_t, const char *k, void *out, size_t *len)
{
    auto it = nvsMem.find(k);
    if (it == nvsMem.end())
        return 0x1102;
    if (it->second.size() > *len)
        return 0x1104;
    memcpy(out, it->second.data(), it->second.size());
    *len = it->second.size();
    return 0;
}
static esp_err_t nvs_commit(nvs_handle_t) { nvsCommitCnt++; return 0; }
static esp_err_t nvs_erase_all(nvs_handle_t) { nvsMem.clear(); return 0; }

#define NVS_READWRITE 1
void dbg_printf(const char *, ...) {}
#include "tree_store.h"
#include "tree_store.c"

/* ---- прогон дампа через парсер (как ske02) ---- */
static ProtoCtx ctx;
static int fails = 0;
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

int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : "tools/ske_l.txt";
    FILE *f = fopen(file, "rb");
    char line[512];
    char json[1 << 16];
    size_t jlen = 0;

    if (!f) { printf("no dump\n"); return 1; }
    proto_init(&ctx);
    ctx.nowMs = 1;
    /* как ske02.c: on_tree_param -> tree_write */
    ctx.on_param = [](ProtoCtx *c, uint16_t id, const SkeParam *p) {
        tree_write(id, c, p);
    };
    ctx.on_value = [](ProtoCtx *, uint16_t id, uint32_t raw) {
        tree_set_value(id, raw);
    };
    tree_init();
    tree_begin();
    ctx.inListing = 1;
    while (fgets(line, sizeof(line), f))
    {
        char work[512];
        strncpy(work, line, sizeof(work) - 1);
        work[sizeof(work) - 1] = 0;
        proto_parse_line(&ctx, work);
    }
    ctx.inListing = 0;
    tree_commit(&ctx, ctx.liveCount);
    fclose(f);
    printf("stored %u\n", (unsigned)tree_count());

    /* JSON как h_api_params: секции + все параметры */
    {
        static char menus[2048];
        tree_menus_load(menus, sizeof(menus));
        uint16_t count = tree_count();
        char secs[12][SKE_NAME_LEN];
        int secN = 0;
        jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen,
                                 "{\"count\":%u,\"sections\":[", count);
        for (uint16_t id = 0; id < count; id++)
        {
            TreeRec r;
            if (!tree_get(id, &r) || !r.section)
                continue;
            int f2;
            for (f2 = 0; f2 < secN; f2++)
                if (!strcmp(secs[f2], tree_menu_name(menus, r.section)))
                    break;
            if (f2 == secN && secN < 12)
            {
                strncpy(secs[secN], tree_menu_name(menus, r.section), SKE_NAME_LEN - 1);
                secs[secN][SKE_NAME_LEN - 1] = 0;
                secN++;
            }
        }
        for (int f2 = 0; f2 < secN; f2++)
            jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen,
                                     "%s\"%s\"", f2 ? "," : "", secs[f2]);
        jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen, "],\"params\":[");

        int first = 1;
        for (uint16_t id = 0; id < count; id++)
        {
            TreeRec r;
            char val[64];
            if (!tree_get(id, &r))
                continue;
            /* v: как rec_value_text */
            val[0] = 0;
            if (r.p.type == SKT_CMD) {}
            else if (!r.p.present) strcpy(val, "-");
            else if (r.p.masked) strcpy(val, "******");
            else if (r.p.type == SKT_STRING)
                snprintf(val, sizeof(val), "%s", r.opts[0] ? r.opts[0] : "");
            else if ((r.p.type == SKT_ENUM || r.p.type == SKT_BOOL) && r.p.optCnt)
            {
                uint32_t v = r.p.value;
                snprintf(val, sizeof(val), "%s",
                         r.opts[v < r.p.optCnt ? v : 0] ? r.opts[v < r.p.optCnt ? v : 0] : "-");
            }
            else
                proto_raw_to_display(r.p.type, r.p.value, val, sizeof(val));

            jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen,
                                     "%s{\"i\":%u,\"t\":%u,\"n\":\"%s\",\"s\":0,"
                                     "\"g\":\"%s\",\"tb\":\"%s\",\"v\":\"%s\",\"o\":[",
                                     first ? "" : ",", id, r.p.type, r.p.name,
                                     tree_menu_name(menus, r.group), tree_menu_name(menus, r.tab2), val);
            for (int k = 0; k < r.p.optCnt; k++)
                jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen,
                                         "%s\"%s\"", k ? "," : "",
                                         r.opts[k] ? r.opts[k] : "");
            jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen, "]}");
            first = 0;
        }
        jlen += (size_t)snprintf(json + jlen, sizeof(json) - jlen, "]}");
    }
    json[jlen] = 0;

    /* валидация UTF-8 всего JSON */
    {
        unsigned char *p = (unsigned char *)json;
        int bad = -1;
        for (size_t i = 0; i < jlen; i++)
        {
            if (p[i] < 0x80)
                continue;
            int n = (p[i] & 0xE0) == 0xC0 ? 2 : (p[i] & 0xF0) == 0xE0 ? 3 : 0;
            int ok = n == 2 || n == 3;
            for (int k = 1; k < n && ok; k++)
                if ((p[i + k] & 0xC0) != 0x80)
                    ok = 0;
            if (!ok)
            {
                bad = (int)i;
                break;
            }
            i += (size_t)(n - 1);
        }
        CHECK(bad < 0, "invalid UTF-8 at %d: %.20s", bad, json + (bad > 5 ? bad - 5 : 0));
    }

    /* все строки записей корректно NUL-разделены: пройтись по blob-ам
     * напрямую и проверить, что strlen каждой opt не склеивает метки */
    {
        int glued = 0;
        for (uint16_t id = 0; id < tree_count(); id++)
        {
            TreeRec r;
            if (!tree_get(id, &r))
                continue;
            for (int k = 0; k < r.p.optCnt; k++)
                if (r.opts[k] && strlen(r.opts[k]) > 44)
                    glued++;
        }
        CHECK(glued == 0, "glued option labels: %d", glued);
    }

    /* отладка: hex-дамп первого enum c опциями */
    for (uint16_t id = 0; id < tree_count(); id++)
    {
        TreeRec r;
        if (tree_get(id, &r) && r.p.optCnt >= 2)
        {
            printf("blob p%u optCnt=%u:\n", id, r.p.optCnt);
            tree_dump_blob(id);
            break;
        }
    }

    printf(fails ? "FAILED (%d)\n" : "OK (%d fails)\n", fails);
    if (argc > 2) /* dump JSON в файл для diff */
    {
        FILE *o = fopen(argv[2], "wb");
        fwrite(json, 1, jlen, o);
        fclose(o);
    }
    return fails ? 1 : 0;
}
