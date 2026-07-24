#!/usr/bin/env python3
"""Safely probe a NekoRay Mixed config on the dedicated OpenWrt lab host.

The remote probe is intentionally narrow: one loopback Mixed inbound on the
fixed port 52080, one explicitly selected main outbound, and no TUN/TProxy,
controller, service, file-log, system-network, or service-management actions.

Run ``--dry-run`` first.  A real run requires Paramiko and the ignored
RouteFluent lab credentials; it never prints those credentials or the
sanitized runtime config.
"""

from __future__ import annotations

import argparse
import base64
import copy
import dataclasses
import datetime as dt
import hashlib
import json
import os
import re
import secrets
import shlex
import socket
import sys
import time
from pathlib import Path
from typing import Any, Iterable


LAB_HOST = "192.168.1.7"
LAB_PORT = 22
LAB_USER = "root"
PROBE_PORT = 52080
REMOTE_CORE = "/usr/bin/sing-box"
REMOTE_ACTIVE_CONFIG = "/etc/routefluent/runtime/etc/sing-box/config.json"
REMOTE_ACTIVE_MANIFEST = "/etc/routefluent/runtime/manifest.json"
REMOTE_DIR_PREFIX = "/tmp/nekoray-mixed-probe-"
REMOTE_LOCK = "/tmp/nekoray-mixed-probe.lock"
EXPECTED_REMOTE_CORE_VERSION = "1.13.12-routefluent-anytls-client.7"
EXPECTED_REMOTE_CORE_SHA256 = (
    "8d142f917518cd1660d7370e61a93defc3b6827681b092c0088a7c4cd4b46331"
)
EXPECTED_HOST_KEYS_SHA256 = {
    "ssh-ed25519": "B3MIEoyShMUmamopxIyvXP0Es/jVV8EZHpQhazIbWWk",
    "ssh-rsa": "Gk4IyOmvTjgJGduqlX99RZAqeTKi68S6lLJAF8UcFsk",
}
DEFAULT_HTTP_URL = "http://www.gstatic.com/generate_204"
DEFAULT_HTTPS_URL = "https://www.gstatic.com/generate_204"
MAX_REMOTE_OUTPUT = 64 * 1024


class ProbeError(RuntimeError):
    """A normal validation or connectivity failure."""


class SafetyError(ProbeError):
    """A fail-closed safety or cleanup failure."""


@dataclasses.dataclass(frozen=True)
class Credentials:
    username: str = ""
    password: str = ""

    @property
    def present(self) -> bool:
        return bool(self.username or self.password)


@dataclasses.dataclass
class TightenedConfig:
    data: dict[str, Any]
    credentials: Credentials
    source_sha256: str
    sanitized_sha256: str
    source_inbound_count: int
    expected_outbound_tag: str
    expected_outbound_type: str
    expected_outbound_has_detour: bool
    transformations: list[str]

    def public_summary(self) -> dict[str, Any]:
        return {
            "source_sha256": self.source_sha256,
            "sanitized_sha256": self.sanitized_sha256,
            "source_inbound_count": self.source_inbound_count,
            "diagnostic_inbound_count": 1,
            "listen": "127.0.0.1",
            "listen_port": PROBE_PORT,
            "expected_outbound_tag": self.expected_outbound_tag,
            "expected_outbound_type": self.expected_outbound_type,
            "expected_outbound_has_detour": self.expected_outbound_has_detour,
            "inbound_auth_present": self.credentials.present,
            "transformations": list(self.transformations),
        }


@dataclasses.dataclass(frozen=True)
class CommandResult:
    stdout: str
    stderr: str
    exit_code: int


@dataclasses.dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    start_ticks: str
    executable: str
    command_line: str


@dataclasses.dataclass(frozen=True)
class BaselineSnapshot:
    active_processes: tuple[ProcessIdentity, ...]
    active_config_sha256: str
    active_manifest_sha256: str
    core_sha256: str
    core_version: str
    active_listeners: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class CurlResult:
    name: str
    exit_code: int
    http_code: int
    success: bool
    summary: str


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProbeError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def load_strict_json(path: Path) -> tuple[dict[str, Any], bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ProbeError(f"cannot read config: {path}: {exc}") from exc
    try:
        value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProbeError(f"config is not strict UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ProbeError("config must be a JSON object")
    return value, raw


def _require_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ProbeError(f"{label} must be an object")
    return value


def _require_object_list(value: Any, label: str) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise ProbeError(f"{label} must be an array")
    result: list[dict[str, Any]] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise ProbeError(f"{label}[{index}] must be an object")
        result.append(item)
    return result


def _validate_tag(tag: str, label: str) -> str:
    if not tag or len(tag) > 128 or re.search(r"[\x00-\x1f\x7f]", tag):
        raise ProbeError(f"{label} is empty, too long, or contains control characters")
    return tag


def _find_forbidden_key(value: Any, forbidden: set[str], path: str = "$") -> str | None:
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{path}.{key}"
            if key in forbidden:
                return child_path
            found = _find_forbidden_key(child, forbidden, child_path)
            if found:
                return found
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found = _find_forbidden_key(child, forbidden, f"{path}[{index}]")
            if found:
                return found
    return None


def _json_bytes(value: dict[str, Any]) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def _strict_outbound_closure(
    outbounds: list[dict[str, Any]], root_tag: str
) -> list[dict[str, Any]]:
    by_tag: dict[str, dict[str, Any]] = {}
    for index, outbound in enumerate(outbounds):
        tag = outbound.get("tag")
        if not isinstance(tag, str) or not tag:
            raise SafetyError(f"outbounds[{index}] has no non-empty string tag")
        if tag in by_tag:
            raise SafetyError(f"duplicate outbound tag {tag!r}")
        by_tag[tag] = outbound

    closure: list[dict[str, Any]] = []
    visited: set[str] = set()
    current = root_tag
    while current:
        if current in visited:
            raise SafetyError(f"outbound detour cycle at {current!r}")
        visited.add(current)
        outbound = by_tag.get(current)
        if outbound is None:
            raise SafetyError(f"outbound detour target {current!r} does not exist exactly once")
        outbound_type = str(outbound.get("type", "")).strip().lower()
        if outbound_type in {"direct", "block", "selector", "urltest", "url-test"}:
            raise SafetyError(
                f"strict remote line probe refuses outbound type {outbound_type!r} at {current!r}"
            )
        closure.append(outbound)
        if "detour" not in outbound:
            break
        detour = outbound.get("detour")
        if not isinstance(detour, str):
            raise SafetyError(f"outbound {current!r} has a non-string detour")
        current = detour.strip()

    return closure


def tighten_config(
    source: dict[str, Any],
    source_raw: bytes,
    *,
    inbound_tag: str,
    expected_outbound_tag: str,
    remove_detour: bool = False,
    anytls_client: str = "preserve",
    force_auto_detect_interface: bool = False,
) -> TightenedConfig:
    inbound_tag = _validate_tag(inbound_tag, "inbound tag")
    expected_outbound_tag = _validate_tag(expected_outbound_tag, "outbound tag")
    if anytls_client not in {"preserve", "native", "mihomo"}:
        raise ProbeError("unsupported AnyTLS client override")

    config = copy.deepcopy(source)
    transformations: list[str] = []
    inbounds = _require_object_list(config.get("inbounds"), "inbounds")
    if not inbounds:
        raise ProbeError("config has no inbounds")
    hazardous = [
        str(item.get("tag", "<untagged>"))
        for item in inbounds
        if item.get("type") in {"tun", "tproxy"}
    ]
    if hazardous:
        raise SafetyError(
            "refusing a source config containing TUN/TProxy inbound(s): "
            + ", ".join(hazardous)
        )
    targets = [item for item in inbounds if item.get("tag") == inbound_tag]
    if len(targets) != 1:
        raise ProbeError(
            f"expected exactly one inbound tagged {inbound_tag!r}, found {len(targets)}"
        )
    target = copy.deepcopy(targets[0])
    if target.get("type") != "mixed":
        raise ProbeError(f"inbound {inbound_tag!r} is not Mixed")
    target["listen"] = "127.0.0.1"
    target["listen_port"] = PROBE_PORT
    if "set_system_proxy" in target:
        target.pop("set_system_proxy", None)
        transformations.append("removed inbound.set_system_proxy")
    config["inbounds"] = [target]
    transformations.extend(
        [
            f"reduced inbounds from {len(inbounds)} to one",
            f"forced Mixed listener to 127.0.0.1:{PROBE_PORT}",
        ]
    )

    if config.pop("experimental", None) is not None:
        transformations.append("removed experimental")
    if config.pop("services", None) is not None:
        transformations.append("removed services")
    ntp = config.get("ntp")
    if isinstance(ntp, dict) and ntp.get("write_to_system") is True:
        raise SafetyError("refusing ntp.write_to_system=true")
    if config.pop("ntp", None) is not None:
        transformations.append("removed ntp service")
    endpoints = config.get("endpoints")
    if endpoints not in (None, []):
        raise SafetyError("refusing a config with top-level endpoints")
    config.pop("endpoints", None)

    log = config.get("log")
    if log is None:
        log = {}
        config["log"] = log
        transformations.append("created diagnostic log settings")
    log = _require_object(log, "log")
    log["disabled"] = False
    log["level"] = "debug"
    log["output"] = "core.log"
    log["timestamp"] = True
    transformations.append("forced temporary debug logging to the probe directory")

    outbounds = _require_object_list(config.get("outbounds"), "outbounds")
    selected = [item for item in outbounds if item.get("tag") == expected_outbound_tag]
    if len(selected) != 1:
        raise ProbeError(
            f"expected exactly one outbound tagged {expected_outbound_tag!r}, found {len(selected)}"
        )
    outbound = selected[0]
    forbidden_dial = _find_forbidden_key(
        outbound,
        {"bind_interface", "inet4_bind_address", "inet6_bind_address", "routing_mark"},
        f"$.outbounds[{expected_outbound_tag}]",
    )
    if forbidden_dial:
        raise SafetyError(
            f"remote probe cannot translate platform-specific dial field {forbidden_dial}"
        )
    if (remove_detour or anytls_client != "preserve") and outbound.get("type") != "anytls":
        raise ProbeError("AnyTLS overrides require the selected outbound to be AnyTLS")
    if remove_detour and "detour" in outbound:
        outbound.pop("detour", None)
        transformations.append("removed selected AnyTLS detour")
    if anytls_client == "native":
        outbound.pop("client", None)
        transformations.append("forced native AnyTLS client")
    elif anytls_client == "mihomo":
        outbound["client"] = "mihomo/1.19.28"
        transformations.append("forced mihomo AnyTLS client")

    outbound_closure = _strict_outbound_closure(outbounds, expected_outbound_tag)
    config["outbounds"] = outbound_closure
    transformations.append(
        f"reduced outbounds from {len(outbounds)} to the exact selected detour closure ({len(outbound_closure)})"
    )

    route = config.get("route")
    if route is None:
        route = {}
        config["route"] = route
    route = _require_object(route, "route")
    if route.get("final") != expected_outbound_tag:
        raise SafetyError(
            "source config does not map its default/main route to "
            f"{expected_outbound_tag!r}; refusing to invent a different product mapping"
        )
    if route.get("default_mark") not in (None, 0):
        raise SafetyError("refusing a source config with a non-zero route.default_mark")
    route.pop("default_mark", None)
    if "default_interface" in route:
        route.pop("default_interface", None)
        transformations.append("removed Windows route.default_interface")
    if force_auto_detect_interface:
        route["auto_detect_interface"] = True
        transformations.append(
            "diagnostic override: forced Linux auto interface detection"
        )
    route.pop("geoip", None)
    route.pop("geosite", None)
    route.pop("rule_set", None)
    route["rules"] = [
        {"inbound": [inbound_tag], "action": "sniff"},
        {
            "inbound": [inbound_tag],
            "action": "route",
            "outbound": expected_outbound_tag,
        },
    ]
    route["final"] = expected_outbound_tag
    transformations.append("replaced route rules with Mixed -> selected outbound only")
    transformations.append("removed legacy geo/rule-set assets")

    users = target.get("users", [])
    if users is None:
        users = []
    user_objects = _require_object_list(users, f"inbound {inbound_tag}.users")
    credentials = Credentials()
    if user_objects:
        first = user_objects[0]
        username = first.get("username", "")
        password = first.get("password", "")
        if not isinstance(username, str) or not isinstance(password, str):
            raise ProbeError("Mixed inbound credentials must be strings")
        if re.search(r"[\x00\r\n]", username + password):
            raise ProbeError("Mixed inbound credentials contain unsupported control characters")
        credentials = Credentials(username=username, password=password)

    sanitized_raw = _json_bytes(config)
    return TightenedConfig(
        data=config,
        credentials=credentials,
        source_sha256=hashlib.sha256(source_raw).hexdigest(),
        sanitized_sha256=hashlib.sha256(sanitized_raw).hexdigest(),
        source_inbound_count=len(inbounds),
        expected_outbound_tag=expected_outbound_tag,
        expected_outbound_type=str(outbound.get("type", "")),
        expected_outbound_has_detour=bool(outbound.get("detour")),
        transformations=transformations,
    )


def parse_env_file(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as exc:
        raise ProbeError(f"cannot read ignored lab credential file: {path}: {exc}") from exc
    result: dict[str, str] = {}
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        key, separator, value = stripped.partition("=")
        if separator and key.strip():
            result[key.strip()] = value.strip()
    return result


def _sha256_host_key(key: Any) -> str:
    return base64.b64encode(hashlib.sha256(key.asbytes()).digest()).decode("ascii").rstrip("=")


def validate_host_key(key: Any) -> None:
    algorithm = key.get_name()
    fingerprint = _sha256_host_key(key)
    if EXPECTED_HOST_KEYS_SHA256.get(algorithm) != fingerprint:
        raise SafetyError(
            f"OpenWrt SSH host key does not match the audited lab host ({algorithm})"
        )


class LabSSH:
    def __init__(self, env_path: Path, key_path: Path):
        self.env_path = env_path
        self.key_path = key_path
        self.transport: Any = None
        self._socket: socket.socket | None = None
        self._lock_streams: tuple[Any, Any, Any] | None = None

    def connect(self) -> None:
        try:
            import paramiko
        except ImportError as exc:
            raise ProbeError("Paramiko is required for a real probe: python -m pip install paramiko") from exc

        env = parse_env_file(self.env_path)
        configured_host = os.environ.get(
            "ROUTEFLUENT_DEVICE_HOST", env.get("ROUTEFLUENT_DEVICE_HOST", LAB_HOST)
        )
        configured_port = os.environ.get(
            "ROUTEFLUENT_DEVICE_PORT", env.get("ROUTEFLUENT_DEVICE_PORT", str(LAB_PORT))
        )
        configured_user = os.environ.get(
            "ROUTEFLUENT_DEVICE_USER", env.get("ROUTEFLUENT_DEVICE_USER", LAB_USER)
        )
        if configured_host != LAB_HOST or configured_port != str(LAB_PORT) or configured_user != LAB_USER:
            raise SafetyError(
                f"lab credentials must resolve exactly to {LAB_USER}@{LAB_HOST}:{LAB_PORT}"
            )
        password = os.environ.get(
            "ROUTEFLUENT_DEVICE_PASSWORD", env.get("ROUTEFLUENT_DEVICE_PASSWORD", "")
        )
        transport: Any = None
        try:
            self._socket = socket.create_connection((LAB_HOST, LAB_PORT), timeout=15)
            transport = paramiko.Transport(self._socket)
            transport.banner_timeout = 30
            transport.start_client(timeout=30)
            validate_host_key(transport.get_remote_server_key())
            if password:
                try:
                    transport.auth_password(LAB_USER, password)
                except paramiko.BadAuthenticationType as exc:
                    if "keyboard-interactive" not in getattr(exc, "allowed_types", []):
                        raise

                    def handler(
                        _title: str, _instructions: str, prompts: Iterable[Any]
                    ) -> list[str]:
                        return [password for _prompt, _echo in prompts]

                    transport.auth_interactive(LAB_USER, handler)
            else:
                if not self.key_path.is_file():
                    raise ProbeError(f"lab private key not found: {self.key_path}")
                key = paramiko.Ed25519Key.from_private_key_file(str(self.key_path))
                transport.auth_publickey(LAB_USER, key)
            if not transport.is_authenticated():
                raise ProbeError("OpenWrt SSH authentication failed")
            transport.set_keepalive(15)
            self.transport = transport
        except Exception:
            if transport is not None:
                transport.close()
            if self._socket is not None:
                self._socket.close()
                self._socket = None
            raise

    def close(self) -> None:
        try:
            self.release_lock()
        finally:
            if self.transport is not None:
                self.transport.close()
                self.transport = None
            if self._socket is not None:
                self._socket.close()
                self._socket = None

    def run(self, command: str, timeout: int = 120, stdin_data: bytes = b"") -> CommandResult:
        if self.transport is None or not self.transport.is_active():
            raise SafetyError("SSH transport is not active")
        channel = self.transport.open_session()
        channel.settimeout(1)
        channel.exec_command(command)
        if stdin_data:
            channel.sendall(stdin_data)
        channel.shutdown_write()
        stdout_chunks: list[bytes] = []
        stderr_chunks: list[bytes] = []
        stdout_size = 0
        stderr_size = 0
        deadline = time.monotonic() + timeout
        while True:
            progressed = False
            while channel.recv_ready():
                chunk = channel.recv(32768)
                stdout_chunks.append(chunk)
                stdout_size += len(chunk)
                progressed = True
            while channel.recv_stderr_ready():
                chunk = channel.recv_stderr(32768)
                stderr_chunks.append(chunk)
                stderr_size += len(chunk)
                progressed = True
            if stdout_size > MAX_REMOTE_OUTPUT or stderr_size > MAX_REMOTE_OUTPUT:
                channel.close()
                raise SafetyError("remote command output exceeded the safety limit")
            if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                break
            if time.monotonic() >= deadline:
                channel.close()
                raise ProbeError(f"remote command timed out after {timeout} seconds")
            if not progressed:
                time.sleep(0.02)
        exit_code = channel.recv_exit_status()
        channel.close()
        stdout = b"".join(stdout_chunks)
        stderr = b"".join(stderr_chunks)
        return CommandResult(
            stdout=stdout.decode("utf-8", errors="replace"),
            stderr=stderr.decode("utf-8", errors="replace"),
            exit_code=exit_code,
        )

    def acquire_lock(self) -> None:
        if self.transport is None or not self.transport.is_active():
            raise SafetyError("SSH transport is not active")
        command = (
            f"exec 9>{shlex.quote(REMOTE_LOCK)}; "
            "flock -n 9 || exit 75; "
            "printf 'LOCKED\\n'; cat >/dev/null"
        )
        channel = self.transport.open_session()
        channel.settimeout(15)
        channel.exec_command(command)
        stdin = channel.makefile("wb")
        stdout = channel.makefile("rb")
        stderr = channel.makefile_stderr("rb")
        marker = stdout.readline().decode("utf-8", errors="replace").strip()
        if marker != "LOCKED":
            detail = stderr.read(4096).decode("utf-8", errors="replace").strip()
            channel.close()
            raise SafetyError(f"another Mixed OpenWrt probe holds the lab lock: {detail}")
        self._lock_streams = (stdin, stdout, stderr)

    def release_lock(self) -> None:
        if self._lock_streams is None:
            return
        stdin, stdout, stderr = self._lock_streams
        self._lock_streams = None
        try:
            stdin.close()
        finally:
            try:
                stdout.close()
            finally:
                stderr.close()

    def upload_bytes(self, remote_path: str, payload: bytes) -> None:
        quoted = shlex.quote(remote_path)
        result = self.run(f"umask 077; cat > {quoted}", timeout=120, stdin_data=payload)
        if result.exit_code != 0:
            raise ProbeError(f"remote config upload failed: {sanitize_text(result.stderr)}")


def _parse_sha256(output: str, path: str) -> str:
    for line in output.splitlines():
        pieces = line.strip().split()
        if len(pieces) >= 2 and pieces[-1] == path and re.fullmatch(r"[0-9a-fA-F]{64}", pieces[0]):
            return pieces[0].lower()
    return "MISSING"


def _active_process_command() -> str:
    prefix = f"{REMOTE_CORE} run -c {REMOTE_ACTIVE_CONFIG}"
    return (
        "for proc in /proc/[0-9]*; do "
        "pid=${proc##*/}; "
        "exe=$(readlink -f \"$proc/exe\" 2>/dev/null || true); "
        f"[ \"$exe\" = {shlex.quote(REMOTE_CORE)} ] || continue; "
        "cmd=$(tr '\\000' ' ' < \"$proc/cmdline\" 2>/dev/null || true); "
        f"case \"$cmd\" in {shlex.quote(prefix + ' ')}*) ;; *) continue;; esac; "
        "start=$(awk '{print $22}' \"$proc/stat\" 2>/dev/null || true); "
        "printf 'PROC\\t%s\\t%s\\t%s\\t%s\\n' \"$pid\" \"$start\" \"$exe\" \"$cmd\"; "
        "done"
    )


def _parse_processes(output: str) -> tuple[ProcessIdentity, ...]:
    processes: list[ProcessIdentity] = []
    for line in output.splitlines():
        if not line.startswith("PROC\t"):
            continue
        pieces = line.split("\t", 4)
        if len(pieces) != 5 or not pieces[1].isdigit() or not pieces[2].isdigit():
            raise SafetyError("malformed active process snapshot")
        processes.append(
            ProcessIdentity(
                pid=int(pieces[1]),
                start_ticks=pieces[2],
                executable=pieces[3],
                command_line=pieces[4],
            )
        )
    return tuple(sorted(processes, key=lambda item: item.pid))


def take_baseline(remote: LabSSH) -> BaselineSnapshot:
    process_result = remote.run(_active_process_command())
    if process_result.exit_code != 0:
        raise SafetyError("cannot enumerate the active RouteFluent core")
    processes = _parse_processes(process_result.stdout)
    hash_result = remote.run(
        "sha256sum "
        + " ".join(
            shlex.quote(path)
            for path in (REMOTE_ACTIVE_CONFIG, REMOTE_ACTIVE_MANIFEST, REMOTE_CORE)
        )
        + " 2>/dev/null || true"
    )
    version_result = remote.run(f"{shlex.quote(REMOTE_CORE)} version 2>&1 | head -n 1")
    if version_result.exit_code != 0:
        raise SafetyError("cannot read the remote core version")
    listener_lines: list[str] = []
    for process in processes:
        result = remote.run(
            "netstat -lntup 2>/dev/null | "
            + f"awk -v owner={shlex.quote(str(process.pid) + '/')} "
            + "'$NF ~ (\"^\" owner) {print}' | sort"
        )
        if result.exit_code != 0:
            raise SafetyError("cannot snapshot active core listeners")
        listener_lines.extend(line.strip() for line in result.stdout.splitlines() if line.strip())
    return BaselineSnapshot(
        active_processes=processes,
        active_config_sha256=_parse_sha256(hash_result.stdout, REMOTE_ACTIVE_CONFIG),
        active_manifest_sha256=_parse_sha256(hash_result.stdout, REMOTE_ACTIVE_MANIFEST),
        core_sha256=_parse_sha256(hash_result.stdout, REMOTE_CORE),
        core_version=version_result.stdout.strip(),
        active_listeners=tuple(sorted(listener_lines)),
    )


def validate_remote_core(snapshot: BaselineSnapshot) -> None:
    if len(snapshot.active_processes) != 1:
        raise SafetyError(
            "expected exactly one active RouteFluent sing-box process before probing"
        )
    if snapshot.active_config_sha256 == "MISSING":
        raise SafetyError("active RouteFluent config hash is unavailable")
    if snapshot.active_manifest_sha256 == "MISSING":
        raise SafetyError("active RouteFluent manifest hash is unavailable")
    if not snapshot.active_listeners:
        raise SafetyError("active RouteFluent core has no auditable listeners")
    if EXPECTED_REMOTE_CORE_VERSION not in snapshot.core_version:
        raise SafetyError(
            "remote core version drifted from the audited RouteFluent/NekoRay source build"
        )
    if snapshot.core_sha256 != EXPECTED_REMOTE_CORE_SHA256:
        raise SafetyError("remote core SHA256 drifted from the audited Linux artifact")


def assert_probe_port_free(remote: LabSSH) -> None:
    result = remote.run(
        "netstat -lnt 2>/dev/null | "
        + f"awk -v needle={shlex.quote(':' + str(PROBE_PORT))} "
        + "'$4 ~ (needle \"$\") {found=1} END {exit found ? 0 : 1}'"
    )
    if result.exit_code == 0:
        raise SafetyError(f"fixed probe port {PROBE_PORT} is already listening")
    if result.exit_code != 1:
        raise SafetyError("could not determine whether the fixed probe port is free")


def create_remote_dir(remote: LabSSH, remote_dir: str) -> None:
    if not re.fullmatch(r"/tmp/nekoray-mixed-probe-[0-9TZ-]+-[0-9a-f]{12}", remote_dir):
        raise SafetyError("generated remote directory did not match the exact safety prefix")
    quoted = shlex.quote(remote_dir)
    result = remote.run(
        f"umask 077; test ! -e {quoted} && mkdir {quoted} && chmod 700 {quoted}"
    )
    if result.exit_code != 0:
        raise SafetyError("could not create the exact remote probe directory")


def finalize_upload(remote: LabSSH, remote_dir: str, expected_sha256: str) -> None:
    partial = f"{remote_dir}/config.json.partial"
    final = f"{remote_dir}/config.json"
    result = remote.run(
        f"test -f {shlex.quote(partial)} && test ! -L {shlex.quote(partial)} && "
        f"sha256sum {shlex.quote(partial)} && mv {shlex.quote(partial)} {shlex.quote(final)} && "
        f"chmod 600 {shlex.quote(final)}"
    )
    if result.exit_code != 0:
        raise SafetyError("remote config verification/rename failed")
    actual = _parse_sha256(result.stdout, partial)
    if actual != expected_sha256:
        raise SafetyError("remote config SHA256 does not match the local sanitized config")


def run_remote_check(remote: LabSSH, remote_dir: str) -> None:
    config_path = f"{remote_dir}/config.json"
    result = remote.run(
        f"cd {shlex.quote(remote_dir)} && {shlex.quote(REMOTE_CORE)} check -c "
        f"{shlex.quote(config_path)}",
        timeout=90,
    )
    if result.exit_code != 0:
        detail = sanitize_text((result.stderr or result.stdout)[-4000:])
        raise ProbeError(f"remote sing-box check failed: {detail}")


def _read_identity(remote: LabSSH, pid: int) -> ProcessIdentity | None:
    result = remote.run(
        f"proc=/proc/{pid}; test -d \"$proc\" || exit 3; "
        "exe=$(readlink -f \"$proc/exe\" 2>/dev/null || true); "
        "cmd=$(tr '\\000' ' ' < \"$proc/cmdline\" 2>/dev/null || true); "
        "start=$(awk '{print $22}' \"$proc/stat\" 2>/dev/null || true); "
        "printf 'PROC\\t%s\\t%s\\t%s\\t%s\\n' "
        f"{pid} \"$start\" \"$exe\" \"$cmd\""
    )
    if result.exit_code == 3:
        return None
    if result.exit_code != 0:
        raise SafetyError(f"could not inspect probe PID {pid}")
    parsed = _parse_processes(result.stdout)
    if len(parsed) != 1:
        raise SafetyError(f"could not parse probe PID {pid} identity")
    return parsed[0]


def start_probe_core(remote: LabSSH, remote_dir: str) -> ProcessIdentity:
    config_path = f"{remote_dir}/config.json"
    pid_path = f"{remote_dir}/core.pid"
    result = remote.run(
        f"set -eu; cd {shlex.quote(remote_dir)}; umask 077; "
        f"/sbin/start-stop-daemon -S -b -m -p {shlex.quote(pid_path)} "
        f"-x {shlex.quote(REMOTE_CORE)} -- run -c {shlex.quote(config_path)} "
        f"</dev/null >/dev/null 2>&1; "
        f"cat {shlex.quote(pid_path)}"
    )
    raw_pid = result.stdout.strip()
    if result.exit_code != 0 or not raw_pid.isdigit():
        raise SafetyError("remote core did not return one exact PID")
    identity = _read_identity(remote, int(raw_pid))
    if identity is None:
        raise ProbeError("remote core exited before identity verification")
    expected_command = f"{REMOTE_CORE} run -c {config_path} "
    if identity.executable != REMOTE_CORE or identity.command_line != expected_command:
        raise SafetyError("remote core PID identity does not match the exact probe command")
    return identity


def recover_probe_identity(remote: LabSSH, remote_dir: str) -> ProcessIdentity | None:
    pid_path = f"{remote_dir}/core.pid"
    result = remote.run(f"cat {shlex.quote(pid_path)} 2>/dev/null || true")
    raw_pid = result.stdout.strip()
    if not raw_pid:
        return None
    if not raw_pid.isdigit():
        raise SafetyError("remote probe PID file is malformed")
    identity = _read_identity(remote, int(raw_pid))
    if identity is None:
        return None
    expected_command = f"{REMOTE_CORE} run -c {remote_dir}/config.json "
    if identity.executable != REMOTE_CORE or identity.command_line != expected_command:
        raise SafetyError("recovered PID does not match the exact probe command")
    return identity


def wait_for_listener(remote: LabSSH, identity: ProcessIdentity, timeout: int = 10) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = _read_identity(remote, identity.pid)
        if current is None:
            raise ProbeError("remote core exited before opening the Mixed listener")
        if current != identity:
            raise SafetyError("remote probe PID identity changed before listener readiness")
        result = remote.run(
            "netstat -lntp 2>/dev/null | "
            + f"awk -v needle={shlex.quote('127.0.0.1:' + str(PROBE_PORT))} "
            + "'$4 == needle {print $7}'"
        )
        owners = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        if owners:
            if len(owners) == 1 and owners[0].startswith(f"{identity.pid}/"):
                return
            raise SafetyError("fixed probe port is not owned only by the exact probe PID")
        time.sleep(0.2)
    raise ProbeError("remote core did not open the fixed Mixed listener in time")


def _curl_config(credentials: Credentials) -> bytes:
    if not credentials.present:
        return b""
    value = f"{credentials.username}:{credentials.password}"
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'proxy-user = "{escaped}"\n'.encode("utf-8")


def run_curl_probe(
    remote: LabSSH,
    *,
    name: str,
    scheme: str,
    url: str,
    credentials: Credentials,
    timeout_seconds: int,
) -> CurlResult:
    proxy = f"{scheme}://127.0.0.1:{PROBE_PORT}"
    command = (
        "HTTP_PROXY= HTTPS_PROXY= ALL_PROXY= NO_PROXY= "
        "http_proxy= https_proxy= all_proxy= no_proxy= "
        "curl --config - --silent --show-error --fail "
        f"--connect-timeout {min(10, timeout_seconds)} --max-time {timeout_seconds} "
        "--output /dev/null "
        "--write-out 'HTTP_CODE=%{http_code};CONNECT=%{time_connect};TOTAL=%{time_total}' "
        f"--proxy {shlex.quote(proxy)} {shlex.quote(url)}"
    )
    result = remote.run(command, timeout=timeout_seconds + 15, stdin_data=_curl_config(credentials))
    combined = (result.stdout + "\n" + result.stderr).strip()
    match = re.search(r"HTTP_CODE=(\d+)", combined)
    http_code = int(match.group(1)) if match else 0
    return CurlResult(
        name=name,
        exit_code=result.exit_code,
        http_code=http_code,
        success=result.exit_code == 0 and http_code == 204,
        summary=sanitize_text(combined),
    )


def read_remote_log(remote: LabSSH, remote_dir: str) -> str:
    result = remote.run(f"cat {shlex.quote(remote_dir + '/core.log')} 2>/dev/null || true")
    return result.stdout


def log_evidence(log_text: str, inbound_tag: str, outbound_tag: str) -> dict[str, Any]:
    plain = re.sub(r"\x1b\[[0-9;]*m", "", log_text)
    lines = plain.splitlines()
    error_lines = [
        line
        for line in lines
        if re.search(r"\b(?:ERROR|FATAL|panic|failed)\b", line, re.I)
    ]
    return {
        "mixed_events": sum(
            1
            for line in lines
            if re.search(rf"inbound/mixed\[{re.escape(inbound_tag)}\]", line)
        ),
        "selected_outbound_events": sum(
            1
            for line in lines
            if re.search(rf"outbound/[^\[]+\[{re.escape(outbound_tag)}\]", line)
        ),
        "startup_errors": sum(
            1
            for line in lines
            if re.search(r"start service:|initialize inbound|listen tcp.*failed", line, re.I)
        ),
        "anytls_session_errors": sum(
            1
            for line in lines
            if re.search(r"outbound/anytls|failed to create session", line, re.I)
            and re.search(r"ERROR|failed|EOF", line, re.I)
        ),
        "tls_errors": sum(
            1
            for line in lines
            if re.search(r"TLS handshake|handshake failed|remote error: tls|tls:", line, re.I)
        ),
        "tcp_dial_errors": sum(
            1
            for line in lines
            if re.search(
                r"dial tcp|connection refused|connection timed out|i/o timeout|network is unreachable",
                line,
                re.I,
            )
        ),
        "error_lines": len(error_lines),
        "sanitized_error_tail": [sanitize_text(line) for line in error_lines[-8:]],
    }


def sanitize_text(value: str) -> str:
    value = re.sub(r"\x1b\[[0-9;]*m", "", value)
    value = re.sub(r"(?i)\b(?:https?|socks5h?)://[^\s]+", "<url>", value)
    value = re.sub(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F-]{27,}\b", "<uuid>", value)
    value = re.sub(r"\b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b", "<ip>", value)
    value = re.sub(
        r"\b(?:[A-Za-z0-9-]+\.)+[A-Za-z]{2,}(?::\d+)?\b", "<domain>", value
    )
    return value[-4000:]


def terminate_exact_process(remote: LabSSH, identity: ProcessIdentity) -> None:
    current = _read_identity(remote, identity.pid)
    if current is None:
        return
    if current != identity:
        raise SafetyError("refusing to signal a PID whose identity changed")
    result = remote.run(f"kill -TERM {identity.pid}")
    if result.exit_code != 0:
        raise SafetyError("TERM failed for the exact probe PID")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if _read_identity(remote, identity.pid) is None:
            return
        time.sleep(0.2)
    current = _read_identity(remote, identity.pid)
    if current is None:
        return
    if current != identity:
        raise SafetyError("refusing cleanup after probe PID reuse/identity change")
    result = remote.run(f"kill -KILL {identity.pid}")
    if result.exit_code != 0:
        raise SafetyError("KILL failed for the still-identical probe PID")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if _read_identity(remote, identity.pid) is None:
            return
        time.sleep(0.2)
    raise SafetyError("exact probe PID remained alive after cleanup")


def assert_probe_port_gone(remote: LabSSH) -> None:
    result = remote.run(
        "netstat -lnt 2>/dev/null | "
        + f"awk -v needle={shlex.quote(':' + str(PROBE_PORT))} "
        + "'$4 ~ (needle \"$\") {found=1} END {exit found ? 0 : 1}'"
    )
    if result.exit_code == 0:
        raise SafetyError(f"fixed probe port {PROBE_PORT} is still listening after cleanup")
    if result.exit_code != 1:
        raise SafetyError("could not confirm that the fixed probe port disappeared")


def delete_exact_remote_dir(remote: LabSSH, remote_dir: str) -> None:
    if not re.fullmatch(r"/tmp/nekoray-mixed-probe-[0-9TZ-]+-[0-9a-f]{12}", remote_dir):
        raise SafetyError("refusing cleanup outside the exact remote probe prefix")
    quoted_dir = shlex.quote(remote_dir)
    allowed = ["config.json.partial", "config.json", "core.log", "core.pid"]
    pieces = [
        "set -eu",
        f"test -d {quoted_dir}",
        f"test ! -L {quoted_dir}",
    ]
    for name in allowed:
        path = shlex.quote(f"{remote_dir}/{name}")
        pieces.append(
            f"if [ -e {path} ]; then test -f {path}; test ! -L {path}; rm -f {path}; fi"
        )
    pieces.extend(
        [
            f"test -z \"$(ls -A {quoted_dir} 2>/dev/null)\"",
            f"rmdir {quoted_dir}",
        ]
    )
    result = remote.run("; ".join(pieces))
    if result.exit_code != 0:
        raise SafetyError(
            "exact remote directory cleanup failed; it was retained for manual inspection"
        )


def _snapshot_public(snapshot: BaselineSnapshot) -> dict[str, Any]:
    return {
        "active_process_count": len(snapshot.active_processes),
        "active_process_ids": [item.pid for item in snapshot.active_processes],
        "active_config_sha256": snapshot.active_config_sha256,
        "active_manifest_sha256": snapshot.active_manifest_sha256,
        "core_sha256": snapshot.core_sha256,
        "core_version": snapshot.core_version,
        "active_listeners": list(snapshot.active_listeners),
    }


def run_probe(args: argparse.Namespace, tightened: TightenedConfig) -> tuple[dict[str, Any], int]:
    remote = LabSSH(args.remote_env, args.private_key)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    remote_dir = f"{REMOTE_DIR_PREFIX}{timestamp}-{secrets.token_hex(6)}"
    baseline: BaselineSnapshot | None = None
    after: BaselineSnapshot | None = None
    identity: ProcessIdentity | None = None
    directory_created = False
    directory_cleaned = False
    cleanup_errors: list[str] = []
    probes: list[CurlResult] = []
    evidence: dict[str, Any] = {}
    failure = ""

    try:
        remote.connect()
        remote.acquire_lock()
        baseline = take_baseline(remote)
        validate_remote_core(baseline)
        assert_probe_port_free(remote)
        create_remote_dir(remote, remote_dir)
        directory_created = True
        payload = _json_bytes(tightened.data)
        remote.upload_bytes(f"{remote_dir}/config.json.partial", payload)
        finalize_upload(remote, remote_dir, tightened.sanitized_sha256)
        run_remote_check(remote, remote_dir)
        identity = start_probe_core(remote, remote_dir)
        if baseline and any(item.pid == identity.pid for item in baseline.active_processes):
            raise SafetyError("probe PID unexpectedly overlaps the active RouteFluent core")
        wait_for_listener(remote, identity)
        probes = [
            run_curl_probe(
                remote,
                name="http_absolute_form",
                scheme="http",
                url=args.http_url,
                credentials=tightened.credentials,
                timeout_seconds=args.timeout,
            ),
            run_curl_probe(
                remote,
                name="https_connect",
                scheme="http",
                url=args.https_url,
                credentials=tightened.credentials,
                timeout_seconds=args.timeout,
            ),
            run_curl_probe(
                remote,
                name="socks5h",
                scheme="socks5h",
                url=args.http_url,
                credentials=tightened.credentials,
                timeout_seconds=args.timeout,
            ),
        ]
        current = _read_identity(remote, identity.pid)
        if current != identity:
            raise SafetyError("probe core did not remain the same process after requests")
        evidence = log_evidence(
            read_remote_log(remote, remote_dir), args.inbound_tag, args.expected_outbound_tag
        )
        if not all(item.success for item in probes):
            raise ProbeError("one or more Mixed proxy protocols did not return exact HTTP 204")
        if evidence.get("mixed_events", 0) < 3:
            raise ProbeError("core log did not prove all three requests entered the Mixed inbound")
        if evidence.get("selected_outbound_events", 0) < 1:
            raise ProbeError("core log did not prove the selected outbound was used")
        if evidence.get("startup_errors", 0) > 0:
            raise ProbeError("core log contains a startup error")
    except Exception as exc:
        failure = sanitize_text(str(exc))
        if directory_created and remote.transport is not None and remote.transport.is_active():
            try:
                evidence = log_evidence(
                    read_remote_log(remote, remote_dir),
                    args.inbound_tag,
                    args.expected_outbound_tag,
                )
            except Exception:
                # Cleanup and production-baseline verification take priority over
                # optional diagnostic evidence.
                pass
    finally:
        if remote.transport is not None and remote.transport.is_active():
            if identity is None and directory_created:
                try:
                    identity = recover_probe_identity(remote, remote_dir)
                except Exception as exc:
                    cleanup_errors.append(sanitize_text(str(exc)))
            if identity is not None:
                try:
                    terminate_exact_process(remote, identity)
                except Exception as exc:
                    cleanup_errors.append(sanitize_text(str(exc)))
            try:
                assert_probe_port_gone(remote)
            except Exception as exc:
                cleanup_errors.append(sanitize_text(str(exc)))
            if directory_created and not cleanup_errors:
                try:
                    delete_exact_remote_dir(remote, remote_dir)
                    directory_cleaned = True
                except Exception as exc:
                    cleanup_errors.append(sanitize_text(str(exc)))
            try:
                after = take_baseline(remote)
                if baseline is not None and after != baseline:
                    cleanup_errors.append("active RouteFluent PID/cmdline/hash/listeners changed")
            except Exception as exc:
                cleanup_errors.append(sanitize_text(str(exc)))
        elif directory_created or identity is not None:
            cleanup_errors.append("SSH transport unavailable; remote cleanup was not proven")
        remote.close()

    success = not failure and not cleanup_errors
    result = {
        "schema": "nekoray.mixed_openwrt_probe.v1",
        "success": success,
        "target": f"{LAB_HOST}:{LAB_PORT}",
        "remote_directory_cleaned": (not directory_created) or directory_cleaned,
        "retained_remote_directory": remote_dir if directory_created and not directory_cleaned else "",
        "fixed_probe_port": PROBE_PORT,
        "config": tightened.public_summary(),
        "baseline": _snapshot_public(baseline) if baseline else None,
        "after": _snapshot_public(after) if after else None,
        "baseline_unchanged": baseline is not None and after == baseline,
        "probe_pid": identity.pid if identity else None,
        "protocols": [dataclasses.asdict(item) for item in probes],
        "evidence": evidence,
        "failure": failure,
        "cleanup_errors": cleanup_errors,
    }
    return result, 0 if success else (2 if cleanup_errors else 1)


def default_routefluent_root() -> Path:
    return Path(r"D:\complex\RouteFluent")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    root = default_routefluent_root()
    parser = argparse.ArgumentParser(
        description=(
            "Run a side-effect-constrained Mixed HTTP/CONNECT/SOCKS5h probe on the "
            "fixed OpenWrt lab host. The active RouteFluent runtime is never signalled."
        )
    )
    parser.add_argument("config", type=Path, help="Exported NekoRay sing-box JSON config.")
    parser.add_argument("--inbound-tag", default="mixed-in")
    parser.add_argument("--expected-outbound-tag", default="proxy")
    parser.add_argument("--remove-detour", action="store_true")
    parser.add_argument(
        "--anytls-client", choices=["preserve", "native", "mihomo"], default="preserve"
    )
    parser.add_argument(
        "--force-auto-detect-interface",
        action="store_true",
        help=(
            "Diagnostic-only override for the temporary OpenWrt config. By default "
            "route.auto_detect_interface is preserved exactly from the exported config."
        ),
    )
    parser.add_argument("--http-url", default=DEFAULT_HTTP_URL)
    parser.add_argument("--https-url", default=DEFAULT_HTTPS_URL)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument(
        "--remote-env", type=Path, default=root / "scripts" / "remote.env"
    )
    parser.add_argument(
        "--private-key", type=Path, default=root / "19203" / "19203_client"
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only validate/tighten locally and print a secret-free summary.",
    )
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON.")
    args = parser.parse_args(argv)
    if args.timeout < 5 or args.timeout > 120:
        parser.error("--timeout must be between 5 and 120 seconds")
    return args


def print_result(result: dict[str, Any], as_json: bool) -> None:
    if as_json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    print(f"Result: {'PASS' if result.get('success') else 'FAIL'}")
    if result.get("dry_run"):
        print("Mode: local dry-run (no SSH or remote writes)")
    else:
        print(f"Target: {result.get('target')}")
        print(f"Baseline unchanged: {result.get('baseline_unchanged')}")
        print(f"Remote directory cleaned: {result.get('remote_directory_cleaned')}")
    config = result.get("config") or {}
    print(
        "Constrained path: "
        f"127.0.0.1:{config.get('listen_port')} -> {config.get('expected_outbound_tag')} "
        f"({config.get('expected_outbound_type')})"
    )
    for protocol in result.get("protocols") or []:
        print(
            f"{protocol['name']}: exit={protocol['exit_code']} "
            f"http={protocol['http_code']} pass={protocol['success']}"
        )
    if result.get("failure"):
        print(f"Failure: {result['failure']}")
    for error in result.get("cleanup_errors") or []:
        print(f"Cleanup failure: {error}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        source, source_raw = load_strict_json(args.config.resolve())
        tightened = tighten_config(
            source,
            source_raw,
            inbound_tag=args.inbound_tag,
            expected_outbound_tag=args.expected_outbound_tag,
            remove_detour=args.remove_detour,
            anytls_client=args.anytls_client,
            force_auto_detect_interface=args.force_auto_detect_interface,
        )
        if args.dry_run:
            result = {
                "schema": "nekoray.mixed_openwrt_probe.v1",
                "success": True,
                "dry_run": True,
                "remote_actions": False,
                "fixed_target": f"{LAB_HOST}:{LAB_PORT}",
                "config": tightened.public_summary(),
                "protocols": [],
                "cleanup_errors": [],
            }
            print_result(result, args.json)
            return 0
        result, exit_code = run_probe(args, tightened)
        print_result(result, args.json)
        return exit_code
    except ProbeError as exc:
        result = {
            "schema": "nekoray.mixed_openwrt_probe.v1",
            "success": False,
            "failure": sanitize_text(str(exc)),
            "cleanup_errors": [],
        }
        print_result(result, args.json)
        return 2 if isinstance(exc, SafetyError) else 1


if __name__ == "__main__":
    raise SystemExit(main())
