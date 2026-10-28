"""Check bounded GoW3 metadata windows; this does not validate pixels or DLAA."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


def identity(resource):
    if resource.get('resolution') == 2:
        return resource['uid'], resource['backing_ordinal']
    return None


def analyze(path):
    raw = path.read_bytes()
    lines = raw.splitlines(keepends=True)
    rows = [json.loads(line) for line in lines]
    start, end = rows[0], rows[-1]
    if start.get('schema') != 'gow3-provenance-v2' or end.get('kind') != 'end':
        raise ValueError('Missing expected start/end markers: ' + str(path))
    draws = [r for r in rows if r['kind'] == 'draw']
    intervals = [r for r in rows if r['kind'] == 'interval']
    events = [r for r in rows if r['kind'] == 'provenance_events']
    errors = []
    checks = {
        'byte_count': sum(map(len, lines[:-1])) == end['bytes_before_end'],
        'byte_limit': len(raw) <= start['byte_budget'],
        'draw_count': len(draws) == end['retained_draws'],
        'two_intervals': len(intervals) == end['closed_intervals'] == 2,
        'contiguous_draws': bool(draws) and [d['draw'] for d in draws] ==
            list(range(draws[0]['draw'], draws[-1]['draw'] + 1)),
        'batch_accounting': end['provenance_batches_seen'] ==
            end['provenance_batches_written'] + end['provenance_empty_batches_elided'] + end['provenance_batches_dropped'],
        'event_accounting': end['provenance_events_seen'] ==
            end['provenance_events_written'] + end['provenance_events_dropped'],
        'serialized_event_count': sum(len(r['events']) for r in events) == end['provenance_events_written'],
        'lossless_window': not any(end[k] for k in (
            'dropped_draws', 'io_errors_before_end', 'byte_budget_drops',
            'provenance_batches_dropped', 'provenance_events_dropped',
            'provenance_busy_drains', 'provenance_incomplete_draws'))
            and end['provenance_lost_total'] == end['provenance_loss_baseline'],
        'instrumented_window_complete': end['complete_instrumented_draw_window'] == 1,
    }
    errors.extend(name for name, passed in checks.items() if not passed)
    camera = [d for d in draws if d['vs'] == 'aed40011eb5259f3' and d['camera_observed']]
    per_interval = []
    matrices = []
    for interval in intervals:
        ds = [d for d in draws if d['interval'] == interval['interval']]
        cs = [d for d in camera if d['interval'] == interval['interval']]
        variants = {tuple(d['words_hex'][:16]) for d in cs if d['snapshot_present']}
        matrix = next(iter(variants)) if len(variants) == 1 else None
        if matrix:
            matrices.append([struct.unpack('<f', int(w, 16).to_bytes(4, 'little'))[0] for w in matrix])
        closing = any(e['observed_at'] == 'before_accepted_flip' and
                      e['closing_flip'] == int(interval['closing_flip']['id'], 16)
                      for e in events if e['interval'] == interval['interval'])
        if not closing:
            errors.append('missing_closing_provenance_' + str(interval['interval']))
        per_interval.append(dict(interval=interval['interval'], draws=len(ds),
                                 camera_status=interval['camera_status'], camera_draws=len(cs),
                                 snapshot_missing=sum(d['snapshot_missing'] for d in cs),
                                 snapshot_conflicts=sum(d['snapshot_conflict'] for d in cs),
                                 vp_variants=len(variants), closing_provenance=closing,
                                 selected_vs_keys=dict(Counter(d['vs_permutation_key'] for d in cs))))
    camera_depth = {identity(d['effective_depth_provenance']) for d in camera}
    camera_depth.discard(None)
    combinations = {}
    for draw in draws:
        reads = [b for b in draw['image_descriptors'] if identity(b['resource'])]
        depth_reads = [b for b in reads if identity(b['resource']) in camera_depth]
        color_reads = [b for b in reads if b['resource']['samples'] == 2 and b['resource']['view_format'] == 43]
        for depth in depth_reads:
            for color in color_reads:
                key = (draw['vs'], draw['ps'], depth['binding'], color['binding'],
                       identity(depth['resource']), identity(color['resource']))
                entry = combinations.setdefault(key, dict(vs=draw['vs'], ps=draw['ps'],
                    depth_binding=depth['binding'], color_binding=color['binding'],
                    depth_identity=list(identity(depth['resource'])),
                    color_identity=list(identity(color['resource'])),
                    vs_key=draw['vs_permutation_key'], ps_key=draw['ps_permutation_key'],
                    first_draw=draw['draw'], observations=0))
                entry['observations'] += 1
    return dict(file=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(),
                bytes=len(raw), checks=checks, errors=errors, intervals=per_interval,
                camera_depth_identities=[list(k) for k in sorted(camera_depth)],
                camera_vp_max_component_delta=max(abs(a-b) for a,b in zip(*matrices))
                    if len(matrices) == 2 else None,
                depth_srgb_composition_candidates=list(combinations.values()),
                footer=end, metadata_valid=not errors,
                dlaa_validated=False, pixel_contents_captured=False,
                labels_are_intent=True, movement_cause_proven=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    files = sorted((args.session / 'logs').glob('group.*.jsonl'))
    if not files:
        parser.error('No grouped metadata windows found')
    result = dict(windows=[analyze(path) for path in files],
                  scope='Ordered instrumented graphics path only; no exhaustive writes, preserved pixels, or GPU lifetime proof.')
    output = args.session / 'diagnostic-summary.json'
    with output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
    for window in result['windows']:
        print(Path(window['file']).name, 'valid=' + str(window['metadata_valid']),
              'draws=' + str(window['footer']['retained_draws']),
              'vp_delta=' + str(window['camera_vp_max_component_delta']),
              'composition_candidates=' + str(len(window['depth_srgb_composition_candidates'])))
    print('Report: ' + str(output.resolve()))
    return 0 if all(w['metadata_valid'] for w in result['windows']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
