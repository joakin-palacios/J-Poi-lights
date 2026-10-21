import json
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from datetime import datetime, timezone
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
BASE_VERSION = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))["version"]


class BuildEnv(dict):
    def AddBuildMiddleware(self, callback, pattern):
        self.middleware = callback
        self.pattern = pattern

    def AddPostAction(self, target, callback):
        self.post_action = callback

    def Object(self, node, **kwargs):
        return kwargs


def metadata_env(when):
    env = BuildEnv(CPPDEFINES=[("WLED_REPO", '"example/repo"')], PIOENV="example", PIOPLATFORM="espressif32")
    with patch("datetime.datetime") as clock:
        clock.now.return_value = when
        runpy.run_path(str(ROOT / "pio-scripts/set_metadata.py"), init_globals={"Import": lambda _: None, "env": env})
        clock.now.assert_called_once_with(timezone.utc)
    return env


class BuildIdentificationTests(unittest.TestCase):
    def test_identifier_is_generated_once_and_only_added_to_metadata_object(self):
        env = metadata_env(datetime(2026, 10, 21, 15, 30, 42, 123456, tzinfo=timezone.utc))
        self.assertEqual(env["J_POI_BUILD_ID"], "20261021-153042-123456Z")
        original = env["CPPDEFINES"].copy()
        first = env.middleware(env, "wled_metadata.cpp")["CPPDEFINES"]
        second = env.middleware(env, "wled_metadata.cpp")["CPPDEFINES"]
        self.assertEqual(first, second)
        self.assertEqual(env["CPPDEFINES"], original)
        self.assertEqual(env.pattern, "*/wled_metadata.cpp")
        self.assertIn(("J_POI_BUILD_ID", '\\"20261021-153042-123456Z\\"'), first)
        self.assertIn(("WLED_VERSION", BASE_VERSION), first)

    def test_release_names_preserve_base_version_and_previous_builds(self):
        with tempfile.TemporaryDirectory(prefix=".jpoi-test-", dir=ROOT) as temporary:
            folder = Path(temporary)
            source = folder / "firmware.bin"
            source.write_bytes(b"application firmware")
            names = []
            for microsecond in (123456, 123457):
                env = metadata_env(datetime(2026, 10, 21, 15, 30, 42, microsecond, tzinfo=timezone.utc))
                env["CPPDEFINES"].append(("WLED_RELEASE_NAME", '\\"ESP32-S3_4M_qspi\\"'))
                script = runpy.run_path(str(ROOT / "pio-scripts/output_bins.py"), init_globals={"Import": lambda _: None, "env": env})
                script["create_release"].__globals__["OUTPUT_DIR"] = str(folder) + os.sep
                script["_create_dirs"]()
                script["create_release"](str(source))
                names.append(folder / "release" / f'J-Poi_{env["J_POI_BUILD_ID"]}_WLED_{BASE_VERSION}_ESP32-S3_4M_qspi.bin')
            self.assertNotEqual(names[0], names[1])
            for name in names:
                self.assertEqual(name.read_bytes(), source.read_bytes())

    def test_environment_without_release_name_still_gets_stamped_output(self):
        env = metadata_env(datetime(2026, 10, 21, tzinfo=timezone.utc))
        with tempfile.TemporaryDirectory(prefix=".jpoi-test-", dir=ROOT) as temporary:
            folder = Path(temporary)
            source = folder / "firmware.bin"
            source.write_bytes(b"application firmware")
            script = runpy.run_path(str(ROOT / "pio-scripts/output_bins.py"), init_globals={"Import": lambda _: None, "env": env})
            script["create_release"].__globals__["OUTPUT_DIR"] = str(folder) + os.sep
            script["_create_dirs"]()
            script["create_release"](str(source))
            output = folder / "firmware" / f'J-Poi_{env["J_POI_BUILD_ID"]}_example.bin'
            self.assertEqual(output.read_bytes(), source.read_bytes())


if __name__ == "__main__":
    unittest.main()
