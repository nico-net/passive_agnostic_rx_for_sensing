# Capturing other UEs' packets to Wireshark

The passive receiver decodes other UEs' transport blocks and CRC-checks them; set
`ISAC_PASSIVE_PCAP=<file>` and every CRC-verified transport block is also written to a pcap.

## Capture

    sudo env ISAC_PASSIVE_PCAP=/tmp/passive.pcap ./nr-uesoftmodem \
      -O ../../../tests/passive_rx/ota/nrue.passive_rx.bladerf.full_dlul.conf \
      -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 --ue-rxgain 32 \
      --ue-fo-compensation --ue-rx-only --passive-rx \
      --time-sync-I 0.01 --ntn-initial-time-drift -4.21 --usrp-args 'libusb:'

The file is Ethernet/IPv4/UDP framing of MAC-NR, flushed per packet, so it is usable even if the
run is killed with Ctrl-C.

## Read

The MAC-NR-over-UDP dissector is a Wireshark heuristic that is OFF by default. Turn it on:

    # tshark
    tshark -r /tmp/passive.pcap --enable-heuristic mac_nr_udp

    # Wireshark GUI: Analyze > Enabled Protocols > search "mac_nr_udp" > tick it

Then frames dissect as MAC-NR DL-SCH: per-UE by RNTI, MAC subheaders, LCIDs and SDU lengths.

    # packets per UE
    tshark -r /tmp/passive.pcap --enable-heuristic mac_nr_udp -T fields -e mac-nr.rnti | sort | uniq -c

Each pcap frame carries one PDSCH transport block. RNTI is in the mac-nr framing; SI-RNTI (SIB1)
is tagged as SI, everything else as C-RNTI keyed by RNTI. Only DL (PDSCH) is captured today; the
PUSCH decode does not yet reach CRC, so there is nothing to write for UL.
