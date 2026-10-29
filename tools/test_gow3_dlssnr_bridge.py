"""Exercise the public-SDK diagnostic and the LDR boundary, without NGX/GPU calls.

Usage: python tools/test_gow3_dlssnr_bridge.py path/to/spartanport_dlssnr.dll
Only the diagnostic build is accepted: synthetic handles must never reach NGX.
"""
import ctypes as c
import pathlib
import sys
import unittest


class Range(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ("aspect", "mip", "levels", "layer", "layers")]


class Image(c.Structure):
    _fields_ = [("image", c.c_uint64), ("view", c.c_uint64), ("range", Range),
                ("format", c.c_int32), ("width", c.c_uint32), ("height", c.c_uint32)]


class Settings(c.Structure):
    _fields_ = [(name, c.c_float) for name in ("intensity", "tone", "structure", "global_tone", "skin")]
    _fields_ += [("style", c.c_uint32), ("auto_mask", c.c_int32)]


class Frame(c.Structure):
    _fields_ = [(name, Image) for name in ("color", "output", "depth", "motion")]
    _fields_ += [("display_encoded", c.c_int32), ("reset", c.c_int32),
                ("depth_inverted", c.c_int32), ("settings", Settings)]


class Api(c.Structure):
    _fields_ = [("abi", c.c_uint32), ("compiled", c.c_int32)]
    _fields_ += [(name, c.c_void_p) for name in ("configure", "instance", "device", "initialize",
                  "create", "evaluate", "release", "shutdown", "problem")]


module = c.CDLL(str(pathlib.Path(sys.argv.pop(1)).resolve()))
module.SpartanDlssNrGetApi.restype = c.POINTER(Api)
api = module.SpartanDlssNrGetApi().contents
if api.abi != 1 or api.compiled:
    raise RuntimeError("These synthetic-handle tests require the ABI 1 public-SDK diagnostic build")
evaluate = c.CFUNCTYPE(c.c_int32, c.c_void_p, c.POINTER(Frame))(api.evaluate)
problem = c.CFUNCTYPE(c.c_char_p)(api.problem)


def frame():
    result = Frame()
    for index, name in enumerate(("color", "output"), 1):
        setattr(result, name, Image(index, index, Range(1, 0, 1, 0, 1), 109, 1920, 1080))
    result.display_encoded = 1
    result.settings = Settings(1, 1, 0, 0, 0, 0, 1)
    return result


class Contract(unittest.TestCase):
    def reject(self, value, expected):
        self.assertEqual(evaluate(1, c.byref(value) if value else None), 0)
        self.assertIn(expected, problem().decode())

    def test_null_frame(self):
        self.reject(None, "valid")

    def test_hdr_requires_explicit_ldr_contract(self):
        value = frame()
        value.display_encoded = 0
        self.reject(value, "post-tonemap LDR")

    def test_srgb_view_would_decode_display_encoding(self):
        value = frame()
        value.color.format = 43  # VK_FORMAT_R8G8B8A8_SRGB
        self.reject(value, "linear HDR/sRGB")

    def test_output_cannot_alias_input(self):
        value = frame()
        value.output.image = value.color.image
        self.reject(value, "separate RGBA32F")

    def test_mismatched_extents(self):
        value = frame()
        value.output.width = 1280
        self.reject(value, "display extent")

    def test_nonfinite_strength(self):
        value = frame()
        value.settings.tone = float("nan")
        self.reject(value, "Invalid NR strengths")

    def test_unsupported_global_tone(self):
        value = frame()
        value.settings.global_tone = 1
        self.reject(value, "not exposed")

    def test_local_structure_needs_temporal_inputs(self):
        value = frame()
        value.settings.structure = 1
        self.reject(value, "aligned depth and motion")

    def test_unpaired_depth_rejected(self):
        value = frame()
        value.depth = value.color
        self.reject(value, "matching pair")

    def test_valid_contract_reports_missing_sdk(self):
        self.reject(frame(), "SDK not compiled")

    def test_missing_sdk_never_returns_extensions(self):
        query = c.CFUNCTYPE(c.c_int32, c.POINTER(c.c_uint32), c.POINTER(c.c_void_p))(api.instance)
        count, extensions = c.c_uint32(99), c.c_void_p(1)
        self.assertEqual(query(c.byref(count), c.byref(extensions)), 0)
        self.assertEqual(count.value, 0)
        self.assertIsNone(extensions.value)


unittest.main()
