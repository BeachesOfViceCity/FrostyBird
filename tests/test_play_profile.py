"""Launching from a previous capture shell must still give normal play."""
import sys
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from frosty import play_environment


class PlayEnvironmentTest(unittest.TestCase):
    def test_old_capture_shell_cannot_enable_pixel_or_queue_capture(self):
        env=dict(FROSTY_PRESENT="gdi",FROSTY_CAPTURE_DIR="previous",FROSTY_TRACE_FENCES="0",
                 FROSTY_TRACE_WORK="1",FROSTY_CAPTURE_PASS="old",FROSTY_HOLD_LIFTS="old",
                 FROSTY_STREET_PAIR="1",FROSTY_HOLD_VERIFIED_PRESENT="1",FROSTY_FRAME_TIMES="old",
                 FROSTY_SHADER_CACHE="cache",VKD3D_SHADER_CACHE_PATH="driver-cache")
        with patch("frosty.radeon_play_adapter",return_value=False):play_environment(env)
        self.assertEqual({k:v for k,v in env.items() if k.startswith("FROSTY_")},
                         {"FROSTY_PRESENT":"dxgi","FROSTY_SHADER_CACHE":"cache","FROSTY_SPIRV_STAGES":"1","FROSTY_ASYNC_HTTP_HEADERS":"1"})
        self.assertEqual(env["VKD3D_SHADER_CACHE_PATH"],"driver-cache")

    def test_measured_adapter_defaults_respect_explicit_scheduler(self):
        with patch("frosty.radeon_play_adapter",return_value=True):
            values={};play_environment(values)
            self.assertEqual((values["RADV_PERFTEST"],values["FROSTY_NATIVE_GROUP_ORDINAL"]),("pswave32,gewave32","1"))
            self.assertEqual(values["VKD3D_CONFIG"],"retain_psos,no_upload_hvv")
            values={"RADV_PERFTEST":"","FROSTY_NATIVE_GROUP_ORDINAL":"0","VKD3D_CONFIG":"retain_psos"};play_environment(values)
            self.assertEqual((values["RADV_PERFTEST"],values["FROSTY_NATIVE_GROUP_ORDINAL"]),("","0"))
            self.assertEqual(values["VKD3D_CONFIG"],"retain_psos")


if __name__=="__main__":unittest.main()
