#include "nr_passive_pcap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"

/* pcap link type: DLT_WIRESHARK_UPPER_PDU. The record payload starts with an EXP_PDU header that
 * names the dissector, so no "Decode As" is needed. Same construction as macpdu2wireshark.c. */
#define DLT_EN10MB            1
#define PCAP_MAGIC_NANOSECOND 0xa1b23c4dU
#define MAC_NR_UDP_PORT       9999
/// Magic the Wireshark MAC-NR UDP heuristic looks for; 6 bytes, no terminator.
#define MAC_NR_START_STRING "mac-nr"

/* mac-nr framed tags, TS-independent, defined by the Wireshark dissector. */
#define MAC_NR_PAYLOAD_TAG    0x01
#define MAC_NR_RNTI_TAG       0x02
#define MAC_NR_UEID_TAG       0x03
#define MAC_NR_HARQID         0x06
#define MAC_NR_FRAME_SLOT_TAG 0x07

#define NR_TDD_RADIO          2
#define NR_DIRECTION_UPLINK   0
#define NR_DIRECTION_DOWNLINK 1
#define NR_C_RNTI             3
#define NR_SI_RNTI            4

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_f;              // NULL until opened
static int g_state;            // 0 = undecided, 1 = open, -1 = disabled

/// Ethernet + IPv4 + UDP encapsulation. Addresses are loopback and checksums are zero: nothing
/// reads them, they exist only so the heuristic dissector is reached.
static int build_encap(unsigned char *out, int udp_payload_len)
{
  int n = 0;
  memset(out + n, 0, 12);                       // dst + src MAC
  n += 12;
  out[n++] = 0x08;                              // ethertype IPv4
  out[n++] = 0x00;
  const int ip_total = 20 + 8 + udp_payload_len;
  out[n++] = 0x45;                              // IPv4, IHL 5
  out[n++] = 0x00;                              // DSCP/ECN
  out[n++] = (unsigned char)((ip_total >> 8) & 0xFF);
  out[n++] = (unsigned char)(ip_total & 0xFF);
  out[n++] = 0x00; out[n++] = 0x00;             // identification
  out[n++] = 0x40; out[n++] = 0x00;             // flags: don't fragment
  out[n++] = 64;                                // TTL
  out[n++] = 17;                                // protocol UDP
  out[n++] = 0x00; out[n++] = 0x00;             // header checksum (unchecked)
  out[n++] = 127; out[n++] = 0; out[n++] = 0; out[n++] = 1;   // src 127.0.0.1
  out[n++] = 127; out[n++] = 0; out[n++] = 0; out[n++] = 1;   // dst 127.0.0.1
  const int udp_total = 8 + udp_payload_len;
  out[n++] = (unsigned char)((MAC_NR_UDP_PORT >> 8) & 0xFF);
  out[n++] = (unsigned char)(MAC_NR_UDP_PORT & 0xFF);
  out[n++] = (unsigned char)((MAC_NR_UDP_PORT >> 8) & 0xFF);
  out[n++] = (unsigned char)(MAC_NR_UDP_PORT & 0xFF);
  out[n++] = (unsigned char)((udp_total >> 8) & 0xFF);
  out[n++] = (unsigned char)(udp_total & 0xFF);
  out[n++] = 0x00; out[n++] = 0x00;             // UDP checksum: 0 = not computed
  return n;                                     // always 42
}

/// \returns true when a capture file is open and writable. Caller must hold g_lock.
static bool pcap_open_locked(void)
{
  if (g_state != 0)
    return g_state > 0;

  const char *path = getenv("ISAC_PASSIVE_PCAP");
  if (path == NULL || path[0] == '\0') {
    g_state = -1;
    return false;
  }

  g_f = fopen(path, "wb");
  if (g_f == NULL) {
    LOG_E(PHY, "SENSING: cannot open ISAC_PASSIVE_PCAP file %s, capture disabled\n", path);
    g_state = -1;
    return false;
  }

  const uint32_t magic = PCAP_MAGIC_NANOSECOND;
  const uint16_t major = 2, minor = 4;
  const uint32_t zero = 0, snaplen = 0xFFFFFFFFU, dlt = DLT_EN10MB;
  if (fwrite(&magic, 4, 1, g_f) != 1 || fwrite(&major, 2, 1, g_f) != 1 || fwrite(&minor, 2, 1, g_f) != 1
      || fwrite(&zero, 4, 1, g_f) != 1 || fwrite(&zero, 4, 1, g_f) != 1 || fwrite(&snaplen, 4, 1, g_f) != 1
      || fwrite(&dlt, 4, 1, g_f) != 1) {
    LOG_E(PHY, "SENSING: cannot write pcap header to %s, capture disabled\n", path);
    fclose(g_f);
    g_f = NULL;
    g_state = -1;
    return false;
  }

  LOG_A(PHY, "SENSING: writing CRC-verified passive transport blocks to %s (open with Wireshark)\n", path);
  g_state = 1;
  return true;
}

void nr_passive_pcap_write(bool uplink,
                           uint16_t rnti,
                           bool si_rnti,
                           int frame,
                           int slot,
                           int harq_pid,
                           const uint8_t *tb,
                           int tb_bits)
{
  if (tb == NULL || tb_bits <= 0)
    return;

  /* The transport block carries a 16- or 24-bit CRC that the decode already verified. Wireshark's
   * MAC dissector expects the MAC PDU alone, so drop it: >3824 bits uses CRC24A, else CRC16. */
  const int crc_bits = (tb_bits > 3824) ? 24 : 16;
  const int mac_bits = tb_bits - crc_bits;
  if (mac_bits <= 0)
    return;
  const int mac_bytes = mac_bits / 8;
  if (mac_bytes <= 0)
    return;

  unsigned char hdr[48];
  int n = 0;
  memcpy(hdr + n, MAC_NR_START_STRING, 6);      // heuristic magic, no terminator
  n += 6;
  hdr[n++] = NR_TDD_RADIO;
  hdr[n++] = uplink ? NR_DIRECTION_UPLINK : NR_DIRECTION_DOWNLINK;
  hdr[n++] = si_rnti ? NR_SI_RNTI : NR_C_RNTI;
  hdr[n++] = MAC_NR_RNTI_TAG;
  hdr[n++] = (unsigned char)((rnti >> 8) & 0xFF);
  hdr[n++] = (unsigned char)(rnti & 0xFF);
  if (!si_rnti) {
    /* Wireshark keys its per-UE reassembly on the UE id; the RNTI is the only stable identity a
     * passive receiver has, so use it directly rather than inventing a mapping. */
    hdr[n++] = MAC_NR_UEID_TAG;
    hdr[n++] = (unsigned char)((rnti >> 8) & 0xFF);
    hdr[n++] = (unsigned char)(rnti & 0xFF);
  }
  hdr[n++] = MAC_NR_FRAME_SLOT_TAG;
  hdr[n++] = (unsigned char)((frame >> 8) & 0xFF);
  hdr[n++] = (unsigned char)(frame & 0xFF);
  hdr[n++] = (unsigned char)((slot >> 8) & 0xFF);
  hdr[n++] = (unsigned char)(slot & 0xFF);
  hdr[n++] = MAC_NR_HARQID;
  hdr[n++] = (unsigned char)(harq_pid & 0xFF);
  hdr[n++] = MAC_NR_PAYLOAD_TAG;

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);

  pthread_mutex_lock(&g_lock);
  if (pcap_open_locked()) {
    unsigned char encap[42];
    const int encap_len = build_encap(encap, n + mac_bytes);
    const uint32_t sec = (uint32_t)now.tv_sec;
    const uint32_t nsec = (uint32_t)now.tv_nsec;
    const uint32_t len = (uint32_t)(encap_len + n + mac_bytes);
    if (fwrite(&sec, 4, 1, g_f) != 1 || fwrite(&nsec, 4, 1, g_f) != 1 || fwrite(&len, 4, 1, g_f) != 1
        || fwrite(&len, 4, 1, g_f) != 1 || fwrite(encap, encap_len, 1, g_f) != 1
        || fwrite(hdr, n, 1, g_f) != 1 || fwrite(tb, mac_bytes, 1, g_f) != 1) {
      LOG_E(PHY, "SENSING: pcap write failed, capture disabled\n");
      fclose(g_f);
      g_f = NULL;
      g_state = -1;
    } else {
      /* Flushed per record: a passive capture is usually ended with a signal, and an unflushed
       * tail is the difference between a usable file and a truncated one. */
      fflush(g_f);
    }
  }
  pthread_mutex_unlock(&g_lock);
}
