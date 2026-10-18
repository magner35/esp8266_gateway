#ifndef GW_MB_PASS_H
#define GW_MB_PASS_H

/* Прозрачный Modbus TCP -> RTU мост: порт 502, до 2 клиентов.
 * MBAP-обёртка снимается, кадр RTU (адрес+PDU, CRC дописывает прибор)
 * уходит консольной командой MB в движок прибора, ответ MBR
 * заворачивается обратно в MBAP с тем же transaction id.
 * Регистровая карта - самого прибора (modbus.h прошивки
 * Totalizer_FreeRTOS). Служебная карта шлюза - :503. */

void mb_pass_start(void);

#endif /* GW_MB_PASS_H */
