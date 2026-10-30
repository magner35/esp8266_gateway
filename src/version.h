#ifndef GW_VERSION_H
#define GW_VERSION_H

/* Версия прошивки шлюза — единообразно с прибором (FWVERSION в board.h).
 * Обновлять при релизе. Используется в versions.txt (сервер обновлений)
 * и в консоли ESP (команда INFO). Штамп даты сборки остаётся в
 * g_gw_version (main.c) для отладки. */
#define GW_FWVERSION "3425"

#endif /* GW_VERSION_H */
