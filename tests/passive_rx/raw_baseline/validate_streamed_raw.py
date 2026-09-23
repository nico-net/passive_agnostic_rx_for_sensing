#!/usr/bin/env python3
"""Validate streamed single-channel raw IQ, or delegate legacy four-channel captures."""
import argparse
import csv
import json
from pathlib import Path
import re
from validate_raw import validate as legacy_validate


def validate(root):
    root = Path(root)
    info = json.loads((root / 'capture_info.json').read_text())
    if info.get('recorder') != 'bounded_stream_v1':
        return legacy_validate(root)
    if info['status'] != 'COMPLETE_CONTIGUOUS_RAW_IQ' or info['format'] != 'sc16_le' or info['channels'] != 1:
        raise ValueError('VOID: incomplete or unsupported streamed recording')
    count, rate, first = info['samples_per_channel'], info['sample_rate_hz'], info['first_sample_tick']
    if count <= 0 or count != info['requested_samples_per_channel'] or rate <= 0 or first < 0:
        raise ValueError('VOID: invalid sample/time extent')
    if info['rx_metadata_errors'] or info['timestamp_tick_rate_hz'] != rate:
        raise ValueError('VOID: RX metadata or timestamp rate')
    if (root / 'rx0.sc16').stat().st_size != count * 4:
        raise ValueError('VOID: partial IQ file')
    consumed = blocks = 0
    with (root / 'timestamps.tsv').open() as file:
        for row in csv.DictReader(file, delimiter='\t'):
            offset, tick, size = (int(row[k]) for k in ('offset_samples', 'first_tick', 'samples'))
            if offset != consumed or tick != first + consumed or not 0 < size <= 65536:
                raise ValueError('VOID: timestamp gap/overlap or invalid block')
            consumed += size; blocks += 1
    if consumed != count or blocks != info['blocks']:
        raise ValueError('VOID: timestamp journal extent mismatch')
    if (root / 'nic_before.txt').read_text().strip() != (root / 'nic_after.txt').read_text().strip():
        raise ValueError('VOID: NIC missed counter changed')
    if not re.fullmatch(r'[0-9a-f]{64}  rx0\.sc16\n', (root / 'IQ_SHA256SUMS').read_text()):
        raise ValueError('VOID: missing streaming SHA256')
    return info


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    result = validate(args.capture)
    print(json.dumps(dict(status='VALID_CONTIGUOUS_RAW_IQ', channels=result['channels'],
                          duration_s=result['duration_s'], samples_per_channel=result['samples_per_channel'],
                          nr_message_decode_coverage='NOT_YET_VALIDATED',
                          checksum='generated during recording; independent reread not performed')))
