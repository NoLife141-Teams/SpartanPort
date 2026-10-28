"""Independent analytic projection and image-translation controls."""
import unittest
import numpy as np
from tools.gow3_reprojection import Viewport, reproject
from tools.validate_gow3_pixel_reprojection import match, HALF, SIDE


class CalibratedReprojectionTests(unittest.TestCase):
    def test_minus_w_near_plane_and_offset_viewport(self):
        # Analytic perspective: x_clip=x, y_clip=y, z_clip=11z/9-20/9, w=z.
        matrix = np.array([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 11/9, -20/9], [0, 0, 1, 0]])
        previous = matrix.copy()
        previous[:, 3] -= matrix[:, 0]*.1
        view = Viewport(11, 553, 960, -540, .2, .9, True)
        world = np.array([.2, .1, 1.5, 1])  # Negative exported clip Z, valid MinusW.
        clip = matrix @ world
        ndc = clip[:3]/clip[3]
        x = view.x+(ndc[0]+1)*480
        y = view.y+(ndc[1]+1)*-270
        depth = .2+(ndc[2]+1)/2*.7
        px, py, pz, valid = reproject(matrix, previous, view, view, x, y, depth)
        self.assertTrue(valid)
        self.assertAlmostEqual(float(px), x-.1/1.5*480, places=9)
        self.assertAlmostEqual(float(py), y, places=9)
        self.assertAlmostEqual(float(pz), depth, places=12)
        wrong = Viewport(11, 553, 960, -540, .2, .9, False)
        badx, _, _, _ = reproject(matrix, previous, wrong, wrong, x, y, depth)
        self.assertGreater(abs(float(badx-px)), 1)

    def test_moving_object_requires_history(self):
        view = Viewport(0, 0, 640, 480, 0, 1, False)
        now = np.eye(4)
        before = now.copy()
        before[0, 3] = -.02
        px, _, _, valid = reproject(now, before, view, view, 320.5, 240.5, .5)
        history = np.eye(4)
        history[0, 3] = -.1
        objx, _, _, objvalid = reproject(now, before, view, view, 320.5, 240.5, .5, history)
        self.assertTrue(valid and objvalid)
        self.assertAlmostEqual(float(objx-px), -32, places=9)

    def test_invalid_contract_and_background(self):
        with self.assertRaises(ValueError):
            Viewport(0, 0, 640, 0, 0, 1, True)
        view = Viewport(0, 480, 640, -480, 0, 1, True)
        *_, valid = reproject(np.eye(4), np.eye(4), view, view, [10, 10, -1], [10, 10, 10], [1, -1, .5])
        self.assertFalse(valid.any())
        with self.assertRaises(np.linalg.LinAlgError):
            reproject(np.zeros((4, 4)), np.eye(4), view, view, 10, 10, .5)

    def test_image_match_is_independent_of_camera(self):
        random = np.random.default_rng(42)
        current = random.random((128, 128), dtype=np.float32)
        # Pixel at (64,64) now appeared at (61,66) previously.
        previous = np.roll(current, (2, -3), axis=(0, 1))
        windows = np.lib.stride_tricks.sliding_window_view(previous, (SIDE, SIDE))
        result = match(windows, current[64-HALF:65+HALF, 64-HALF:65+HALF], 64, 64)
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[0], [-3, 2], atol=.1)

    def test_ambiguous_patch_is_rejected(self):
        image = np.ones((128, 128), dtype=np.float32)
        windows = np.lib.stride_tricks.sliding_window_view(image, (SIDE, SIDE))
        self.assertIsNone(match(windows, image[64-HALF:65+HALF, 64-HALF:65+HALF], 64, 64))


if __name__ == '__main__':
    unittest.main()
