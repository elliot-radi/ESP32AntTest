/* ESP32AntTest — Station main (skeleton)
 *
 * Build-verify skeleton for the shared-component wiring (step 1). Full
 * Station behaviour (beacon RX, logging, serial protocol, LittleFS,
 * protocol forward to Mobile) is step 2. For now it emits the boot banner
 * and the time_prompt event per docs/SERIAL_PROTOCOL.md and exercises the
 * shared protocol component to prove it links.
 */
#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "config.h"

void app_main(void)
{
    /* Boot banner (# channel) + time prompt ($ control event). */
    printf("# ESP32AntTest Station fw 0.3.0\n");
    printf("# MAC: <TBD>\n");
    printf("# Built: " __DATE__ "\n");
    printf("$ {\"evt\":\"time_prompt\"}\n");
    fflush(stdout);

    /* Exercise the shared component to prove it compiles + links. */
    ant_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic[0] = ANT_MAGIC_0;
    pkt.magic[1] = ANT_MAGIC_1;
    pkt.version  = ANT_PROTO_VER;
    pkt.type     = PKT_BEACON;
    pkt.tx_power = ANT_DEFAULT_TX_POWER;

    uint8_t buf[sizeof(ant_packet_t)];
    ant_packet_encode(&pkt, buf);

    ant_packet_t decoded;
    if (ant_packet_decode(buf, (int)sizeof(buf), &decoded) != 0) {
        printf("$ {\"evt\":\"error\",\"reason\":\"self-test decode failed\"}\n");
        return;
    }
    printf("# self-test OK: ant_packet_t=%u bytes, ANT_STATION_IP=%s\n",
           (unsigned)sizeof(ant_packet_t), ANT_STATION_IP);
    printf("# Station skeleton ready (Quick-Check beaconing not yet implemented)\n");
    fflush(stdout);
}
