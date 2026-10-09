#include <stdio.h>
#include <string.h>
#include "protocol.h"
static ProtoCtx ctx;
int main(void) {
    char l1[] = "m,612.646610,36758.797000,111396.000000,0.000000,111396.000000";
    char l2[] = "1.000000,100923,300.923030,0C,C8,00,1";
    proto_init(&ctx);
    proto_values_restart(&ctx);
    proto_values_line(&ctx, l1);
    proto_values_line(&ctx, l2);
    printf("ready=%d freq=%.3f rateMLPM=%.3f totalml_plus=%.1f gtotalml=%.1f kf=%.3f pulses=%lu batch=%.3f st=%02X sp=%02X isr=%02X cfg=%u\n",
        (int)proto_values_ready(&ctx), (double)ctx.vals.frequency, (double)ctx.vals.rateMLPM,
        (double)ctx.vals.totalml_plus, (double)ctx.vals.gtotalml, (double)ctx.vals.kf_value,
        (unsigned long)ctx.vals.pulses, (double)ctx.vals.batch,
        ctx.vals.status, ctx.vals.setpoint, ctx.vals.isr, (unsigned)ctx.vals.cfg_rev);
    return 0;
}
