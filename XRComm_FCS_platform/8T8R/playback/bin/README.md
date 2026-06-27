```
# XRComm Playback v8T8R (v1.3.0)

**Module:** Traffic Generation / Playback  
**Mode:** Zero-Copy NVMe/File to Network Streaming  
**Target Performance:** 100 Gbps (Line Rate) with Precision Timing  

---

## 1. Overview

**XRComm Universal Playback v8T8R** is a high-performance engine designed to stream raw I/Q signal data from NVMe storage or binary files directly to a network interface. 

This version introduces **Precision Timing** using floating-point cycle tracking to prevent drift, a **Real-time Statistics Monitor**, and **Interactive Runtime Tuning** to adjust transmission timing on the fly.

### Key Capabilities
* **Dual Source Modes:** * **NVMe:** Direct SPDK integration for streaming large datasets from SSDs.
    * **File:** Preloads entire binary files into Hugepage RAM for zero-latency looping.
* **Precision Timing:** Uses a custom "Precise Wait" busy loop with sub-microsecond drift correction (Floating Point math for cycle targets).
* **Real-Time Stats:** Displays instantaneous (10ms resolution) and average (1s) throughput on the console.
* **CSV Logging:** Automatically saves high-resolution burst logs to `burst_log_YYYYMMDD_HHMMSS.csv`.
* **Interactive Tuning:** Adjust timing offsets dynamically while streaming without restarting.

---

## 2. Architecture & Performance

The application utilizes a split-core pipelined architecture to maximize throughput:

1.  **Reader Core:** Fetches data from NVMe or RAM-Cache and pushes to a lockless Ring Buffer (`rte_ring`).
2.  **TX Core:** Dequeues from the ring, applies protocol headers, and performs precision wait loops before `rte_eth_tx_burst`.
3.  **Stats/Main Thread:** Monitors counters, handles user input, and writes CSV logs.

### Tuning Parameters (`universal.h`)
* **Burst Size:** `32` (Optimized for smoother pacing at high rates).
* **Ring Size:** `131,072` descriptors.
* **Read Buffer:** `1.5 MB` Super-Chunks (NVMe mode).
* **Hugepages:** Requires 1GB+ Hugepages explicitly allocated on the specific NUMA socket.

---

## 3. Packet Specification

The tool generates **480-byte packets**.

**Total Packet Length:** 480 Bytes  
**Payload Length:** 384 Bytes (96 Samples $\times$ 4 Bytes)

| Segment | Offset (Bytes) | Length | Description | Details |
| :--- | :--- | :--- | :--- | :--- |
| **Ethernet Header** | 0 - 13 | 14 | L2 Header | **Type:** `0x88B5`<br>**Dest:** User Configurable |
| **Sequence ID** | 14 - 21 | 8 | XRComm Metadata | Packet Sequence # (Little Endian) |
| **Reserved** | 22 - 31 | 10 | Padding | Zeroed |
| **Aurora Header** | 32 - 63 | 32 | Protocol Header | **Structure (Little Endian):**<br>+0: Packet Count<br>+4: Slot Count<br>+8: Trigger (`0x01000000`)<br>+13: Bitfields (RF Grp, LID, Dir)<br>+29: Sync (`0xABAAAA`) |
| **Aurora Reserved** | 64 - 95 | 32 | Padding | Zeroed |
| **Payload** | 96 - 479 | 384 | I/Q Data | Raw Binary Data |

---

## 4. Usage Guide

Run the application with root privileges.

```bash
sudo ./xrcomm_universal_playback

```

### Configuration Inputs

Upon launch, the application prompts for:

1. **Core List:** Comma-separated list of 3 cores (e.g., `7,8,9`).
2. **Source Mode:** `0` for NVMe (SPDK), `1` for File Playback.
3. **Filename:** Path to the binary file (checks `../file_logs/` and local dir).
4. **Sample Rate:** Target rate in Hz (e.g., `800000000`).
5. **TX Port ID:** DPDK Port ID.
6. **Dest MAC:** Destination Ethernet Address.
7. **Loop Count:** `0` for infinite.

### Runtime Controls (Interactive)

While the streaming is running, the console accepts the following keys to adjust the **Tuning Offset** (TX Cycle Delay) or **Tuning Rate**:

| Key | Action | Description |
| --- | --- | --- |
| `+` | Offset +1 | Increase delay per tuning interval (slower) |
| `-` | Offset -1 | Decrease delay per tuning interval (faster) |
| `]` | Offset +10 | Coarse increment |
| `[` | Offset -10 | Coarse decrement |
| `#<num>` | Set Rate | Set Tuning Rate Hz (e.g., `#20000` + Enter) |

---

## 5. Output & Logging

### Console Display

Updates every 1 second:

```text
METRIC          | INSTANT (10ms)| AVERAGE (1s)    
----------------|---------------|-----------------
Read Input      | 61.43 Gbps    | 61.44 Gbps      
Wire Transmit   | 61.43 Gbps    | 61.43 Gbps      

```

### CSV Log

A file named `burst_log_YYYYMMDD_HHMMSS.csv` is generated containing high-resolution stats:

```csv
Time_Sec, Read_Gbps, TX_Gbps, Delta_Gbps
0.0100, 61.4321, 61.4320, 0.0001
...

```

```

```