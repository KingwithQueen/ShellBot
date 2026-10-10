"""M1 configuration, explicit migration and native policy checks; no UAC or live Data."""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--policy-exe", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    executable = args.exe.resolve(strict=True)
    policy_executable = args.policy_exe.resolve(strict=True)
    repo = Path(__file__).resolve().parents[1]
    report_path = args.report.resolve()
    if report_path in (executable, policy_executable):
        parser.error("Report cannot overwrite either executable.")
    folder = repo / "tests" / "test-results" / (datetime.now().strftime("%Y%m%d-%H%M%S") + "-m1-" + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    checks = 0
    native_checks = 0

    def check(condition, label):
        nonlocal checks
        if not condition:
            raise AssertionError(label)
        checks += 1

    def receipt(path):
        return {"path": str(path), "bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    report = {"schema_version": 1, "status": "running", "scope": "M1_configuration_policy_and_migration",
              "started_at_utc": datetime.now(timezone.utc).isoformat(), "artifact": receipt(executable),
              "policy_test_artifact": receipt(policy_executable), "evidence_directory": str(folder),
              "excluded": ["project_processes", "project_gateway_authentication", "full_operation_approval_adapters",
                           "restricted_command_sandbox", "project_administrator_agent", "remote_approval", "real_mcp_client", "UAC"]}

    def run(arguments, success=True):
        result = subprocess.run([str(executable)] + arguments, capture_output=True, timeout=15)
        check(result.returncode == (0 if success else 2), "CLI exit: " + arguments[0])
        if not success:
            check(not result.stdout and result.stderr, "rejection has no configuration or file payload")
            return result
        return json.loads(result.stdout)

    def validate(value, success=True, name="registry"):
        path = folder / (name + ".json")
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return run(["--validate-projects", str(path)], success)

    def migrated_policy_checks(profile, enabled, legacy_path, expected_legacy):
        nonlocal native_checks
        destination = folder / ("migrated-" + profile + "-" + str(enabled) + ".json")
        data = folder / ("new-data-" + profile + "-" + str(enabled))
        source_bytes = legacy_path.read_bytes()
        response = run(["--migrate-config", str(legacy_path), "--output", str(destination),
                        "--project-id", "legacy-default", "--project-data-dir", str(data)])
        check(response["migrated"] and not response["data_migrated"] and not response["runtime_started"], "migration stays configuration-only")
        backup = Path(str(destination) + ".legacy-backup.json")
        check(legacy_path.read_bytes() == source_bytes == backup.read_bytes(), "backup and original bytes match exactly")
        migrated = json.loads(destination.read_bytes())
        check(migrated["projects"][0]["legacy_config"] == expected_legacy, "legacy permissions and settings retained without translation")
        check(migrated["gateway"]["port"] == 8766 and migrated["gateway"]["default_project_id"] == "legacy-default", "migration preserves port and explicitly binds compatibility default")
        summary = run(["--validate-projects", str(destination)])
        project = summary["projects"][0]
        check(project["legacy_compatibility"] and project["approval_mode"] == "dev7_compatibility" and project["file_scope"] == "configured", "legacy account command permission never becomes full-disk file access")
        check(project["administrator_allowed"] == (profile == "administrator" and enabled), "legacy administrator grant independent of active backend")
        native = subprocess.run([str(policy_executable), str(destination)], capture_output=True, timeout=15)
        check(native.returncode == 0, "migrated policy evaluated from actual parsed legacy settings")
        native_checks += json.loads(native.stdout)["assertions_passed"]
        check(not data.exists(), "migration does not create or move Data")
        preserved = destination.read_bytes()
        run(["--migrate-config", str(legacy_path), "--output", str(destination), "--project-id", "legacy-default", "--project-data-dir", str(data)], False)
        check(destination.read_bytes() == preserved and backup.read_bytes() == source_bytes, "repeat migration cannot overwrite destination or backup")
        run(["--config", str(destination)], False)
        check(not data.exists(), "new registry cannot fall through to old runtime")

    try:
        native = subprocess.run([str(policy_executable)], capture_output=True, timeout=15)
        check(native.returncode == 0, "native policy assertions: " + native.stderr.decode("utf-8", errors="replace"))
        native_result = json.loads(native.stdout)
        native_checks = native_result["assertions_passed"]
        check(native_result["status"] == "passed" and native_checks > 100, "meaningful native policy matrix completed")
        example = json.loads((repo / "config" / "projects.example.json").read_text(encoding="utf-8"))
        summary = validate(example)
        check(len(summary["projects"]) == 2 and not summary["runtime_started"], "two permission projects parse without runtime activation")
        check(summary["projects"][0]["approval_mode"] == "never" and summary["projects"][1]["approval_mode"] == "workspace_auto", "different approval modes retained")
        check(not any(key in json.dumps(summary) for key in ("SHELLBOT_FULL_TOKEN", "ShellBotWorkspace")), "summary omits credential references and actual directory paths")
        minimal = {"format_version": 1, "projects": [{"id": "minimal", "token_env": "TOKEN_MIN", "data_dir": str(folder / "minimal-data")}]}
        check(validate(minimal)["projects"][0]["approval_mode"] == "always", "safe defaults work without implicit workspace")
        for mode in ("always", "workspace_auto", "never"):
            for identity in ("restricted", "current_user"):
                value = copy.deepcopy(example)
                value["projects"][0]["policy"]["approval_mode"] = mode
                value["projects"][0]["policy"]["execution"]["default_identity"] = identity
                validate(value, name="accepted-" + mode + "-" + identity)
        value = copy.deepcopy(example)
        value["projects"][0]["policy"]["execution"]["administrator_allowed"] = True
        check(validate(value)["projects"][0]["administrator_allowed"], "administrator configuration allowed separately from backend availability")

        cases = []
        def changed(label, change):
            value = copy.deepcopy(example)
            change(value)
            cases.append((label, value))

        changed("future-version", lambda v: v.update(format_version=2))
        changed("wrong-version-type", lambda v: v.update(format_version=True))
        changed("unknown-top-field", lambda v: v.update(approved=True))
        changed("empty-projects", lambda v: v.update(projects=[]))
        changed("duplicate-project", lambda v: v["projects"][1].update(id="full-access"))
        changed("duplicate-token", lambda v: v["projects"][1].update(token_env="shellbot_full_token"))
        changed("overlapping-data", lambda v: v["projects"][1].update(data_dir="c:\\shellbotprojects\\full-access\\STATE\\child"))
        changed("invalid-id", lambda v: v["projects"][0].update(id="../other"))
        changed("nul-id", lambda v: v["projects"][0].update(id="full\0access"))
        changed("relative-data", lambda v: v["projects"][0].update(data_dir="relative"))
        changed("parent-data", lambda v: v["projects"][0].update(data_dir="C:/safe/../data"))
        changed("device-data", lambda v: v["projects"][0].update(data_dir="\\\\?\\C:\\Data"))
        changed("unknown-project-field", lambda v: v["projects"][0].update(approved=True))
        changed("invalid-enabled", lambda v: v["projects"][0].update(enabled="true"))
        changed("zero-revision", lambda v: v["projects"][0].update(policy_revision=0))
        changed("fractional-revision", lambda v: v["projects"][0].update(policy_revision=1.5))
        changed("wrong-workspaces-type", lambda v: v["projects"][0].update(workspaces={}))
        changed("invalid-read-only", lambda v: v["projects"][0]["workspaces"][0].update(read_only=0))
        changed("workspace-ads", lambda v: v["projects"][0]["workspaces"][0].update(path="C:/workspace:stream"))
        changed("duplicate-directory-id", lambda v: v["projects"][0].update(extra_directories=[{"id": "workspace", "path": "C:/elsewhere"}]))
        changed("overlapping-directory-grants", lambda v: v["projects"][0].update(extra_directories=[{"id": "nested", "path": "c:/SHELLBOTworkspace/nested"}]))
        changed("invalid-cwd", lambda v: v["projects"][0].update(default_workspace_id="missing"))
        changed("extra-cwd", lambda v: v["projects"][0].update(extra_directories=[{"id": "extra", "path": "C:/extra"}], default_workspace_id="extra"))
        changed("unknown-mode", lambda v: v["projects"][0]["policy"].update(approval_mode="rules"))
        changed("invalid-scope", lambda v: v["projects"][0]["policy"]["files"].update(scope="all"))
        changed("readonly-type", lambda v: v["projects"][0]["policy"]["files"].update(current_user_read_only="false"))
        changed("scope-inconsistent", lambda v: v["projects"][1]["policy"]["files"].update(current_user_read_only=False))
        changed("admin-default-identity", lambda v: v["projects"][0]["policy"]["execution"].update(default_identity="administrator"))
        changed("admin-on-restricted-project", lambda v: v["projects"][1]["policy"]["execution"].update(administrator_allowed=True))
        changed("admin-with-restricted-identity", lambda v: v["projects"][0]["policy"]["execution"].update(default_identity="restricted", administrator_allowed=True))
        changed("self-reported-backend", lambda v: v["projects"][0]["policy"]["execution"].update(restricted_backend_verified=True))
        changed("runtime-on-account-identity", lambda v: v["projects"][0]["policy"]["execution"].update(runtime_files=["C:/runtime/native.exe"]))
        changed("runtime-wrong-type", lambda v: v["projects"][1]["policy"]["execution"].update(runtime_files="C:/runtime/native.exe"))
        changed("runtime-relative-path", lambda v: v["projects"][1]["policy"]["execution"].update(runtime_files=["native.exe"]))
        changed("runtime-duplicate-path", lambda v: v["projects"][1]["policy"]["execution"].update(runtime_files=["C:/runtime/native.exe"]*2))
        changed("runtime-resource-budget", lambda v: v["projects"][1]["policy"]["execution"].update(runtime_files=["C:/runtime/native"+str(i)+".exe" for i in range(17)]))
        changed("unknown-tool", lambda v: v["projects"][0]["policy"].update(tools=["approve"]))
        changed("duplicate-tool", lambda v: v["projects"][0]["policy"].update(tools=["fs_read", "fs_read"]))
        changed("unknown-default-project", lambda v: v["gateway"].update(default_project_id="missing"))
        changed("invalid-port", lambda v: v["gateway"].update(port=80))
        changed("gateway-credential", lambda v: v["gateway"].update(token="literal-credential"))
        changed("ttl-zero", lambda v: v["projects"][0].update(approval_ttl_seconds=0))
        changed("audit-budget-large", lambda v: v["projects"][0].update(audit_max_bytes=67108865))
        changed("literal-token", lambda v: v["projects"][0].update(token="credential"))
        for label, value in cases:
            validate(value, False, "reject-" + label)
        raw_path = folder / "duplicates.json"
        for raw in (json.dumps(example).replace('"format_version": 1', '"format_version": 1, "format_version": 1', 1),
                    json.dumps(example).replace('"approval_mode": "never"', '"approval_mode": "never", "approval_mode": "always"', 1),
                    json.dumps(example).replace('"read_only": false', '"read_only": false, "read_only": true', 1),
                    json.dumps(example).replace('"scope": "current_user"', '"scope": "current_user", "scope": "configured"', 1),
                    '{"format_version":1,"projects":[],"format_version\\u0000":1}',
                    json.dumps(example) + "\n{}"):
            raw_path.write_text(raw, encoding="utf-8")
            run(["--validate-projects", str(raw_path)], False)
        too_many = copy.deepcopy(minimal)
        too_many["projects"] *= 17
        validate(too_many, False, "excess-projects")
        too_many = copy.deepcopy(example)
        too_many["projects"][0]["workspaces"] = [{"id": "w" + str(i), "path": "C:/w" + str(i)} for i in range(17)]
        validate(too_many, False, "excess-workspaces")
        run(["--validate-projects", str(raw_path), "--http"], False)

        root = folder / "workspace 中文"
        root.mkdir()
        old_data = folder / "old-data"
        old_data.mkdir()
        sentinel = old_data / "keep.bin"
        sentinel.write_bytes(b"M1 must preserve old Data\x00\xff")
        old_data_bytes = sentinel.read_bytes()
        for profile in ("controlled", "user", "administrator"):
            for enabled in (False, True):
                legacy = {"roots": [{"id": "workspace", "path": str(root), "read_only": False}],
                          "http": {"port": 8766, "token_env": "LEGACY_TOKEN", "public_url": ""},
                          "data_dir": str(old_data), "approval_ttl_seconds": 60, "audit_max_bytes": 16384,
                          "execution": {"enabled": enabled, "profile": profile}}
                if profile == "controlled":
                    legacy["execution"]["approval_mode"] = "rules" if enabled else "always"
                path = folder / ("legacy-" + profile + "-" + str(enabled) + ".json")
                path.write_text(json.dumps(legacy, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
                migrated_policy_checks(profile, enabled, path, legacy)
        check(sentinel.read_bytes() == old_data_bytes and sorted(p.name for p in old_data.iterdir()) == ["keep.bin"], "all migrations leave old Data untouched")
        path = folder / "legacy-user-True.json"
        destination = folder / "reuse-old-data.json"
        run(["--migrate-config", str(path), "--output", str(destination), "--project-id", "legacy-default", "--project-data-dir", str(old_data)], False)
        check(not destination.exists() and not Path(str(destination) + ".legacy-backup.json").exists(), "old Data reuse refused before output creation")
        destination = folder / "backup-conflict.json"
        conflict_backup = Path(str(destination) + ".legacy-backup.json")
        conflict_backup.write_bytes(b"preserve existing backup")
        run(["--migrate-config", str(path), "--output", str(destination), "--project-id", "legacy-default", "--project-data-dir", str(folder / "new-conflict-data")], False)
        check(not destination.exists() and conflict_backup.read_bytes() == b"preserve existing backup", "backup conflict preserves existing file and removes only newly created output")
        run(["--migrate-config", str(path), "--output", str(path), "--project-id", "legacy-default", "--project-data-dir", str(folder / "new-source-conflict")], False)
        check(json.loads(path.read_bytes())["execution"]["profile"] == "user", "migration cannot overwrite source")
        invalid = folder / "invalid-legacy.json"
        invalid.write_text('{"execution":{"enabled":true,"profile":"user","approval_mode":"never"}}', encoding="utf-8")
        run(["--migrate-config", str(invalid), "--output", str(folder / "invalid-output.json"), "--project-id", "legacy-default", "--project-data-dir", str(folder / "new-invalid-data")], False)
        check(not (folder / "invalid-output.json").exists(), "invalid legacy combination not migrated")
        migrated = json.loads((folder / "migrated-user-True.json").read_bytes())
        migrated["projects"][0]["policy"] = {"approval_mode": "never"}
        validate(migrated, False, "legacy-policy-override")
        for artifact, key in ((executable, "artifact"), (policy_executable, "policy_test_artifact")):
            check(receipt(artifact)["sha256"] == report[key]["sha256"], "verified executable unchanged during tests")
        report["status"] = "passed"
    except BaseException as error:
        report["status"] = "failed"
        report["error_type"] = type(error).__name__
        raise
    finally:
        report["assertions_passed"] = checks + native_checks
        report["cli_assertions_passed"] = checks
        report["native_policy_assertions_passed"] = native_checks
        report["completed_at_utc"] = datetime.now(timezone.utc).isoformat()
        contents = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        (folder / "report.json").write_text(contents, encoding="utf-8")
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(contents, encoding="utf-8")
    print(f"PASS M1: {checks + native_checks} assertions; configuration/policy only, no project runtime or UAC.")


if __name__ == "__main__":
    main()
