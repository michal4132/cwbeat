#!/usr/bin/env python3
"""Read and CRC-verify the experiment device's full flash without writing or resetting."""

import argparse
import json
from pathlib import Path
import sys

import bt_patch as patch


def dump(port, output):
    output = Path(output).expanduser().resolve()
    repository = Path(__file__).resolve().parents[2]
    patch.need(not output.is_relative_to(repository),
               'Keep the full firmware backup outside the repository')
    output.mkdir(parents=True, exist_ok=False)
    print('Reading and CRC-verifying 4 MiB flash; leave the device connected.', file=sys.stderr)
    with patch.bl.open_session(port) as session:
        patch.hardware_guard(session)
        data = patch.stable_full_read(session)
        patch.need(len(data) == patch.FLASH_SIZE, 'Incomplete full flash dump')
    digest = patch.sha(data)
    report = {
        'backup': str(output / 'backup.bin'),
        'size': len(data),
        'sha256': digest,
        'native_crc': patch.bl.crc32(data),
        'flash_id': patch.FLASH_ID.hex(),
        'matches_patch_source': digest == patch.SOURCE_SHA,
        'reset_sent': False,
    }
    patch.save_new(output / 'backup.bin', data)
    patch.save_new(output / 'result.json', patch.canonical(report))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='Device already in SBL USB recovery')
    parser.add_argument('--output', required=True, help='New directory outside the repository')
    args = parser.parse_args()
    report = dump(args.port, args.output)
    print(json.dumps(report, indent=2, sort_keys=True))
    if not report['matches_patch_source']:
        print('Backup saved, but it does not match the original BT patch source; '
              'plan/apply will reject it.', file=sys.stderr)


if __name__ == '__main__':
    main()
