#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <rte_ether.h>

/* v6.10 Geometry: 32B Std Eth + 32B W0 + 32B W1 = 96B Header */
#define PACKET_HEADER_LEN  96
/* v6.10 Payload: 128 samples * 4 bytes = 512B */
#define PACKET_PAYLOAD_LEN 512
#define FULL_PACKET_LEN    608

#define SAMPLES_PER_PKT    128

void protocol_init_template(uint8_t *t, const struct rte_ether_addr *dest_mac);
void protocol_update_header(uint8_t *hdr, uint64_t pkt_cnt, int trig_ch, int trig_offset);

#endif /* PROTOCOL_H */