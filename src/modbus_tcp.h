#ifndef GW_MODBUS_TCP_H
#define GW_MODBUS_TCP_H

/* Modbus TCP slave (port 502) exposing the gateway status (input
 * registers) and the SKE-02 parameters (holding registers). */

void mbSetup(void);
void mbLoop(void);
bool mbConsumeRestartRequest(void); /* CONTROL=3: gateway restart */

#endif /* GW_MODBUS_TCP_H */
