"""Development-only integration checks; distributed ShellBot needs no Python."""
import argparse
import base64
import concurrent.futures
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zlib


CHECKS = 0
CLOSE_PROBE = True
CLOSE_FAILURES = 0
SKIPPED = []
EXPECTED_VERSION = "0.1.0-dev.8"


def check(condition, message):
    global CHECKS
    if not condition:
        raise AssertionError(message)
    CHECKS += 1


def request(method, params=None, identifier=1):
    result = {"jsonrpc": "2.0", "id": identifier, "method": method}
    if params is not None:
        result["params"] = params
    return result


def initialize(version="2025-11-25"):
    return request("initialize", {"protocolVersion": version, "capabilities": {},
                                  "clientInfo": {"name": "ShellBot smoke", "version": "1"}})


def modern(method, params=None, identifier=1):
    values = dict(params or {})
    values["_meta"] = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                       "io.modelcontextprotocol/clientCapabilities": {}}
    return request(method, values, identifier)


def owner(executable, data_dir, command, approval_id=None, success=True):
    arguments = [str(executable), "--" + command]
    if approval_id is not None:
        arguments.append(approval_id)
    arguments += ["--data-dir", str(data_dir)]
    result = subprocess.run(arguments, capture_output=True, timeout=10)
    check(result.returncode == (0 if success else 2), "owner utility exit: " + command)
    return json.loads(result.stdout)


def ready(server):
    server.call(initialize())
    server.send({"jsonrpc": "2.0", "method": "notifications/initialized"})


def audit_lines(data_dir):
    files = sorted(data_dir.glob("audit-*.jsonl"))
    check(bool(files), "audit journal exists")
    all_events = []
    for path in files:
        raw = path.read_bytes()
        check(raw.endswith(b"\n"), "audit record ends at a line boundary")
        events = [json.loads(line) for line in raw.decode("utf-8").splitlines()]
        check([event["sequence"] for event in events] == list(range(1, len(events) + 1)), "audit sequence is ordered without gaps")
        check(all(event["version"] == EXPECTED_VERSION and event["schema_version"] == 1 for event in events), "audit format and source version")
        check(all(path.name == "audit-" + event["instance_id"] + ".jsonl" for event in events), "audit instance identity")
        check(all(abs(event["time_unix_ms"] - time.time() * 1000) < 120000 for event in events), "audit UTC wall clock")
        check(all(not ({"text", "token", "authorization", "request", "response", "error"} & event.keys()) for event in events), "audit excludes raw payloads and exception messages")
        all_events.extend(events)
    return all_events


def approval_checks(executable, folder, root, other):
    config = folder / "write-config.json"
    config.write_text(json.dumps({"roots": [{"id": "workspace", "path": str(root), "read_only": False},
                                          {"id": "other", "path": str(other)}]}), encoding="utf-8")
    data_dir = folder / "approval-state"
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(data_dir)])
    try:
        ready(server)
        def write(path="created 中文 & !.txt", text="批准后写入 😀\n", **extra):
            return server.tool("fs_write", workspace_id="workspace", path=path, text=text, **extra)
        check(server.tool("fs_write", workspace_id="other", path="new.txt", text="x")["isError"], "read-only ceiling")
        for path in ["../outside.txt", "escape/new.txt", "escape2/new.txt", "file.txt:stream", str(other / "new.txt"), ".", "missing/", " "]:
            check(write(path)["isError"], "write path confinement: " + path)
        for text in ["x" * 65537, "A\0B", "A\x01B"]:
            check(write(text=text)["isError"], "invalid write text")
        check(write(approved=True)["isError"], "client cannot self-approve")
        pending = write()["structuredContent"]
        approval_id = pending["approval_id"]
        check(pending["status"] == "pending" and not (root / "created 中文 & !.txt").exists(), "request has no file side effects")
        check(write()["structuredContent"]["approval_id"] == approval_id, "pending approval deduplication")
        inspected = owner(executable, data_dir, "inspect", approval_id)
        check(inspected["text"] == "批准后写入 😀\n" and inspected["sha256"] == hashlib.sha256(inspected["text"].encode()).hexdigest(), "owner sees exact bound content")
        check(owner(executable, data_dir, "list-approvals")["approvals"][0]["approval_id"] == approval_id, "owner list")
        check(write(approval_id=approval_id)["structuredContent"]["status"] == "pending", "cannot run pending approval")
        owner(executable, data_dir, "approve", approval_id)
        check(write()["structuredContent"]["status"] == "approved" and not (root / "created 中文 & !.txt").exists(), "id required for execution")
        check(write(text="changed", approval_id=approval_id)["isError"], "approval binds text")
        check(write(path="different.txt", approval_id=approval_id)["isError"], "approval binds target")
        completed = write(approval_id=approval_id)
        check(not completed["isError"] and completed["structuredContent"]["status"] == "completed", "approved create")
        check((root / "created 中文 & !.txt").read_bytes() == "批准后写入 😀\n".encode(), "exact UTF-8 write bytes")
        check(write(approval_id=approval_id)["structuredContent"]["status"] == "completed", "completed retry cached")
        owner(executable, data_dir, "revoke", approval_id, success=False)
        denied_id = write("denied.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "deny", denied_id)
        check(write("denied.txt", approval_id=denied_id)["structuredContent"]["status"] == "denied", "denial enforced")
        check(not (root / "denied.txt").exists(), "denied write has no side effects")
        revoked_id = write("revoked.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", revoked_id)
        owner(executable, data_dir, "revoke", revoked_id)
        check(write("revoked.txt", approval_id=revoked_id)["isError"] and not (root / "revoked.txt").exists(), "revoke approved write")
        race_id = write("race.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", race_id)
        (root / "race.txt").write_bytes(b"external")
        check(write("race.txt", approval_id=race_id)["structuredContent"]["status"] == "failed", "no-replace commit")
        check((root / "race.txt").read_bytes() == b"external" and not list(root.glob(".shellbot-*.tmp")), "existing file preserved and temp cleaned")
        (root / "replace-parent").mkdir()
        replaced_id = write("replace-parent/new.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", replaced_id)
        (root / "replace-parent").rename(root / "old-parent")
        (root / "replace-parent").mkdir()
        check(write("replace-parent/new.txt", approval_id=replaced_id)["isError"], "approval binds physical parent identity")
        empty_id = write("empty.txt", "")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", empty_id)
        check(not write("empty.txt", "", approval_id=empty_id)["isError"] and (root / "empty.txt").stat().st_size == 0, "empty file creation")
        restart_id = write("restart.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", restart_id)
        check(not (other / "new.txt").exists(), "outside directory unchanged")
    finally:
        server.close()
    logs_before_restart = {path: path.read_bytes() for path in data_dir.glob("audit-*.jsonl")}
    events = audit_lines(data_dir)
    selected = [event for event in events if event.get("approval_id") == approval_id]
    check([event["event"] for event in selected] == ["approval_requested", "approval_approved", "write_started", "write_completed"], "approval/write audit chain exactly once")
    check(selected[0]["actor"] == "stdio" and selected[1]["actor"] == "local-owner", "audit actors are transport and owner identities")
    check(selected[-1]["sha256"] == hashlib.sha256("批准后写入 😀\n".encode()).hexdigest(), "audit content digest matches created file")
    check(all("批准后写入" not in raw.decode() for raw in logs_before_restart.values()), "audit never stores write text")
    check({"approval_denied", "approval_revoked", "write_failed"} <= {event["event"] for event in events}, "audit denial, revoke and actual write failure")
    check(events[0]["event"] == "instance_opened" and events[-1]["event"] == "instance_closed", "audit clean lifecycle")
    # A crash tail from a previous instance must never be opened for append or erased.
    torn_log = data_dir / "audit-00000000000000000000000000000000.jsonl"
    torn_log.write_bytes(b'{"event":')
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(data_dir)])
    try:
        ready(server)
        check(server.tool("fs_write", workspace_id="workspace", path="restart.txt", text="批准后写入 😀\n",
                          approval_id=restart_id)["isError"], "restart revokes prior approvals")
        check(not (root / "restart.txt").exists(), "no restart replay")
    finally:
        server.close()
    check(all(path.read_bytes() == raw for path, raw in logs_before_restart.items()), "restart preserves previous audit bytes")
    check(torn_log.read_bytes() == b'{"event":', "restart preserves crash-tail evidence")
    check(len(list(data_dir.glob("audit-*.jsonl"))) == len(logs_before_restart) + 2, "restart creates a separate audit journal")
    config.write_text(json.dumps({"roots": [{"id": "workspace", "path": str(root), "read_only": False}],
                                  "approval_ttl_seconds": 1}), encoding="utf-8")
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(data_dir)])
    try:
        ready(server)
        for i in range(64):
            data = server.tool("fs_write", workspace_id="workspace", path=f"queue-{i}.txt", text="x")["structuredContent"]
            if i == 0:
                expired_id = data["approval_id"]
                owner(executable, data_dir, "approve", expired_id)
        check(server.tool("fs_write", workspace_id="workspace", path="queue-full.txt", text="x")["isError"], "bounded approval queue")
        time.sleep(1.05)
        check(server.tool("fs_write", workspace_id="workspace", path="queue-0.txt", text="x", approval_id=expired_id)["structuredContent"]["status"] == "expired", "approved request expiry")
        check(not (root / "queue-0.txt").exists(), "expired approval no write")
    finally:
        server.close()
    newest = max((path for path in data_dir.glob("audit-*.jsonl") if path != torn_log), key=lambda path: path.stat().st_mtime_ns)
    check(any(json.loads(line)["event"] == "approval_expired" for line in newest.read_text(encoding="utf-8").splitlines()), "approval expiry remains in audit")


def modification_checks(executable, folder, root, other):
    state = folder / "modify-state"
    config = folder / "modify-config.json"
    config.write_text(json.dumps({"roots": [{"id": "workspace", "path": str(root), "read_only": False},
                                           {"id": "other", "path": str(other), "read_only": True}]}), encoding="utf-8")
    target = root / "修改 中文 😀.txt"
    original = b"\xef\xbb\xbf" + "旧值\r\n旧值 😀\n尾行\r".encode()
    target.write_bytes(original)
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(state)])
    def patch(**changes):
        args = {"workspace_id": "workspace", "path": target.name, "search": "旧值", "replacement": "新值",
                "expected_sha256": hashlib.sha256(original).hexdigest(), "expected_matches": 2}
        args.update(changes)
        return server.tool("fs_patch", **args)
    try:
        ready(server)
        read = server.tool("fs_read", workspace_id="workspace", path=target.name)["structuredContent"]
        check(read["sha256"] == hashlib.sha256(original).hexdigest(), "read hash covers original bytes and BOM")
        pending = patch()["structuredContent"]
        aid = pending["approval_id"]
        check(pending["status"] == "pending" and target.read_bytes() == original and not list(state.glob("backup-*.bin")), "patch approval has no file or backup side effects")
        check(patch()["structuredContent"]["approval_id"] == aid, "patch approval deduplication")
        check(patch(expected_matches=1)["isError"] and patch(replacement="x" * 65536)["isError"], "patch occurrence and resulting byte limits checked before approval")
        inspected = owner(executable, state, "inspect", aid)
        expected = original.decode().replace("旧值", "新值").encode()
        check(inspected["original_text"].encode() == original and inspected["text"].encode() == expected, "owner inspects original and exact patch")
        check(inspected["original_sha256"] == read["sha256"] and inspected["tool"] == "fs_patch", "owner sees modification identity")
        check(patch(approval_id=aid)["structuredContent"]["status"] == "pending", "pending patch cannot execute")
        owner(executable, state, "approve", aid)
        check(patch(replacement="篡改", approval_id=aid)["isError"], "patch approval binds replacement")
        check(patch(expected_matches=1, approval_id=aid)["isError"], "patch approval binds expected matches")
        identity = target.stat().st_ino
        done = patch(approval_id=aid)["structuredContent"]
        check(done["status"] == "completed" and not done["atomic_save"], "patch completes with explicit save behavior")
        check(target.read_bytes() == expected and target.stat().st_ino == identity, "patch preserves BOM line endings and file identity")
        backup = state / ("backup-" + done["backup_id"] + ".bin")
        check(backup.read_bytes() == original, "patch backup contains exact original bytes")
        check(patch(approval_id=aid)["structuredContent"]["status"] == "completed" and len(list(state.glob("backup-*.bin"))) == 1, "patch cached retry creates no backup or write")
        check(patch()["isError"], "stale base hash rejected")
        for changes in [{"expected_sha256": "x" * 64}, {"expected_sha256": "a" * 63}, {"expected_sha256": None},
                        {"search": ""}, {"replacement": None}, {"expected_matches": 0}, {"expected_matches": 1001},
                        {"expected_matches": True}, {"approved": True}, {"search": "\0"}, {"replacement": "\x01"}]:
            check(patch(**changes)["isError"], "invalid patch arguments")
        for path in ["../outside.txt", "escape/secret.txt", "..", "new.txt", target.name + ":stream"]:
            check(patch(path=path)["isError"], "patch path confinement")
        check(patch(workspace_id="other")["isError"], "patch respects read-only root")
        args = {"workspace_id": "workspace", "path": target.name, "text": "短😀", "overwrite": True,
                "expected_sha256": hashlib.sha256(expected).hexdigest().upper()}
        def overwrite(**changes):
            return server.tool("fs_write", **dict(args, **changes))
        overwrite_id = overwrite()["structuredContent"]["approval_id"]
        owner(executable, state, "approve", overwrite_id)
        replaced = overwrite(approval_id=overwrite_id)["structuredContent"]
        check(replaced["status"] == "completed" and target.read_bytes() == "短😀".encode(), "explicit overwrite truncates original and accepts uppercase hash")
        check(target.stat().st_ino == identity and (state / ("backup-" + replaced["backup_id"] + ".bin")).read_bytes() == expected, "overwrite retains identity and previous bytes")
        check(overwrite(approval_id=overwrite_id)["structuredContent"]["status"] == "completed", "overwrite retry cached after content changed")
        for changes in [{"overwrite": False}, {"overwrite": "true"}, {"expected_sha256": None}, {"text": "x" * 65537}, {"path": "missing.txt"}]:
            check(overwrite(**changes)["isError"], "overwrite requires explicit mode hash existing target and byte limit")
        args["expected_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
        changed_id = overwrite()["structuredContent"]["approval_id"] if args["text"] != target.read_text(encoding="utf-8") else None
        check(changed_id is None and overwrite()["isError"], "no-op overwrite rejected")
        args["text"] = "next"
        conflict_id = overwrite()["structuredContent"]["approval_id"]
        owner(executable, state, "approve", conflict_id)
        target.write_bytes(b"external change")
        conflict = overwrite(approval_id=conflict_id)["structuredContent"]
        check(conflict["status"] == "failed" and target.read_bytes() == b"external change" and "backup_id" not in conflict, "approved overwrite refuses intervening content edit")
        args["expected_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
        identity_id = overwrite()["structuredContent"]["approval_id"]
        owner(executable, state, "approve", identity_id)
        target.rename(root / "old-identity.txt")
        target.write_bytes(b"external change")
        check(overwrite(approval_id=identity_id)["structuredContent"]["status"] == "failed" and target.read_bytes() == b"external change", "same-content file replacement invalidates approval")
        with target.open("rb"):
            check(overwrite()["isError"], "busy file refused without backup")
        target.write_bytes(b"binary\0data")
        args["expected_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
        check(overwrite()["isError"], "binary source rejected")
        target.write_bytes(b"\xff\xfe" + "旧值".encode("utf-16le"))
        args["expected_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
        check(overwrite()["isError"], "UTF-16 modification rejected without conversion")
        target.write_bytes(b"ABCDEFGH")
        part = server.tool("fs_read", workspace_id="workspace", path=target.name, max_bytes=4)["structuredContent"]
        check(part["truncated"] and "sha256" not in part, "partial read does not expose a misleading complete hash")
        hardlink = root / "hard-link.txt"
        try:
            os.link(target, hardlink)
        except OSError as error:
            if error.winerror != 50:
                raise
            SKIPPED.append("hardlink fixture unavailable on development filesystem")
        else:
            args["expected_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
            check(overwrite()["isError"] and hardlink.read_bytes() == b"ABCDEFGH", "hard-linked source rejected")
            hardlink.unlink()
        args["expected_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
        args["text"] = ""
        empty_id = overwrite()["structuredContent"]["approval_id"]
        owner(executable, state, "approve", empty_id)
        check(overwrite(approval_id=empty_id)["structuredContent"]["status"] == "completed" and target.read_bytes() == b"", "empty overwrite truncates entire file")
        check(server.tool("fs_read", workspace_id="workspace", path=target.name)["structuredContent"]["sha256"] == hashlib.sha256(b"").hexdigest(), "empty complete read returns correct hash")
    finally:
        server.close()
    events = audit_lines(state)
    selected = [event for event in events if event.get("approval_id") == aid]
    check([event["event"] for event in selected] == ["approval_requested", "approval_approved", "write_started", "backup_saved", "write_completed"], "patch audit binds backup before writing")
    check(all(event["tool"] == "fs_patch" and event["operation"] == "patch" for event in selected), "patch audit identifies actual operation")
    check(selected[-1]["backup_id"] == done["backup_id"] and selected[-1]["original_sha256"] == hashlib.sha256(original).hexdigest(), "audit records backup and original hash")
    check(all("旧值" not in path.read_text(encoding="utf-8") and "新值" not in path.read_text(encoding="utf-8") for path in state.glob("audit-*.jsonl")), "audit does not contain edit text")


def modification_budget_checks(executable, folder, root):
    config = folder / "modify-budget.json"
    settings = {"roots": [{"id": "workspace", "path": str(root), "read_only": False}], "approval_ttl_seconds": 1}
    config.write_text(json.dumps(settings), encoding="utf-8")
    state = folder / "modify-budget-state"
    target = root / "backup-limit.txt"
    target.write_bytes(b"x")
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(state)])
    try:
        ready(server)
        for number in range(65):
            old = target.read_bytes()
            args = {"workspace_id": "workspace", "path": target.name, "text": str(number), "overwrite": True,
                    "expected_sha256": hashlib.sha256(old).hexdigest()}
            aid = server.tool("fs_write", **args)["structuredContent"]["approval_id"]
            owner(executable, state, "approve", aid)
            done = server.tool("fs_write", **dict(args, approval_id=aid))["structuredContent"]
            if number < 64:
                check(done["status"] == "completed" and target.read_bytes() == str(number).encode(), "bounded backup-backed overwrite")
            else:
                check(done["status"] == "failed" and "64-backup" in done["error"] and target.read_bytes() == old, "backup budget blocks writes before modifying original")
        check(len(list(state.glob("backup-*.bin"))) == 64, "backup storage per instance remains bounded")
    finally:
        server.close()
    settings.pop("approval_ttl_seconds")
    original = b"original edit text"
    target = root / "audit-edit.txt"
    def execute(data_dir):
        target.write_bytes(original)
        server = Stdio(executable, ["--config", str(config), "--data-dir", str(data_dir)])
        try:
            ready(server)
            args = {"workspace_id": "workspace", "path": target.name, "text": "edited", "overwrite": True,
                    "expected_sha256": hashlib.sha256(original).hexdigest()}
            aid = server.tool("fs_write", **args)["structuredContent"]["approval_id"]
            owner(executable, data_dir, "approve", aid)
            for index in range(8):
                check("approval_id" in server.tool("fs_write", workspace_id="workspace", path=f"edit-padding-{index}.txt", text="x")["structuredContent"], "edit audit budget padding")
            result = server.tool("fs_write", **dict(args, approval_id=aid))
            cached = server.tool("fs_write", **dict(args, approval_id=aid))
            return result, cached
        finally:
            server.close()
    config.write_text(json.dumps(settings), encoding="utf-8")
    scout = folder / "edit-audit-scout"
    execute(scout)
    records = next(scout.glob("audit-*.jsonl")).read_bytes().splitlines(keepends=True)
    budget = 0
    for line in records:
        if json.loads(line)["event"] == "backup_saved":
            break
        budget += len(line)
    check(budget >= 4096, "edit budget permits recorded write intent")
    settings["audit_max_bytes"] = budget
    config.write_text(json.dumps(settings), encoding="utf-8")
    final = folder / "edit-audit-final"
    result, cached = execute(final)
    data = result["structuredContent"]
    check(result["isError"] and data["status"] == "failed" and "audit_warning" in data and target.read_bytes() == original, "backup audit failure preserves original")
    check((final / ("backup-" + data["backup_id"] + ".bin")).read_bytes() == original, "private backup remains usable after journal failure")
    check(cached["structuredContent"]["backup_id"] == data["backup_id"] and len(list(final.glob("backup-*.bin"))) == 1, "failed edit retry reuses cached state")


def audit_failure_checks(executable, folder, root):
    data_dir = folder / "audit-budget-state"
    config = folder / "audit-budget.json"
    config.write_text(json.dumps({"roots": [{"id": "workspace", "path": str(root), "read_only": False}],
                                 "audit_max_bytes": 8192}), encoding="utf-8")
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(data_dir)])
    try:
        ready(server)
        def write(path, **extra):
            return server.tool("fs_write", workspace_id="workspace", path=path, text="secret audit marker", **extra)
        completed_id = write("audit-completed.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", completed_id)
        check(not write("audit-completed.txt", approval_id=completed_id)["isError"], "audited create before storage failure")
        approved_id = write("audit-blocked.txt")["structuredContent"]["approval_id"]
        pending_id = write("audit-pending.txt")["structuredContent"]["approval_id"]
        owner(executable, data_dir, "approve", approved_id)
        journal = next(data_dir.glob("audit-*.jsonl"))
        try:
            with journal.open("ab"):
                pass
            writable = True
        except OSError:
            writable = False
        check(not writable, "live audit file cannot be opened for external writes")
        try:
            journal.unlink()
            removable = True
        except OSError:
            removable = False
        check(not removable, "live audit file cannot be deleted")
        # Real file budget exhaustion, without disk/system changes or fault flags.
        for i in range(64):
            data = write(f"audit-fill-{i}.txt")["structuredContent"]
            if "approval_id" not in data:
                break
        check(i < 63, "audit storage budget must actually fail before queue capacity")
        caps = server.tool("host_capabilities")["structuredContent"]
        check(caps["durable_audit"] and not caps["audit_healthy"] and caps["audit_max_bytes"] == 8192, "audit failure is discoverable")
        check(owner(executable, data_dir, "approve", pending_id, success=False).get("error"), "audit failure cannot grant approval")
        check(owner(executable, data_dir, "inspect", pending_id)["status"] == "pending", "failed approval keeps pending state")
        check(write("audit-blocked.txt", approval_id=approved_id)["isError"] and not (root / "audit-blocked.txt").exists(), "audit failure blocks previously approved writes")
        check(write("audit-completed.txt", approval_id=completed_id)["structuredContent"]["status"] == "completed", "cached completed outcome survives audit failure")
        denied = owner(executable, data_dir, "deny", pending_id)
        revoked = owner(executable, data_dir, "revoke", approved_id)
        check(denied["status"] == revoked["status"] == "denied" and denied.get("audit_warning") and revoked.get("audit_warning"), "deny and revoke still apply during audit failure")
        check(not write("audit-new.txt")["structuredContent"].get("approval_id"), "audit failure blocks new requests")
        check(server.tool("fs_list", workspace_id="workspace")["isError"] is False, "audit failure keeps read-only tools available")
        raw = journal.read_bytes()
        check(len(raw) <= 8192 and raw.endswith(b"\n"), "budget failure leaves complete bounded audit records")
        check(b"secret audit marker" not in raw, "failed audit does not leak tool text")
    finally:
        server.close()
    audit_lines(data_dir)
    for value in [True, 4095, 67108865, "8192", None]:
        config.write_text(json.dumps({"audit_max_bytes": value}), encoding="utf-8")
        result = subprocess.run([str(executable), "--config", str(config), "--data-dir", str(data_dir)], input=b"", capture_output=True, timeout=10)
        check(result.returncode == 2 and not result.stdout, "invalid audit budget rejected")


def audit_result_failure_checks(executable, folder, root):
    # Calibrate from actual persisted records, then reproduce a budget boundary
    # after the write intent but before its result. File names have equal lengths.
    config = folder / "audit-result-config.json"
    settings = {"roots": [{"id": "workspace", "path": str(root), "read_only": False}]}
    config.write_text(json.dumps(settings), encoding="utf-8")
    def execute(data_dir, path):
        server = Stdio(executable, ["--config", str(config), "--data-dir", str(data_dir)])
        try:
            ready(server)
            arguments = {"workspace_id": "workspace", "path": path, "text": "result marker"}
            approval_id = server.tool("fs_write", **arguments)["structuredContent"]["approval_id"]
            owner(executable, data_dir, "approve", approval_id)
            for i in range(8):
                check("approval_id" in server.tool("fs_write", workspace_id="workspace", path=f"result-padding-{i}.txt", text="x")["structuredContent"], "result failure fixture requests succeed")
            arguments["approval_id"] = approval_id
            result = server.tool("fs_write", **arguments)
            cached = server.tool("fs_write", **arguments)
            caps = server.tool("host_capabilities")["structuredContent"]
            return result, cached, caps
        finally:
            server.close()
    scout_dir = folder / "audit-result-scout"
    execute(scout_dir, "audit-result-probe.txt")
    records = next(scout_dir.glob("audit-*.jsonl")).read_bytes().splitlines(keepends=True)
    prefix = []
    for line in records:
        prefix.append(line)
        if json.loads(line)["event"] == "write_started":
            break
    budget = sum(map(len, prefix))
    check(budget >= 4096 and json.loads(prefix[-1])["event"] == "write_started", "result failure budget has a recorded intent")
    settings["audit_max_bytes"] = budget
    config.write_text(json.dumps(settings), encoding="utf-8")
    data_dir = folder / "audit-result-final"
    result, cached, caps = execute(data_dir, "audit-result-final.txt")
    check(not result["isError"] and result["structuredContent"]["status"] == "completed" and result["structuredContent"].get("audit_warning"), "result recording failure reports committed creation with warning")
    check(cached["structuredContent"]["status"] == "completed" and cached["structuredContent"].get("audit_warning"), "retry preserves actual completion after failed result recording")
    check(not caps["audit_healthy"] and (root / "audit-result-final.txt").read_text() == "result marker", "committed file stays intact when result journal fails")
    records = next(data_dir.glob("audit-*.jsonl")).read_bytes().splitlines(keepends=True)
    check(json.loads(records[-1])["event"] == "write_started" and sum(map(len, records)) <= budget, "incomplete audit preserves intent without a fabricated completion")


def modern_stdio_checks(executable, folder):
    server = Stdio(executable, ["--data-dir", str(folder / "modern-state")])
    try:
        discover = server.call(modern("server/discover"))["result"]
        check(discover["resultType"] == "complete" and "2026-07-28" in discover["supportedVersions"], "modern discover without initialize")
        check(discover["ttlMs"] == 0 and discover["cacheScope"] == "private", "discovery cache scope")
        check(discover["_meta"]["io.modelcontextprotocol/serverInfo"]["version"] == EXPECTED_VERSION, "modern server info metadata")
        tools = server.call(modern("tools/list"))["result"]
        check(len(tools["tools"]) == 13 and tools["resultType"] == "complete" and tools["cacheScope"] == "private", "modern tools list")
        check(server.call(modern("tools/call", {"name": "host_info"}))["result"]["structuredContent"]["version"] == EXPECTED_VERSION, "modern call")
        check(server.call(request("tools/list"))["error"]["code"] == -32600, "stdio cannot switch era")
        check(server.call(modern("initialize"))["error"]["code"] == -32601, "modern has no initialize")
        invalid = modern("tools/list")
        del invalid["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"]
        check(server.call(invalid)["error"]["code"] == -32602, "modern metadata required")
        invalid = modern("tools/list")
        invalid["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01"
        error = server.call(invalid)["error"]
        check(error["code"] == -32022 and error["data"]["requested"] == "2099-01-01", "unsupported version error")
    finally:
        server.close()


def external_url_checks(executable, folder):
    valid = [("Bot.Example.COM", "https://bot.example.com/mcp"),
             ("https://Bot.Example.COM/", "https://bot.example.com/mcp"),
             ("https://bot.example.com:00443", "https://bot.example.com/mcp"),
             ("HTTP://Bot.Example.COM:0080/", "http://bot.example.com/mcp"),
             ("https://bot.example.com:8443", "https://bot.example.com:8443/mcp"),
             ("http://bot.example.com:18080/", "http://bot.example.com:18080/mcp"),
             ("https://" + "a" * 63 + ".test", "https://" + "a" * 63 + ".test/mcp")]
    for value, endpoint in valid:
        server = Stdio(executable, ["--public-url", value, "--data-dir", str(folder / "url-state")])
        try:
            ready(server)
            caps = server.tool("host_capabilities")["structuredContent"]
            check(caps["public_endpoint"] == endpoint and not caps["oauth"] and not caps["tunnel_management"], "configured external endpoint without OAuth or tunnel management")
        finally:
            server.close()
    config = folder / "external-url.json"
    invalid = ["https://", "https://user:secret@bot.example.com", "https://bot.example.com/mcp",
               "https://bot.example.com?token=secret", "https://bot.example.com#fragment", "https://*.example.com",
               "bot.example.com\r\nInjected: value", " bot.example.com", "https://bot.example.com:0",
               "https://bot.example.com:65536", "https://bot.example.com:123456", "https://bot.example.com:443:443",
               "https://bot.example.com:", "https://-bad.example.com", "https://bad-.example.com",
               "https://bad..example.com", "https://bot.example.com.", "https://" + "a" * 64 + ".test",
               "https://bot.example.com//", "ftp://bot.example.com", "https://[::1]", "https://中文.example", None, True, 7]
    for value in invalid:
        config.write_text(json.dumps({"http": {"public_url": value}}), encoding="utf-8")
        result = subprocess.run([str(executable), "--config", str(config), "--data-dir", str(folder / "bad-url-state")], input=b"", capture_output=True, timeout=10)
        check(result.returncode == 2 and not result.stdout and b"secret" not in result.stderr, "invalid external URL rejected without reflecting credentials")
    config.write_text(json.dumps({"http": {"public_url": ""}}), encoding="utf-8")
    server = Stdio(executable, ["--config", str(config), "--data-dir", str(folder / "url-state")])
    try:
        ready(server)
        check("public_endpoint" not in server.tool("host_capabilities")["structuredContent"], "empty external URL disables external address")
    finally:
        server.close()


def content_search_checks(executable, folder):
    root = folder / "content search 中文 & !"; root.mkdir()
    nested = root / "nested"; nested.mkdir()
    outside = folder / "search-outside"; outside.mkdir()
    content = "Alpha\n你好 😀 match\nsame match match\r\nlast MATCH\rline match"
    (root / "utf8.txt").write_bytes(b"\xef\xbb\xbf" + content.encode("utf-8"))
    (root / "utf16le.txt").write_bytes(b"\xff\xfe" + content.encode("utf-16le"))
    (root / "utf16be.txt").write_bytes(b"\xfe\xff" + content.encode("utf-16be"))
    (root / "binary.bin").write_bytes(b"match\0hidden")
    (root / "invalid.bin").write_bytes(b"\xff\xffmatch")
    (root / "empty.txt").write_bytes(b"")
    (root / "longline.txt").write_text("😀" * 400 + "needle" + "中" * 400, encoding="utf-8")
    (nested / "nested.txt").write_text("nested match\n", encoding="utf-8")
    (outside / "secret.txt").write_text("private-secret-match", encoding="utf-8")
    created = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(root / "escape"), str(outside)], capture_output=True, timeout=10)
    check(created.returncode == 0, "content search escape fixture")
    bounded = root / "bounded"; bounded.mkdir()
    (bounded / "partial.txt").write_bytes("A😀B match\nlate target".encode())
    server = Stdio(executable, ["--root", str(root), "--data-dir", str(folder / "content-state")])
    try:
        ready(server)
        def search(query="match", **options):
            return server.tool("fs_search", workspace_id="workspace", query=query, mode="content", **options)
        result = search()["structuredContent"]
        check(len(result["matches"]) == 9 and result["skipped_files"] == 2 and not result["truncated"], "UTF-8/UTF-16 content with binary skips")
        check({item["encoding"] for item in result["matches"]} == {"utf-8", "utf-16le", "utf-16be"}, "content search encoding detection")
        for filename in ["utf8.txt", "utf16le.txt", "utf16be.txt"]:
            items = [item for item in result["matches"] if item["name"] == filename]
            check([item["line"] for item in items] == [2, 3, 5], "content line numbers for LF/CRLF/CR")
            check(items[0]["column_byte"] == len("你好 😀 ".encode()) + 1 and items[0]["excerpt"] == "你好 😀 match", "Unicode excerpt and byte column")
            check(len([item for item in items if item["line"] == 3]) == 1, "one result per matching line")
        recursive = search(recursive=True)["structuredContent"]
        check(len(recursive["matches"]) == 11 and recursive["skipped_reparse_directories"] == 1, "recursive content and escaped directory skip")
        check(not search("private-secret-match", recursive=True)["structuredContent"]["matches"], "content search never returns outside directory text")
        check(search(path="escape", recursive=True)["isError"], "outside starting directory denied")
        check(search(path="../search-outside")["isError"], "content starting path traversal denied")
        unicode = search("你好 😀")["structuredContent"]
        check(len(unicode["matches"]) == 3, "Chinese and astral query")
        limited = search(max_results=1, recursive=True)["structuredContent"]
        check(len(limited["matches"]) == 1 and limited["truncated"], "content result budget")
        longline = search("needle")["structuredContent"]["matches"][0]
        check(longline["column_byte"] == 1601 and "needle" in longline["excerpt"] and longline["excerpt_truncated"], "bounded excerpt surrounds late match")
        check(len(longline["excerpt"].encode()) <= 1024 and not longline["file_truncated"], "excerpt keeps complete Unicode characters")
        partial = search("A", path="bounded", max_file_bytes=4)["structuredContent"]
        check(partial["bytes_read"] == 4 and partial["partial_files"] == 1 and partial["truncated"], "per-file byte budget")
        check(partial["matches"][0]["excerpt"] == "A" and partial["matches"][0]["file_truncated"], "partial UTF-8 character excluded from excerpt")
        capped = search("late", path="bounded", max_total_bytes=8)["structuredContent"]
        check(capped["bytes_read"] == 8 and not capped["matches"] and capped["truncated"], "total byte budget prevents reading later match")
        (bounded / "second.txt").write_text("match\n", encoding="utf-8")
        capped = search(path="bounded", max_total_bytes=4)["structuredContent"]
        check(capped["bytes_read"] <= 4 and capped["files_read"] == 1 and capped["truncated"], "total budget stops opening additional files")
        for options in [{"mode": "invalid"}, {"mode": 7}, {"max_file_bytes": 3}, {"max_file_bytes": 1048577},
                        {"max_total_bytes": True}, {"max_total_bytes": 16777217}, {"recursive": "true"}]:
            args = {"workspace_id": "workspace", "query": "match", "mode": "content", **options}
            check(server.tool("fs_search", **args)["isError"], "invalid content search options")
        for query in ["", "line\nbreak", "A\0B", "x" * 513]:
            check(search(query)["isError"], "invalid content search query")
        for tool_args in [dict(query="utf8"), dict(query="utf8", mode="filename")]:
            result = server.tool("fs_search", workspace_id="workspace", **tool_args)["structuredContent"]
            check(result["mode"] == "filename" and len(result["matches"]) == 1, "filename search remains default")
    finally:
        server.close()


class Stdio:
    def __init__(self, executable, arguments):
        self.process = subprocess.Popen([str(executable), *arguments], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lines = queue.Queue()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def send(self, value):
        data = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.process.stdin.write(data + b"\n")
        self.process.stdin.flush()

    def call(self, value):
        self.send(value)
        line = self.lines.get(timeout=10)
        check(line is not None, "stdio exited without a response")
        response = json.loads(line)
        check(response.get("jsonrpc") == "2.0", "non-protocol stdout")
        return response

    def tool(self, name, **arguments):
        return self.call(request("tools/call", {"name": name, "arguments": arguments}))["result"]

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                raise AssertionError("stdio did not exit on EOF")
        stderr = self.process.stderr.read()
        self.thread.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()
        check(self.process.returncode == 0, "stdio process failed: " + stderr.decode(errors="replace"))


def png(width=1, height=1):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\0\xff\0\0")) + chunk(b"IEND", b""))


# Original 1x1 red fixture encoded with Windows GDI+, not remote media.
JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/"
    "2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/"
    "wAARCAABAAEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKB"
    "kaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
    "p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
    "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpj"
    "ZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/"
    "9oADAMBAAIRAxEAPwD50ooor8MP9Uz/2Q==")


def stdio_checks(executable, folder, root, other, config, junction):
    arguments = ["--config", str(config), "--data-dir", str(folder / "stdio-state")]
    server = Stdio(executable, arguments)
    try:
        check(server.call(request("tools/list"))["error"]["code"] == -32600, "stdio lifecycle")
        result = server.call(initialize())["result"]
        check(result["protocolVersion"] == "2025-11-25", "version negotiation")
        check(set(result["capabilities"]) == {"tools"}, "unimplemented capability advertised")
        server.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        tools = server.call(request("tools/list"))["result"]["tools"]
        check({item["name"] for item in tools} == {"host_info", "host_capabilities", "fs_list", "fs_read", "fs_search", "image_view", "fs_write", "fs_patch", "task_start", "task_status", "task_output", "task_input", "task_cancel"}, "tool registry")
        check(all(item["annotations"]["readOnlyHint"] == (item["name"] not in {"fs_write", "fs_patch", "task_start", "task_input", "task_cancel"}) for item in tools), "tool annotations")
        check(server.call(initialize())["error"]["code"] == -32600, "duplicate initialization")
        info = server.tool("host_info")["structuredContent"]
        check(info["version"] == EXPECTED_VERSION and info["system"].startswith("Windows"), "host info")
        caps = server.tool("host_capabilities")["structuredContent"]
        check({r["workspace_id"] for r in caps["workspaces"]} == {"workspace", "other"}, "multiple roots")
        check(not caps["execution"] and not caps["oauth"] and caps["file_write"] and caps["approval"] and caps["file_overwrite"] and caps["file_patch"] and not caps["atomic_overwrite"], "actual capabilities")
        check(caps["durable_audit"] and caps["audit_healthy"] and not caps["durable_approvals"], "audit persistence does not restore authorization")
        listing = server.tool("fs_list", workspace_id="workspace", max_entries=2)["structuredContent"]
        check(len(listing["entries"]) == 2 and listing["has_more"], "directory pagination")
        page = server.tool("fs_list", workspace_id="workspace", offset=listing["next_offset"], max_entries=200)["structuredContent"]
        check(len(page["entries"]) > 0 and not page["has_more"], "directory continuation")
        check(server.tool("fs_read", workspace_id="other", path="secret.txt")["structuredContent"]["text"] == "outside", "explicit second root")
        text = server.tool("fs_read", workspace_id="workspace", path="中文 空格 & !/文本.txt")["structuredContent"]
        check(text["text"] == "你好，ShellBot 😀\n第二行\n", "Chinese paths and Unicode")
        for name, encoding in [("utf8.txt", "utf-8"), ("le.txt", "utf-16le"), ("be.txt", "utf-16be")]:
            cursor = 0
            combined = ""
            for _ in range(20):
                data = server.tool("fs_read", workspace_id="workspace", path=name, offset=cursor,
                                   encoding="auto" if cursor == 0 else encoding, max_bytes=4)["structuredContent"]
                check(data["encoding"] == encoding, "text encoding detection")
                combined += data["text"]
                check(data["next_offset"] > cursor, "pagination progress")
                cursor = data["next_offset"]
                if not data["truncated"]:
                    break
            check(combined == "A😀中B", "Unicode pagination: " + name)
        rejected = ["../other/secret.txt", str(other / "secret.txt"), "C:/Windows/win.ini", "\\\\localhost\\C$\\Windows\\win.ini",
                    "utf8.txt:stream", "utf8.txt\0ignored", "dir/../../other/secret.txt"]
        if junction:
            rejected += ["escape/secret.txt", "escape2/secret.txt"]
        for path in rejected:
            check(server.tool("fs_read", workspace_id="workspace", path=path)["isError"], "path escape accepted: " + repr(path))
        if junction:
            check(server.tool("fs_list", workspace_id="workspace", path="escape")["isError"], "junction directory escape")
        for name in ["binary.bin", "invalid.txt", "broken16.txt"]:
            check(server.tool("fs_read", workspace_id="workspace", path=name)["isError"], "invalid text accepted")
        check(server.tool("fs_read", workspace_id="workspace", path="utf8.txt", offset=2, encoding="utf-8")["isError"], "mid-character offset")
        check(server.tool("fs_read", workspace_id="workspace", path="utf8.txt", offset=1)["isError"], "nonzero auto encoding")
        check(server.tool("fs_read", workspace_id="workspace", path="le.txt", offset=3, encoding="utf-16le")["isError"], "odd UTF-16 offset")
        check(server.tool("fs_read", workspace_id="workspace", path="utf8.txt", max_bytes=65537)["isError"], "text limit")
        check(server.tool("fs_read", workspace_id="missing", path="utf8.txt")["isError"], "unknown workspace")
        check(server.tool("fs_read", workspace_id="workspace", path="utf8.txt", approved=True)["isError"], "unknown permission-like arguments")
        check(server.tool("host_info", unexpected=1)["isError"], "unexpected host arguments")
        for name, mime, original in [("pixel.png", "image/png", png()), ("pixel.jpg", "image/jpeg", JPEG)]:
            result = server.tool("image_view", workspace_id="workspace", path=name)
            check(not result["isError"], "image failed: " + name)
            images = [item for item in result["content"] if item["type"] == "image"]
            check(len(images) == 1 and images[0]["mimeType"] == mime, "native image result")
            check(base64.b64decode(images[0]["data"]) == original, "image bytes changed")
            check(result["structuredContent"]["width"] == 1 and result["structuredContent"]["height"] == 1, "image dimensions")
        for name in ["utf8.txt", "big.png", "huge-dim.png"]:
            check(server.tool("image_view", workspace_id="workspace", path=name)["isError"], "invalid image accepted")
        search = server.tool("fs_search", workspace_id="workspace", query="needle", recursive=True)["structuredContent"]
        check(len(search["matches"]) == 2, "recursive file-name search or junction traversal")
        search = server.tool("fs_search", workspace_id="workspace", query="needle", recursive=False)["structuredContent"]
        check(len(search["matches"]) == 1, "nonrecursive search")
        limited = server.tool("fs_search", workspace_id="workspace", query="needle", recursive=True, max_results=1)["structuredContent"]
        check(len(limited["matches"]) == 1 and limited["truncated"], "search output limit")
        check(server.call(request("unknown"))["error"]["code"] == -32601, "unknown method")
        check(server.call(request("tools/call", {"name": "exec_run", "arguments": {}}))["error"]["code"] == -32602, "unimplemented tool")
        check(server.call(b"{")["error"]["code"] == -32700, "malformed JSON")
        check(server.call(b'{"jsonrpc":"2.0","id":1,"method":"ping","method":"tools/list"}')["error"]["code"] == -32600, "duplicate keys")
        check(server.call(b'{"jsonrpc":"2.0","id":null,"method":"ping"}')["error"]["code"] == -32600, "null request id")
        check(server.call(b"[" + b"[" * 100 + b"0" + b"]" * 101)["error"]["code"] == -32600, "nested invalid request")
        check(server.call(b"x" * (1024 * 1024 + 10))["error"]["code"] == -32700, "stdio request limit")
        server.send({"jsonrpc": "2.0", "method": "tools/call", "params": {"name": "host_info"}})
        check(server.call(request("ping", identifier="after-notifications"))["id"] == "after-notifications", "notification emitted a response")
        collision = subprocess.run([str(executable), *arguments], input=b"", capture_output=True, timeout=10)
        check(collision.returncode == 2 and b"locked" in collision.stderr, "data directory lease")
    finally:
        server.close()
    # A closed process must release its lease. Legacy version fallback remains explicit.
    server = Stdio(executable, arguments)
    try:
        check(server.call(initialize("not-a-supported-version"))["result"]["protocolVersion"] == "2025-11-25", "legacy fallback")
    finally:
        server.close()


def http_checks(executable, folder, root):
    global CLOSE_FAILURES
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    token = subprocess.check_output([str(executable), "--generate-token"], timeout=10).decode().strip()
    check(len(token) == 64 and all(c in "0123456789abcdef" for c in token), "system random token")
    http_config = folder / "http-write.json"
    http_config.write_text(json.dumps({"roots": [{"id": "workspace", "path": str(root), "read_only": False}],
                                       "http": {"public_url": "https://bot.example.test"}}), encoding="utf-8")
    http_data = folder / "http-state"
    args = ["--http", "--config", str(http_config), "--port", str(port), "--data-dir", str(http_data)]
    environment = os.environ.copy()
    environment.pop("SHELLBOT_TOKEN", None)
    missing = subprocess.run([str(executable), *args], env=environment, capture_output=True, timeout=10)
    check(missing.returncode == 2 and not missing.stdout, "unauthenticated HTTP startup")
    environment["SHELLBOT_TOKEN"] = token
    process = subprocess.Popen([str(executable), *args], env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)

    def call(body=None, path="/mcp", method="POST", changes=None, auth=True):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                   "MCP-Protocol-Version": "2025-11-25"}
        if auth:
            headers["Authorization"] = "Bearer " + token
        if changes:
            for key, value in changes.items():
                if value is None:
                    headers.pop(key, None)
                else:
                    headers[key] = value
        payload = body if isinstance(body, bytes) else json.dumps(body or request("ping")).encode()
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        try:
            connection.request(method, path, payload if method == "POST" else None, headers)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                if call(path="/healthz", method="GET")[0] == 200:
                    break
            except OSError:
                pass
            if process.poll() is not None or time.monotonic() > deadline:
                raise AssertionError("HTTP did not become ready")
            time.sleep(0.05)
        check(call(auth=False)[0] == 401, "HTTP anonymous access")
        check(call(changes={"Authorization": "Bearer " + "0" * 64})[0] == 401, "HTTP wrong token")
        check(call(changes={"Origin": "https://attacker.example"})[0] == 403, "HTTP Origin protection")
        check(call(changes={"Host": "attacker.example"})[0] == 403, "HTTP Host protection")
        check(call(changes={"Origin": "null"})[0] == 403, "opaque origin")
        check(call(changes={"Origin": f"http://127.0.0.1:{port}"})[0] == 200, "valid local origin")
        check(call(changes={"Host": "bot.example.test", "Origin": "https://bot.example.test"})[0] == 200, "configured external Host and Origin")
        check(call(changes={"Host": "BOT.EXAMPLE.TEST:443", "Origin": "https://BOT.EXAMPLE.TEST:443"})[0] == 200, "external DNS casing and default port")
        check(call(changes={"Host": "bot.example.test"}, auth=False)[0] == 401, "external domain does not bypass bearer")
        check(call(changes={"Host": "attacker.example", "X-Forwarded-Host": "bot.example.test", "X-Forwarded-Proto": "https"})[0] == 403, "forwarded headers do not grant external host permission")
        check(call(changes={"Host": "attacker.bot.example.test", "Origin": "https://bot.example.test"})[0] == 403, "external subdomain needs explicit configuration")
        check(call(changes={"Host": "bot.example.test", "Origin": "http://bot.example.test"})[0] == 403, "external origin scheme must match configuration")
        check(call(path="/mcp?token=ignored")[0] == 400, "URL credentials rejected")
        check(call(path="/pixel.png", method="GET")[0] == 404, "no automatic static files")
        check(call(path="/mcp", method="GET")[0] == 405, "no SSE advertised")
        check(call(changes={"MCP-Protocol-Version": "2026-07-28"})[0] == 400, "unsupported modern protocol")
        check(call(changes={"Content-Type": "text/plain"})[0] == 415, "content type")
        check(call(changes={"Accept": "text/event-stream"})[0] == 406, "response negotiation")
        check(call(b"x" * (1024 * 1024 + 1))[0] == 413, "HTTP request limit")
        check(call(b"{")[0] == 400, "HTTP malformed request")
        status, data = call(initialize())
        check(status == 200 and json.loads(data)["result"]["protocolVersion"] == "2025-11-25", "HTTP initialization")
        status, data = call({"jsonrpc": "2.0", "method": "notifications/initialized"})
        check(status == 202 and not data, "HTTP notification response")
        status, data = call(request("tools/list"))
        check(status == 200 and len(json.loads(data)["result"]["tools"]) == 13, "HTTP tool list")
        headers = {"MCP-Protocol-Version": "2026-07-28", "Mcp-Method": "server/discover"}
        status, data = call(modern("server/discover"), changes=headers)
        check(status == 200 and json.loads(data)["result"]["resultType"] == "complete", "modern HTTP discover")
        check(call(modern("server/discover"), changes={**headers, "Mcp-Method": "tools/list"})[0] == 400, "method header mismatch")
        for changes in [{**headers, "Mcp-Method": None}, {**headers, "MCP-Protocol-Version": None},
                        {**headers, "MCP-Protocol-Version": "2025-11-25"}]:
            status, data = call(modern("server/discover"), changes=changes)
            check(status == 400 and json.loads(data)["error"]["code"] == -32020, "required modern headers")
        body = modern("tools/call", {"name": "host_info"})
        headers = {"MCP-Protocol-Version": "2026-07-28", "Mcp-Method": "tools/call", "Mcp-Name": "host_info"}
        check(call(body, changes=headers)[0] == 200, "modern HTTP tool call")
        for encoded in ["=?base64?aG9zdF9pbmZv?=", "=?base64?aG9zdF9pbmZv====?="]:
            check(call(body, changes={**headers, "Mcp-Name": encoded})[0] == (200 if encoded.endswith("Zv?=") else 400), "base64 MCP header")
        for name in [None, "host_capabilities"]:
            status, data = call(body, changes={**headers, "Mcp-Name": name})
            check(status == 400 and json.loads(data)["error"]["code"] == -32020, "tool name header mismatch")
        check(call(modern("unknown"), changes={"MCP-Protocol-Version": "2026-07-28", "Mcp-Method": "unknown"})[0] == 404, "modern unsupported method HTTP status")
        write_body = modern("tools/call", {"name": "fs_write", "arguments": {"workspace_id": "workspace", "path": "http-created.txt", "text": "HTTP 😀\n"}})
        write_body["params"]["_meta"]["io.modelcontextprotocol/clientInfo"] = {"name": "owner", "version": "approved"}
        write_headers = {"MCP-Protocol-Version": "2026-07-28", "Mcp-Method": "tools/call", "Mcp-Name": "fs_write"}
        status, data = call(write_body, changes=write_headers)
        pending = json.loads(data)["result"]["structuredContent"]
        check(status == 200 and pending["status"] == "pending" and pending["actor"] == "http-development", "client metadata does not grant ownership")
        owner(executable, http_data, "approve", pending["approval_id"])
        write_body["params"]["arguments"]["approval_id"] = pending["approval_id"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            replies = list(pool.map(lambda _i: call(write_body, changes=write_headers), range(16)))
        check(all(status == 200 and json.loads(data)["result"]["structuredContent"]["status"] in {"completed", "running"}
                  for status, data in replies), "atomic approval consumption under concurrent retries")
        check((root / "http-created.txt").read_bytes() == "HTTP 😀\n".encode() and not list(root.glob(".shellbot-*.tmp")), "HTTP write published once")
        audited = [event for event in audit_lines(http_data) if event.get("approval_id") == pending["approval_id"]]
        check([event["event"] for event in audited] == ["approval_requested", "approval_approved", "write_started", "write_completed"], "concurrent HTTP retries audit a single execution")
        status, data = call(request("tools/call", {"name": "fs_read", "arguments": {"workspace_id": "workspace", "path": "utf8.txt"}}))
        check(status == 200 and json.loads(data)["result"]["structuredContent"]["text"] == "A😀中B", "HTTP tool call")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            replies = list(pool.map(lambda i: call(request("tools/call", {"name": "host_info"}, i)), range(100)))
        check(all(status == 200 and json.loads(data)["id"] == i for i, (status, data) in enumerate(replies)), "concurrent HTTP responses")
        status, data = call(request("tools/call", {"name": "image_view", "arguments": {"workspace_id": "workspace", "path": "pixel.png"}}))
        images = [part for part in json.loads(data)["result"]["content"] if part["type"] == "image"]
        check(status == 200 and base64.b64decode(images[0]["data"]) == png(), "HTTP native image result")
        persistent = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        try:
            headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "Authorization": "Bearer " + token,
                       "MCP-Protocol-Version": "2025-11-25"}
            for identifier in [101, 102]:
                persistent.request("POST", "/mcp", json.dumps(request("ping", identifier=identifier)), headers)
                response = persistent.getresponse()
                check(response.status == 200 and json.loads(response.read())["id"] == identifier, "persistent HTTP connection")
        finally:
            persistent.close()
        if CLOSE_PROBE:
            def close_call(i):
                try:
                    return call(request("ping", identifier=i), changes={"Connection": "close"})
                except (OSError, http.client.HTTPException):
                    return 0, b"{}"
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                closing = list(pool.map(close_call, range(100)))
            CLOSE_FAILURES = sum(status != 200 or json.loads(data).get("id") != i for i, (status, data) in enumerate(closing))
            check(CLOSE_FAILURES == 0, "100 client-requested HTTP closes")
        check(call(changes={"MCP-Protocol-Version": None})[0] == 200, "legacy HTTP version default")
        check(owner(executable, http_data, "stop")["status"] == "stopping", "local owner stop")
        process.wait(timeout=15)
    finally:
        if process.poll() is None:
            process.send_signal(signal.CTRL_BREAK_EVENT)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError("HTTP shutdown timed out")
        stdout, stderr = process.communicate(timeout=5)
        check(process.returncode == 0, "HTTP clean shutdown")
        check(not stdout and token.encode() not in stderr, "HTTP diagnostics or credentials on stdout")


def proxy_http_checks(executable, folder):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    token = subprocess.check_output([str(executable), "--generate-token"], timeout=10).decode().strip()
    environment = dict(os.environ, SHELLBOT_TOKEN=token)
    state = folder / "proxy-http-state"
    process = subprocess.Popen([str(executable), "--http", "--port", str(port), "--data-dir", str(state)],
                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def call(host="forwarded.example.test", origin=None, authenticated=True, extra=None, body=None):
        headers = {"Host": host, "Content-Type": "application/json", "Accept": "application/json"}
        if origin is not None:
            headers["Origin"] = origin
        if authenticated:
            headers["Authorization"] = "Bearer " + token
        headers.update(extra or {})
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        try:
            connection.request("POST", "/mcp", json.dumps(body or request("ping")), headers)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                if call()[0] == 200:
                    break
            except OSError:
                pass
            if process.poll() is not None or time.monotonic() > deadline:
                raise AssertionError("Domain-independent listener did not start")
            time.sleep(0.05)
        check(call()[0] == 200, "existing proxy works without configured domain")
        check(call(origin="https://client.example.test")[0] == 200, "valid proxy origin works without domain registration")
        check(call(host="forwarded.example.test:8443")[0] == 200, "proxy Host may include external port")
        check(call(authenticated=False)[0] == 401, "unconfigured proxy still requires bearer")
        check(call(authenticated=False, extra={"Forwarded": "host=localhost", "X-Forwarded-Host": "localhost"})[0] == 401, "proxy headers do not authenticate")
        for host in ["bad host.test", "user@host.test", "host.test/path", "host.test:99999"]:
            check(call(host=host)[0] in {400, 403}, "malformed proxy authority rejected")
        for origin in ["null", "https://user:pass@host.test", "https://host.test/path"]:
            check(call(origin=origin)[0] == 403, "malformed proxy origin rejected")
        code, body = call(body=request("tools/call", {"name": "host_capabilities"}))
        caps = json.loads(body)["result"]["structuredContent"]
        check(code == 200 and not caps["domain_required"] and caps["http_binding"] == "127.0.0.1" and "public_endpoint" not in caps, "proxy listener reports actual configuration")
        owner(executable, state, "stop")
        process.wait(timeout=15)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        stdout, stderr = process.communicate(timeout=5)
        check(not stdout and token.encode() not in stderr, "proxy host credential redaction")


def execution_identity_checks(executable, folder):
    SKIPPED.append("administrator agent startup and UAC require explicit interactive acceptance")
    def status(settings=None):
        arguments = [str(executable), "--execution-status"]
        if settings is not None:
            path = folder / "execution-config.json"
            path.write_text(json.dumps({"execution": settings}), encoding="utf-8")
            arguments += ["--config", str(path)]
        output = subprocess.run(arguments, capture_output=True, timeout=10)
        check(output.returncode == 0 and not output.stderr, "execution identity utility")
        return json.loads(output.stdout)

    default = status()
    check(default["profile"] == "controlled" and not default["configured_enabled"], "execution defaults disabled and controlled")
    check(default["approval_mode"] == "always", "controlled defaults require approval")
    check(not default["command_execution_available"] and not default["task_tools_available"], "identity foundation cannot execute commands")
    check(not default["remote_approval_available"] and not default["controlled_isolation_verified"], "unfinished security capabilities are explicit")
    actual = default["host_identity"]
    check(actual["process_id"] > 0 and actual["user_sid"].startswith("S-1-"), "actual identity is populated")
    check(isinstance(actual["elevated"], bool) and isinstance(actual["administrator_group_enabled"], bool), "actual elevation flags are booleans")
    for profile in ["controlled", "user", "administrator"]:
        disabled = status({"enabled": False, "profile": profile})
        check(disabled["profile"] == profile and disabled["readiness"] == "disabled", "disabled profile remains explicit")
        check(not disabled["administrator_agent_started"] and disabled["administrator_agent_starts"] == 0, "disabled administrator configuration never starts an agent")
    if not actual["elevated"] and not actual["administrator_group_enabled"]:
        ordinary = status({"enabled": True, "profile": "user"})
        check(ordinary["approval_mode"] == "preauthorized" and not ordinary["host_identity"]["elevated"], "user profile uses ordinary identity")
        check(not ordinary["administrator_agent_started"], "user profile does not elevate")
        for mode in ["always", "rules"]:
            controlled = status({"enabled": True, "profile": "controlled", "approval_mode": mode})
            check(controlled["approval_mode"] == mode, "controlled approval selection")
            check(controlled["restricted_token_prepared"] and controlled["restricted_owner_rejected"], "restricted token is excluded from owner management")
            check(not controlled["command_execution_available"], "restricted token preparation does not grant execution")
        state = folder / "execution-owner-state"
        configuration = folder / "execution-owner.json"
        configuration.write_text(json.dumps({"execution": {"enabled": True, "profile": "controlled"}}), encoding="utf-8")
        bot = Stdio(executable, ["--config", str(configuration), "--data-dir", str(state)])
        try:
            bot.call(initialize())
            bot.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            capability = bot.tool("host_capabilities")["structuredContent"]
            check(not capability["execution"] and capability["execution_status"]["restricted_owner_rejected"], "MCP capabilities retain disabled commands and owner boundary")
            check(owner(executable, state, "list-approvals")["approvals"] == [], "ordinary owner management works before restricted probe")
            probe = subprocess.run([str(executable), "--check-owner-boundary", "--data-dir", str(state)], capture_output=True, timeout=10)
            check(probe.returncode == 0 and not probe.stderr, "restricted owner boundary probe succeeds")
            checked = json.loads(probe.stdout)
            check(checked["restricted_token_rejected"] and checked["restricted_owner_command_denied"], "live owner channel denies a restricted caller")
            check(owner(executable, state, "list-approvals")["approvals"] == [], "restricted probe does not disturb the owner channel")
            check(bot.tool("host_capabilities", profile="administrator")["isError"], "MCP cannot select administrator profile")
        finally:
            bot.close()
    else:
        path = folder / "elevated-user-rejected.json"
        path.write_text(json.dumps({"execution": {"enabled": True, "profile": "user"}}), encoding="utf-8")
        output = subprocess.run([str(executable), "--execution-status", "--config", str(path)], capture_output=True, timeout=10)
        check(output.returncode == 2 and not output.stdout, "elevated host cannot masquerade as ordinary user mode")
        SKIPPED.append("restricted owner boundary requires an ordinary unelevated test process")

    bad_settings = [[], None, True, {"enabled": 1}, {"enabled": "true"}, {"profile": "root"},
                    {"profile": None}, {"profile": "administrator", "approval_mode": "always"},
                    {"profile": "user", "approval_mode": "rules"}, {"approval_mode": "never"},
                    {"approved": True}, {"elevated": True}, {"rules": [{"program": "cmd.exe"}]}]
    bad_configs = [json.dumps({"execution": value}) for value in bad_settings]
    bad_configs += ['{"execution":{"enabled":false,"enabled":true}}',
                    '{"execution":{"profile":"user","profile":"administrator"}}',
                    '{"execution":{"approval_mode":"always","approval_mode":"rules"}}',
                    '{"execution":{"profile":"controlled\\u0000administrator"}}']
    for index, text in enumerate(bad_configs):
        path = folder / f"bad-execution-{index}.json"
        path.write_text(text, encoding="utf-8")
        output = subprocess.run([str(executable), "--execution-status", "--config", str(path)], capture_output=True, timeout=10)
        check(output.returncode == 2 and not output.stdout, "invalid execution settings cannot grant authority")
    for arguments in [["--execution-status", "--execution-status"], ["--execution-status", "--stdio"],
                      ["--execution-status", "--approve", "0" * 32], ["--check-owner-boundary"],
                      ["--execution-agent"], ["--execution-agent", "1", "bad"],
                      ["--execution-agent", "1", "0" * 32, "extra"]]:
        output = subprocess.run([str(executable), *arguments], capture_output=True, timeout=10)
        check(output.returncode == 2 and not output.stdout, "invalid execution role arguments rejected")
    if not actual["elevated"]:
        output = subprocess.run([str(executable), "--execution-agent", str(os.getpid()), "0" * 32], capture_output=True, timeout=10)
        check(output.returncode == 2 and not output.stdout, "unelevated process cannot serve as administrator agent")


def run_checks(executable, repo):
    test_base = repo / "build" / "tests"
    test_base.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="shellbot-smoke-", dir=test_base)).resolve()
    try:
        root = folder / "workspace 中文 & !"
        other = folder / "other"
        sibling = folder / "workspace 中文 & !-sibling"
        root.mkdir(); other.mkdir(); sibling.mkdir()
        (other / "secret.txt").write_text("outside", encoding="utf-8")
        (other / "needle-outside.txt").write_text("outside", encoding="utf-8")
        (sibling / "secret.txt").write_text("outside", encoding="utf-8")
        (root / "中文 空格 & !").mkdir()
        (root / "中文 空格 & !" / "文本.txt").write_bytes("你好，ShellBot 😀\n第二行\n".encode())
        (root / "utf8.txt").write_bytes(b"\xef\xbb\xbf" + "A😀中B".encode())
        (root / "le.txt").write_bytes(b"\xff\xfe" + "A😀中B".encode("utf-16le"))
        (root / "be.txt").write_bytes(b"\xfe\xff" + "A😀中B".encode("utf-16be"))
        (root / "binary.bin").write_bytes(b"A\0B")
        (root / "invalid.txt").write_bytes(b"\xc0\xaf")
        (root / "broken16.txt").write_bytes(b"\xff\xfe\x00\xd8")
        (root / "pixel.png").write_bytes(png())
        (root / "pixel.jpg").write_bytes(JPEG)
        (root / "big.png").write_bytes(b"x" * (2 * 1024 * 1024 + 1))
        (root / "huge-dim.png").write_bytes(png(100000, 100000))
        (root / "needle.txt").write_text("match", encoding="utf-8")
        (root / "dir").mkdir()
        (root / "dir" / "needle-nested.txt").write_text("match", encoding="utf-8")
        junction = True
        for name, target in [("escape", other), ("escape2", sibling)]:
            created = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(root / name), str(target)], capture_output=True, timeout=10)
            junction &= created.returncode == 0
        check(junction, "junction fixtures must be available to test path confinement")
        config = folder / "配置.json"
        configuration = {"roots": [{"id": "workspace", "path": str(root), "read_only": True},
                                   {"id": "other", "path": str(other), "read_only": True}]}
        config.write_text(json.dumps(configuration, ensure_ascii=False), encoding="utf-8")
        for name, contents in [("unsafe.json", '{"roots":[{"id":"workspace","path":' + json.dumps(str(root)) + ',"read_only":"false"}]}'),
                               ("duplicate.json", '{"http":{"port":8765,"port":8766}}'),
                               ("unknown.json", '{"allow_all":true}'),
                               ("large.json", " " * 65537)]:
            file = folder / name
            file.write_text(contents, encoding="utf-8")
            result = subprocess.run([str(executable), "--config", str(file), "--data-dir", str(folder / "bad-state")], input=b"", capture_output=True, timeout=10)
            check(result.returncode == 2 and not result.stdout, "unsafe configuration accepted: " + name)
        execution_identity_checks(executable, folder)
        from task_checks import task_checks
        task_checks(executable, folder, Stdio, initialize, check)
        from task_audit_checks import task_audit_checks
        task_audit_checks(executable, folder, Stdio, initialize, check)
        from task_http_checks import task_http_checks
        task_http_checks(executable, folder, check)
        stdio_checks(executable, folder, root, other, config, junction)
        modern_stdio_checks(executable, folder)
        external_url_checks(executable, folder)
        content_search_checks(executable, folder)
        approval_checks(executable, folder, root, other)
        modification_checks(executable, folder, root, other)
        modification_budget_checks(executable, folder, root)
        audit_failure_checks(executable, folder, root)
        audit_result_failure_checks(executable, folder, root)
        http_checks(executable, folder, root)
        proxy_http_checks(executable, folder)
    finally:
        # The sole recursive deletion target is this run's verified workspace temp directory.
        if folder.parent != test_base.resolve() or not folder.name.startswith("shellbot-smoke-"):
            raise RuntimeError("Refusing to clean an unexpected test path")
        shutil.rmtree(folder)


def save_report(path, report):
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def main():
    global CLOSE_PROBE
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--report", type=Path, help="Save a validation receipt, including the executable hash.")
    parser.add_argument("--skip-close-probe", action="store_true", help="Run the core suite separately; explicitly exclude the known connection-close diagnostic.")
    args = parser.parse_args()
    CLOSE_PROBE = not args.skip_close_probe
    executable = args.exe.resolve(strict=True)
    repo = Path(__file__).resolve().parents[1]
    report_path = args.report.resolve() if args.report else None
    if report_path == executable:
        parser.error("The report cannot overwrite the executable.")
    artifact_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    report = {"schema_version": 1, "status": "running", "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "artifact": {"path": str(executable), "bytes": executable.stat().st_size, "sha256": artifact_hash},
              "windows_version": str(sys_version()), "assertions_passed": 0,
              "scope": "full" if CLOSE_PROBE else "core_except_connection_close",
              "excluded": [] if CLOSE_PROBE else ["connection_close_diagnostic"]}
    save_report(report_path, report)
    try:
        run_checks(executable, repo)
        if hashlib.sha256(executable.read_bytes()).hexdigest() != artifact_hash:
            raise RuntimeError("The executable changed during validation.")
        report["status"] = "passed"
        report["verified"] = ["stdio", "loopback_http", "unicode_paging", "png_jpeg_bytes", "junction_confinement",
                              "bearer_auth", "100_concurrent_http_calls", "persistent_http", "shutdown",
                              "modern_mcp", "owner_approvals", "create_only_write", "expiry_revoke_restart", "physical_parent_identity",
                              "durable_audit", "audit_budget_failure", "audit_restart_preservation", "audit_payload_redaction", "audit_result_recording_failure",
                              "external_origin_configuration", "external_host_validation", "unicode_content_search", "search_byte_and_excerpt_limits",
                              "hash_bound_overwrite", "literal_patch", "private_edit_backups", "edit_identity_conflict", "edit_backup_and_audit_budgets",
                              "existing_proxy_without_domain_configuration", "execution_profile_configuration",
                              "actual_windows_identity", "restricted_owner_channel_denial",
                              "execution_role_argument_rejection", "ordinary_user_tasks", "task_job_cancel_timeout_shutdown", "task_binary_output_stdin_idempotency", "task_environment_redaction", "task_record_budgets", "task_audit_failure_cancellation"]
    except BaseException as error:
        report["status"] = "failed"
        report["error_type"] = type(error).__name__
        raise
    finally:
        report["assertions_passed"] = CHECKS
        report["skipped"] = SKIPPED
        report["close_probe"] = {"calls": 100 if CLOSE_PROBE else 0, "failures": CLOSE_FAILURES,
                                 "status": "excluded" if not CLOSE_PROBE else "failed" if CLOSE_FAILURES else "passed"}
        report["completed_at_utc"] = datetime.now(timezone.utc).isoformat()
        save_report(report_path, report)
    print(f"PASS: {CHECKS} checks, real stdio + HTTP, Unicode paging, PNG/JPEG, junction confinement, tokens, concurrency and shutdown.")


def sys_version():
    # Exact OS build, without hostname or account details.
    import sys
    version = sys.getwindowsversion()
    return f"{version.major}.{version.minor}.{version.build}"


if __name__ == "__main__":
    main()
