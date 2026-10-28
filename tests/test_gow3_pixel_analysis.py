"""Reject forged frame/resource associations and corrupted raw GPU exports."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import numpy as np
from tools.analyze_gow3_pixels import analyze


class PixelAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'pixels').mkdir()
        (self.root / 'logs').mkdir()
        matrix = np.eye(4).ravel(order='F').tolist()
        words = [format(struct.unpack('<I', struct.pack('<f', v))[0], 'x') for v in matrix]
        self.results = []
        rows = []
        for i in (1, 2):
            sources = {}
            for label, uid, fmt, view in (('depth', 7, 130, 2), ('color', 10, 43, 1)):
                sources[label] = dict(uid=uid, backing=1, view_ordinal=view, image=uid*10,
                                      view=uid*100, address=uid*1000, format=fmt, samples=2)
            r = dict(kind='result', complete=True, error='', interval=i, draw=i*10,
                     tick=i*2, submitted=1, gpu_complete=1, closing_flip=i, camera_frame=i-1,
                     width=2, height=2, bytes=160, file=f'interval-{i}.rgba-depth.f32', camera_vp=matrix, **sources)
            self.results.append(r)
            descriptors = []
            for binding, label in ((1, 'depth'), (2, 'color')):
                resource = dict(sources[label], backing_ordinal=1, resolution=2, current_backing=1)
                descriptors.append(dict(binding=binding, provenance_observed=1, resource=resource))
            rows += [dict(kind='draw', interval=i, draw=i*10, vs='475a41468b1a1fbc', ps='475a4146e3a74097',
                          vs_permutation_key='efd04d8023fec408', ps_permutation_key='efd0530963dc1d55',
                          recorded=1, indirect=0, proxy_path=0, camera_observed=0, image_descriptors=descriptors),
                     dict(kind='draw', interval=i, draw=i*10-1, camera_observed=1, snapshot_present=1,
                          snapshot_missing=0, snapshot_conflict=0, words_hex=words,
                          camera_source_depth=dict(uid='7', guest_address_label=format(7000, 'x'))),
                     dict(kind='interval', interval=i, closing_flip=dict(id=format(i, 'x')), camera_frame=i-1)]
            pixels = np.tile(np.array([[.1, .2, .3, 1, .25], [.2, .3, .4, 1, .5]], dtype='<f4'), (2, 2, 1, 1))
            pixels.tofile(self.root / 'pixels' / r['file'])
        (self.root / 'logs/group.stationary.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))

    def run_analysis(self):
        rows = [dict(kind='start', schema='gow3-real-pixels-v1')] + self.results
        (self.root / 'pixels/pixels.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
        return analyze(self.root)

    def test_two_distinct_samples_and_links(self):
        report = self.run_analysis()
        self.assertTrue(report['valid'])
        self.assertEqual(report['frames'][0]['depth_distinct_sample_pixels'], 4)
        self.assertEqual(report['frames'][0]['color_distinct_sample_pixels'], 4)
        self.assertFalse(report['pre_hud_proven'])

    def test_forged_flip_rejected(self):
        self.results[0]['closing_flip'] += 1
        self.assertIn('closure_token_mismatch', self.run_analysis()['frames'][0]['errors'])

    def test_recycled_resource_label_rejected(self):
        self.results[0]['depth']['uid'] += 1
        self.assertIn('depth_source_identity_mismatch', self.run_analysis()['frames'][0]['errors'])

    def test_missing_submission_tick_rejected(self):
        self.results[0]['tick'] = 0
        with self.assertRaises(ValueError):
            self.run_analysis()

    def test_truncated_file_rejected(self):
        (self.root / 'pixels' / self.results[0]['file']).write_bytes(b'\0'*40)
        with self.assertRaises(ValueError):
            self.run_analysis()

    def test_nonfinite_depth_rejected(self):
        path = self.root / 'pixels' / self.results[0]['file']
        pixels = np.fromfile(path, dtype='<f4')
        pixels[4] = float('nan')
        pixels.tofile(path)
        # Nonfinite values must fail explicitly, never become a valid report.
        with self.assertRaises(ValueError):
            self.run_analysis()


if __name__ == '__main__':
    unittest.main()
