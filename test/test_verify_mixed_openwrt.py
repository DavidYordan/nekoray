from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "tools" / "verify_mixed_openwrt.py"
SPEC = importlib.util.spec_from_file_location("verify_mixed_openwrt", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = probe
SPEC.loader.exec_module(probe)


def source_config() -> dict:
    return {
        "log": {"level": "debug", "output": "unsafe.log"},
        "inbounds": [
            {
                "type": "mixed",
                "tag": "mixed-in",
                "listen": "0.0.0.0",
                "listen_port": 12080,
                "set_system_proxy": True,
                "users": [{"username": "probe-user", "password": "probe-pass"}],
            },
            {
                "type": "mixed",
                "tag": "aux-mixed-1",
                "listen": "127.0.0.1",
                "listen_port": 12100,
            },
        ],
        "outbounds": [
            {
                "type": "anytls",
                "tag": "proxy",
                "server": "example.invalid",
                "server_port": 443,
                "password": "outbound-secret",
                "detour": "front",
            },
            {"type": "trojan", "tag": "front", "server": "front.invalid"},
            {"type": "direct", "tag": "direct"},
        ],
        "route": {
            "auto_detect_interface": False,
            "default_interface": "Windows Adapter",
            "final": "proxy",
            "geoip": {"path": "C:/geoip.db"},
            "geosite": {"path": "C:/geosite.db"},
            "rule_set": [{"type": "local", "path": "C:/rules.srs"}],
            "rules": [{"outbound": "direct"}],
        },
        "experimental": {"clash_api": {"external_controller": "127.0.0.1:9090"}},
        "services": [{"type": "resolved"}],
    }


class TighteningTests(unittest.TestCase):
    def tighten(self, value: dict, **kwargs):
        raw = (json.dumps(value) + "\n").encode()
        return probe.tighten_config(
            value,
            raw,
            inbound_tag="mixed-in",
            expected_outbound_tag="proxy",
            **kwargs,
        )

    def test_tightens_to_one_loopback_mixed_without_side_effect_surfaces(self):
        result = self.tighten(source_config())
        config = result.data
        self.assertEqual(len(config["inbounds"]), 1)
        inbound = config["inbounds"][0]
        self.assertEqual(inbound["type"], "mixed")
        self.assertEqual(inbound["listen"], "127.0.0.1")
        self.assertEqual(inbound["listen_port"], 52080)
        self.assertNotIn("set_system_proxy", inbound)
        self.assertNotIn("experimental", config)
        self.assertNotIn("services", config)
        self.assertNotIn("ntp", config)
        self.assertEqual([item["tag"] for item in config["outbounds"]], ["proxy", "front"])
        self.assertEqual(config["log"]["output"], "core.log")
        self.assertEqual(config["log"]["level"], "debug")
        self.assertFalse(config["route"]["auto_detect_interface"])
        self.assertNotIn("default_interface", config["route"])
        self.assertNotIn("geoip", config["route"])
        self.assertNotIn("geosite", config["route"])
        self.assertNotIn("rule_set", config["route"])
        self.assertEqual(
            config["route"]["rules"],
            [
                {"inbound": ["mixed-in"], "action": "sniff"},
                {
                    "inbound": ["mixed-in"],
                    "action": "route",
                    "outbound": "proxy",
                },
            ],
        )
        self.assertEqual(config["route"]["final"], "proxy")
        self.assertTrue(result.credentials.present)
        public = json.dumps(result.public_summary())
        self.assertNotIn("probe-user", public)
        self.assertNotIn("probe-pass", public)
        self.assertNotIn("outbound-secret", public)

    def test_preserves_detour_by_default(self):
        result = self.tighten(source_config())
        selected = next(item for item in result.data["outbounds"] if item["tag"] == "proxy")
        self.assertEqual(selected["detour"], "front")
        self.assertTrue(result.expected_outbound_has_detour)

    def test_explicit_anytls_variant_removes_detour_and_sets_native(self):
        value = source_config()
        value["outbounds"][0]["client"] = "mihomo/1.19.28"
        result = self.tighten(value, remove_detour=True, anytls_client="native")
        selected = next(item for item in result.data["outbounds"] if item["tag"] == "proxy")
        self.assertNotIn("detour", selected)
        self.assertNotIn("client", selected)
        self.assertFalse(result.expected_outbound_has_detour)

    def test_interface_auto_detection_is_only_changed_by_explicit_diagnostic_override(self):
        value = source_config()
        result = self.tighten(value, force_auto_detect_interface=True)
        self.assertTrue(result.data["route"]["auto_detect_interface"])
        self.assertTrue(
            any("diagnostic override" in item for item in result.transformations)
        )

    def test_rejects_tun_or_tproxy_even_though_other_inbounds_are_removed(self):
        for inbound_type in ("tun", "tproxy"):
            value = source_config()
            value["inbounds"].append({"type": inbound_type, "tag": "danger"})
            with self.subTest(inbound_type=inbound_type):
                with self.assertRaises(probe.SafetyError):
                    self.tighten(value)

    def test_rejects_product_mapping_different_from_expected_main_outbound(self):
        value = source_config()
        value["route"]["final"] = "direct"
        with self.assertRaises(probe.SafetyError):
            self.tighten(value)

    def test_rejects_platform_specific_outbound_binding(self):
        value = source_config()
        value["outbounds"][0]["bind_interface"] = "Windows Adapter"
        with self.assertRaises(probe.SafetyError):
            self.tighten(value)

    def test_rejects_ntp_system_clock_write(self):
        value = source_config()
        value["ntp"] = {"enabled": True, "write_to_system": True}
        with self.assertRaises(probe.SafetyError):
            self.tighten(value)

    def test_removes_ntp_without_system_clock_write(self):
        value = source_config()
        value["ntp"] = {"enabled": True, "write_to_system": False}
        result = self.tighten(value)
        self.assertNotIn("ntp", result.data)

    def test_rejects_dynamic_or_direct_selected_outbound(self):
        for outbound_type in ("direct", "selector", "urltest"):
            value = source_config()
            value["outbounds"][0].pop("detour", None)
            value["outbounds"][0]["type"] = outbound_type
            with self.subTest(outbound_type=outbound_type):
                with self.assertRaises(probe.SafetyError):
                    self.tighten(value)

    def test_rejects_missing_or_cyclic_detour(self):
        for detour in ("missing", "proxy"):
            value = source_config()
            value["outbounds"][0]["detour"] = detour
            with self.subTest(detour=detour):
                with self.assertRaises(probe.SafetyError):
                    self.tighten(value)

    def test_strict_loader_rejects_duplicate_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_text('{"inbounds":[],"inbounds":[]}', encoding="utf-8")
            with self.assertRaises(probe.ProbeError):
                probe.load_strict_json(path)


class BaselineValidationTests(unittest.TestCase):
    def snapshot(self, **overrides):
        process = probe.ProcessIdentity(
            pid=321,
            start_ticks="12345",
            executable="/usr/bin/sing-box",
            command_line="/usr/bin/sing-box run -c /etc/routefluent/config.json ",
        )
        values = {
            "active_processes": (process,),
            "active_config_sha256": "1" * 64,
            "active_manifest_sha256": "2" * 64,
            "core_sha256": probe.EXPECTED_REMOTE_CORE_SHA256,
            "core_version": probe.EXPECTED_REMOTE_CORE_VERSION,
            "active_listeners": ("tcp 0 0 0.0.0.0:10085 LISTEN 321/sing-box",),
        }
        values.update(overrides)
        return probe.BaselineSnapshot(**values)

    def test_accepts_exactly_one_auditable_active_process(self):
        probe.validate_remote_core(self.snapshot())

    def test_rejects_ambiguous_or_unauditable_active_baseline(self):
        process = self.snapshot().active_processes[0]
        cases = {
            "no active process": {"active_processes": ()},
            "multiple active processes": {"active_processes": (process, process)},
            "missing active config": {"active_config_sha256": "MISSING"},
            "missing active manifest": {"active_manifest_sha256": "MISSING"},
            "no active listeners": {"active_listeners": ()},
        }
        for name, overrides in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(probe.SafetyError):
                    probe.validate_remote_core(self.snapshot(**overrides))


class HostKeyValidationTests(unittest.TestCase):
    class FakeKey:
        def __init__(self, algorithm: str, raw: bytes):
            self.algorithm = algorithm
            self.raw = raw

        def get_name(self):
            return self.algorithm

        def asbytes(self):
            return self.raw

    def test_accepts_only_an_exact_algorithm_and_fingerprint_pair(self):
        raw = b"audited-ed25519-key"
        key = self.FakeKey("ssh-ed25519", raw)
        original = probe.EXPECTED_HOST_KEYS_SHA256
        probe.EXPECTED_HOST_KEYS_SHA256 = {
            "ssh-ed25519": probe._sha256_host_key(key),
        }
        try:
            probe.validate_host_key(key)
            with self.assertRaises(probe.SafetyError):
                probe.validate_host_key(self.FakeKey("ssh-rsa", raw))
            with self.assertRaises(probe.SafetyError):
                probe.validate_host_key(self.FakeKey("ssh-ed25519", b"different"))
        finally:
            probe.EXPECTED_HOST_KEYS_SHA256 = original


class CleanupSafetyTests(unittest.TestCase):
    class FakeRemote:
        def __init__(self, responses=None):
            self.commands: list[str] = []
            self.responses = list(responses or [])

        def run(self, command, timeout=120, stdin_data=b""):
            self.commands.append(command)
            if self.responses:
                return self.responses.pop(0)
            return probe.CommandResult("", "", 0)

    def test_exact_directory_cleanup_uses_only_whitelisted_files(self):
        remote = self.FakeRemote()
        directory = "/tmp/nekoray-mixed-probe-20260720T120000Z-012345abcdef"
        probe.delete_exact_remote_dir(remote, directory)
        command = remote.commands[0]
        self.assertNotIn("rm -rf", command)
        self.assertNotIn("*", command)
        for name in ("config.json.partial", "config.json", "core.log", "core.pid"):
            self.assertIn(f"{directory}/{name}", command)
        self.assertIn(f"rmdir {directory}", command)

    def test_cleanup_refuses_directory_outside_exact_prefix(self):
        remote = self.FakeRemote()
        with self.assertRaises(probe.SafetyError):
            probe.delete_exact_remote_dir(remote, "/tmp/not-the-probe")
        self.assertEqual(remote.commands, [])

    def test_identity_change_is_detected_before_any_signal(self):
        original = probe.ProcessIdentity(
            pid=123,
            start_ticks="100",
            executable="/usr/bin/sing-box",
            command_line=(
                "/usr/bin/sing-box run -c "
                "/tmp/nekoray-mixed-probe-20260720T120000Z-012345abcdef/config.json "
            ),
        )
        changed_line = (
            "PROC\t123\t101\t/usr/bin/sing-box\t"
            "/usr/bin/sing-box run -c /tmp/different.json \n"
        )
        remote = self.FakeRemote([probe.CommandResult(changed_line, "", 0)])
        with self.assertRaises(probe.SafetyError):
            probe.terminate_exact_process(remote, original)
        self.assertEqual(len(remote.commands), 1)
        self.assertNotIn("kill -", remote.commands[0])

    def test_source_has_no_broad_process_or_recursive_delete_command(self):
        source = SCRIPT_PATH.read_text(encoding="utf-8")
        self.assertNotIn("kill $(pidof", source)
        self.assertNotIn("pkill ", source)
        self.assertNotIn("rm -rf", source)
        self.assertNotIn("nohup", source)
        self.assertIn("/sbin/start-stop-daemon", source)


if __name__ == "__main__":
    unittest.main()
