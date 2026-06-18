#include "protocol.h"
#include <string.h>

/*
 * === HEADER STRUCTURE (96 Bytes Total) ===
 * WORD 0 (Bytes 0-31): Ethernet (14B) + Custom Padded (18B)
 * WORD 1 (Bytes 32-63): RF Header Word 0
 * - 0-3:   Packet Count (4B)
 * - 4-7:   Slot Count (4B)
 * - 8:     Trigger CH3 (1B)
 * - 9:     Trigger CH2 (1B)
 * - 10:    Trigger CH1 (1B)
 * - 11:    Trigger CH0 (1B)
 * - 12:    Opcode (1B)
 * - 13-14: Bitfields (2B) [RF Group, LID, Dir, Type]
 * - 15-27: Dest Addr (13B)
 * - 28:    Source Addr (1B)
 * - 29-31: PREAMBLE_SFD (3B) -> 0xAAAAAB
 * WORD 2 (Bytes 64-95): RF Header Word 1
 * - 0-7:   TIMESTAMP (8B)
 */

void protocol_init_template(uint8_t *t, const struct rte_ether_addr *dest_mac) {
    memset(t, 0, PACKET_HEADER_LEN);

    // --- WORD 0: Ethernet ---
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)t;
    rte_ether_addr_copy(dest_mac, &eth->dst_addr);
    struct rte_ether_addr src_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_ether_addr_copy(&src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(0x88B5);

    // --- WORD 1: RF Header Word 0 ---
    uint8_t *w0 = t + 32;

    w0[12] = 0x00; // Opcode

    // Bitfields
    uint16_t rf_group_id = 0; 
    uint8_t  lid         = 0; 
    uint8_t  direction   = 1; 
    uint8_t  packet_type = 0; 
    uint16_t bitfields = (rf_group_id << 4) | (lid << 3) | (direction << 2) | (packet_type & 0x3);
    *(uint16_t*)(w0 + 13) = bitfields;

    // Byte 28 (Source Address) must be 0x00 so that when read as a 32-bit int with 
    // the preamble bytes below, it perfectly matches the AVX2 0xAAAAAB00 mask.
    w0[28] = 0x00; 

    // AVX2 Preamble match: 0xABAAAA (Little Endian -> 0xAAAAAB00)
    w0[29] = 0xAB;
    w0[30] = 0xAA;
    w0[31] = 0xAA;
}

void protocol_update_header(uint8_t *hdr, uint64_t pkt_cnt, int trig_ch, int trig_offset) {
    uint8_t *w0 = hdr + 32;
    uint8_t *w1 = hdr + 64;

    *(uint32_t*)(w0 + 0) = (uint32_t)pkt_cnt;
    *(uint32_t*)(w0 + 4) = 0; // slot_cnt
    
    // Provide a basic epoch timestamp in W1
    *(uint64_t*)(w1 + 0) = pkt_cnt; 

    // Clear all triggers by default
    w0[8] = 0x00; w0[9] = 0x00; w0[10] = 0x00; w0[11] = 0x00;

    // Apply specific channel trigger
    if (trig_ch >= 0 && trig_ch <= 3) {
        // Pack: [7:1] Sample Offset, [0] Trigger Event
        uint8_t trig_val = ((trig_offset & 0x7F) << 1) | 0x01;
        
        // Match wire offsets: CH3=8, CH2=9, CH1=10, CH0=11
        w0[11 - trig_ch] = trig_val; 
    }
}