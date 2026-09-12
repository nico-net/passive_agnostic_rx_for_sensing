#!/usr/bin/env python3
"""Fail-closed integration check for broadcast acquisition from raw-IQ replay.
This checks acquisition, not generalized DCI interpretation or all acceptance gates.
"""
import argparse
import json
from pathlib import Path
import re
from types import SimpleNamespace
from run_raw_replay import standard_tables
from offline_cell_config import geometry


def queue_counts(log, channel):
    matches = re.findall(rf'{channel}Q queued=(\d+) decoded=(\d+) crc_ok=(\d+)', log)
    if not matches:
        return None
    queued, decoded, passed = map(int, matches[-1])
    if not 0 <= passed <= decoded <= queued:
        raise ValueError(f'{channel}: internally inconsistent queue counters')
    return dict(queued=queued, decoded=decoded, crc_passes=passed,
                pending_at_last_summary=queued-decoded,
                crc_pass_rate=passed/decoded if decoded else None,
                scope='last logged cumulative summary, not an exhaustive capture message census')


def validate_acquisition(root):
    result = json.loads((root / 'result.json').read_text())
    invocation = json.loads((root / 'invocation.json').read_text())
    if result['status'] not in ('VALID_TRANSPORT_REPLAY', 'VALID_FAULT_INJECTION') or result['faults']:
        raise ValueError('VOID transport replay')
    if invocation['cell_configuration_injected'] or invocation['hardware_access'] != 'FORBIDDEN':
        raise ValueError('not an isolated configuration-free cell acquisition')
    ssb = result['received_broadcasts']['ssb']
    sib = result['received_broadcasts']['sib1']
    if len(ssb) < 2 or not sib:
        raise ValueError('missing repeated PBCH evidence or SIB1')
    if len({(x['pci'], x['ss_ref_hz'], x['ssb_mu']) for x in ssb}) != 1:
        raise ValueError('ambiguous cell identity in this acquisition check')
    rows, bandwidths = standard_tables()
    facts = [geometry(ssb[-1], x, SimpleNamespace(max_bandwidth_mhz=100), bandwidths, rows) for x in sib]
    if any(f != facts[0] for f in facts):
        raise ValueError('broadcast configuration changed; needs configuration-change validation')
    log = (root / 'receiver.log').read_text(errors='replace')
    return dict(status='PASS_RAW_BROADCAST_ACQUISITION', capture=invocation['capture'],
                replay=str(root), received_pbch_observations=len(ssb), received_sib1=len(sib),
                broadcast_geometry=facts[0], sample_shift=int(invocation['env']['ISAC_RAW_IQ_SHIFT']),
                dl=queue_counts(log, 'PDSCH'), ul=queue_counts(log, 'PUSCH'),
                dci_interpretation_confidence='NOT_VALIDATED_BY_THIS_CHECK',
                full_acceptance_gates='NOT_COMPLETE')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('replay', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(validate_acquisition(args.replay), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'FAIL_RAW_ACQUISITION: {error}\n')


if __name__ == '__main__':
    main()
