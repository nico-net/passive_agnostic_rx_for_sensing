"""Standards/config helpers extracted from the existing acquisition orchestrator.
No subprocess, network, device-discovery or live-radio ownership code is imported.
"""
import bisect
import json
from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[3]
BW_MHZ = (5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100)


class Unsupported(RuntimeError):
    pass


def standard_tables():
    # Use the repository's standards tables, never a gNB config or capture log.
    source = (REPO / 'common/utils/nr/nr_common.c').read_text()
    raster = re.search(r'const sync_raster_t sync_raster\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
    bandwidth = re.search(r'static const int tables_5_3_2\[5\]\[NUM_BW_ENTRIES\]\s*=\s*\{(.*?)\n\};', source, re.S)
    if not raster or not bandwidth:
        raise RuntimeError('Standard table format changed; refusing to guess a scan plan')
    rows = [tuple(map(int, row)) for row in re.findall(
        r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', raster[1])]
    bw = [tuple(map(int, re.findall(r'-?\d+', row)))
          for row in re.findall(r'\{([^{}]+)\}', bandwidth[1])]
    if len(bw) != 5 or any(len(row) != len(BW_MHZ) for row in bw):
        raise RuntimeError('Unsupported bandwidth table format')
    return rows, bw


def ss_ref(gscn):
    if 2 <= gscn <= 7498:
        for m in (1, 3, 5):
            numerator = gscn - (m - 3) // 2
            if numerator % 3 == 0:
                return (numerator // 3) * 1200000 + m * 50000
    elif 7499 <= gscn <= 22255:
        return 3000000000 + (gscn - 7499) * 1440000
    raise Unsupported('Outside the supported FR1 synchronization raster')


def arfcn_hz(arfcn):
    if 0 <= arfcn <= 599999:
        return arfcn * 5000
    if 600000 <= arfcn <= 2016666:
        return 3000000000 + (arfcn - 600000) * 15000
    raise Unsupported('UL Point A is outside FR1')


def scan_plan(args, rows, bandwidths):
    plans = []
    for band, mu, first, step, last in rows:
        if band >= 254 or mu not in (0, 1):
            continue  # No FR2 or NTN claim.
        points = [ss_ref(g) for g in range(first, last + 1, step)
                  if args.start_hz <= ss_ref(g) <= args.stop_hz]
        if not points:
            continue
        prb = max(rb for rb, mhz in zip(bandwidths[mu], BW_MHZ)
                  if rb > 0 and mhz <= args.max_bandwidth_mhz)
        scs = 15000 << mu
        reach = prb * 6 * scs - 120 * scs - args.cfo_max_hz
        if reach <= 0:
            raise Unsupported('RF bandwidth too small for SSB and CFO search margin')
        i = 0
        while i < len(points):
            j = bisect.bisect_right(points, points[i] + reach) - 1
            center = points[j]
            plans.append(dict(band=band, mu=mu, prb=prb, center_hz=center))
            i = bisect.bisect_right(points, center + reach)
    return sorted(plans, key=lambda p: (p['center_hz'], p['mu'], p['band']))


def geometry(ssb, sib, args, bandwidths, rows):
    if ssb['pci'] != sib['pci']:
        raise Unsupported('SSB/SIB1 timing identity mismatch')
    mu = sib['dl_mu']
    if mu not in (0, 1) or ssb['ssb_mu'] != mu or sib['mib_mu'] != mu or sib['ul_mu'] != mu:
        raise Unsupported('Mixed SSB/DL/UL numerologies are not supported by this receiver')
    if not sib['tdd']:
        raise Unsupported('Simultaneous passive DL/UL currently requires a co-channel TDD carrier')
    if sib['dl_offset_rb'] != 0 or sib['ul_offset_rb'] != 0:
        raise Unsupported('Nonzero offsetToCarrier needs a PHY grid-contract extension')
    if sib['dl_prb'] != sib['ul_prb']:
        raise Unsupported('Different DL/UL grid widths need a separate capture path')
    if not (0 <= sib['k_ssb'] <= 23 and 0 <= sib['offset_to_point_a_rb'] <= 2199):
        raise Unsupported('Invalid or non-associated SSB/Point A')
    scs = 15000 << mu
    # TS 38.211 4.4.4.2 and 7.4.3.1: FR1 offsetToPointA and kSSB
    # are in 15 kHz reference units, NOT in the trial capture numerology.
    lower_ssb = ssb['ss_ref_hz'] - 120 * scs
    ssb_offset_hz = (12 * sib['offset_to_point_a_rb'] + sib['k_ssb']) * 15000
    point_a = lower_ssb - ssb_offset_hz
    if point_a <= 0 or ssb_offset_hz % scs:
        raise Unsupported('SSB is not integer-aligned to the supported FFT grid')
    if sib['ul_point_a_arfcn'] >= 0 and arfcn_hz(sib['ul_point_a_arfcn']) != point_a:
        raise Unsupported('UL and DL do not share Point A')
    prb = sib['dl_prb']
    if prb not in bandwidths[mu]:
        raise Unsupported('Broadcast bandwidth is not supported by the local PHY')
    mhz = BW_MHZ[bandwidths[mu].index(prb)]
    if mhz > args.max_bandwidth_mhz:
        raise Unsupported('Broadcast bandwidth exceeds configured RF capability')
    first_sc = ssb_offset_hz // scs
    if first_sc < 0 or first_sc + 240 > prb * 12:
        raise Unsupported('SSB is outside the discovered carrier grid')
    allowed = any(b == sib['band'] and m == mu and any(
        ss_ref(g) == ssb['ss_ref_hz'] for g in range(first, last + 1, step))
        for b, m, first, step, last in rows if b < 254 and m in (0, 1))
    if not allowed:
        raise Unsupported('Decoded band and measured SSB raster disagree')
    return dict(band=sib['band'], mu=mu, prb=prb,
                center_hz=point_a + prb * 6 * scs, point_a_hz=point_a,
                bandwidth_mhz=mhz, ssb_sc=first_sc, pci=ssb['pci'],
                ss_ref_hz=ssb['ss_ref_hz'])


def receiver_config(out, probe):
    if probe:
        return 'sensing = { enable = 0; };\n'
    # These are decoder compute/search limits and host affinities, not cell hints.
    return '''sensing = {
  enable = 1;
  pdcch_blind_monitor_autoconf = 1;
  pdcch_blind_monitor_autodiscover = 1;
  pdcch_blind_monitor_full_auto = 1;
  pdcch_blind_monitor_dci10 = "1";
  pdcch_blind_monitor_pdsch = "2:1:0:1:16:2:64:6";
  pdcch_blind_monitor_rnti_range = "1:65519";
  pdcch_blind_monitor_noise_gates = "0:2:500:0:3.0";
  pdcch_blind_monitor_scan_thread = "1:8:5";
  pdcch_blind_monitor_dci01 = "1:0";
  pdcch_blind_monitor_ul_pusch = "1:1:0";
  pdcch_blind_monitor_ul_uci = "256:11:0:32";
  pdcch_blind_monitor_ul_thread = "2:64:2";
  sources = "csi_rs,ssb,pdsch_dmrs_blind,pdsch_data,pusch_dmrs,pusch_data";
  cpi_slots = 32;
  interpolate = 1;
  capture = 1;
  sync_correction = 1;
  nominal_los_range_m = 0.0;
  report_path = %s;
  out_path = %s;
  report_endpoint = "tcp://127.0.0.1:5556";
  rx_id = "rx_x410";
};
''' % (json.dumps(str(out / 'reports.jsonl')), json.dumps(str(out / 'sensing')))


