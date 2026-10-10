"""M2 real ordinary-token workers. Never starts a legacy administrator host/UAC."""
import argparse
import base64
import concurrent.futures
import copy
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    exe = Path(args.exe).resolve()
    fixture = exe.with_name('ShellBot-TestFixture.exe')
    repo = Path(__file__).resolve().parents[1]
    folder = repo / 'build' / 'm2-tests' / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    report = {'stage': 'M2', 'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'artifacts': str(folder), 'uac_attempted': False, 'manual_tests': 'deferred', 'checks': [], 'passed': False}
    managers = []
    unrelated = None
    seq = 0
    flags = subprocess.CREATE_NO_WINDOW
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    def alive(pid):
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == 258
        finally:
            kernel.CloseHandle(handle)

    def terminate_owned(pid):
        # Only a worker pid returned by this test's manager. No name-based cleanup.
        handle = kernel.OpenProcess(1 | 0x100000, False, pid)
        if not handle:
            raise AssertionError('owned worker handle unavailable')
        try:
            if not kernel.TerminateProcess(handle, 77):
                raise AssertionError('owned worker termination failed')
            kernel.WaitForSingleObject(handle, 3000)
        finally:
            kernel.CloseHandle(handle)

    def check(condition, name):
        report['checks'].append({'name': name, 'passed': bool(condition)})
        if not condition:
            raise AssertionError(name)

    def save(name, value):
        path = folder / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
        return path

    def cli(*arguments, success=True):
        p = subprocess.run([str(exe), *map(str, arguments)], capture_output=True, text=True, encoding='utf-8', timeout=35, creationflags=flags)
        check((p.returncode == 0) == success, 'CLI ' + str(arguments[0]) + (' succeeds' if success else ' rejects'))
        return json.loads(p.stdout) if p.stdout.strip() else None

    data = folder / 'manager-data'

    def control(command, project=None, request=None, manager_data=data, success=True):
        nonlocal seq
        arguments = [command]
        if project:
            arguments.append(project)
        if request is not None:
            # Thread-unique file names; bounded owner CLI transport test.
            path = save('request-' + uuid.uuid4().hex + '.json', request)
            arguments += ['--request', path]
        return cli(*arguments, '--manager-data-dir', manager_data, success=success)

    def rpc(project, method, arguments=None, name=None, manager_data=data):
        params = {'_meta': {'io.modelcontextprotocol/protocolVersion': '2026-07-28', 'io.modelcontextprotocol/clientCapabilities': {}}}
        if name:
            params.update(name=name, arguments=arguments or {})
        return control('--project-request', project, {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}, manager_data=manager_data)['response']

    def tool(project, name, arguments=None, failed=False, manager_data=data):
        response = rpc(project, 'tools/call', arguments, name, manager_data)
        check('result' in response and response['result']['isError'] == failed, project + ' ' + name + (' blocked' if failed else ' succeeds'))
        return response['result']['structuredContent']

    def await_task(project, task, instance, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = tool(project, 'task_status', {'task_id': task, 'host_instance_id': instance})
            if value['status'] not in ('running', 'starting'):
                return value
            time.sleep(.07)
        raise AssertionError('task completion deadline exceeded')

    def launch(registry, manager_data=data):
        p = subprocess.Popen([str(exe), '--manager', '--projects', str(registry), '--manager-data-dir', str(manager_data)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8', creationflags=flags,
                             env={**os.environ, 'SHELLBOT_A_TOKEN': 'test-a-private', 'SHELLBOT_B_TOKEN': 'test-b-private',
                                  'SHELLBOT_M2_CANARY': 'test-worker-exclusion'})
        managers.append((p, manager_data))
        ready = p.stdout.readline()
        check(ready and json.loads(ready)['manager_ready'], 'manager starts without elevation')
        return p

    try:
        tools = ['host_info', 'host_capabilities', 'fs_list', 'fs_read', 'fs_search', 'image_view', 'fs_write', 'fs_patch',
                 'task_start', 'task_status', 'task_output', 'task_input', 'task_cancel']
        projects = []
        for name in ('a', 'b', 'confirm', 'restricted', 'auto'):
            workspace = folder / ('workspace-' + name)
            workspace.mkdir()
            (workspace / 'unique.txt').write_text('content-' + name, encoding='utf-8')
            projects.append({'id': name, 'token_env': 'SHELLBOT_' + name.upper() + '_TOKEN', 'data_dir': str(folder / ('data-' + name)),
                             'workspaces': [{'id': 'same-workspace', 'path': str(workspace), 'read_only': False}],
                             'policy': {'approval_mode': 'always' if name == 'confirm' else 'workspace_auto' if name == 'auto' else 'never',
                                        'files': {'scope': 'configured'} if name in ('restricted', 'auto') else {'scope': 'current_user', 'current_user_read_only': False},
                                        'execution': {'enabled': True, 'default_identity': 'restricted' if name == 'restricted' else 'current_user',
                                                      'administrator_allowed': name == 'a'}, 'tools': tools},
                             'resources': {'max_active_tasks': 1, 'max_task_records': 2 if name == 'b' else 12, 'max_output_bytes': 4096, 'max_timeout_ms': 60000 if name == 'b' else 10000,
                                           'max_approval_records': 2, 'worker_memory_mb': 128}})
            if name == 'auto':
                extra = folder / 'auto-extra'
                extra.mkdir()
                (extra / 'secret.txt').write_text('extra-directory-body', encoding='utf-8')
                projects[-1]['extra_directories'] = [{'id': 'extra', 'path': str(extra), 'read_only': True}]
                projects[-1]['policy']['tools'] = [tool_name for tool_name in tools if tool_name != 'fs_search']
        definitions = [save('project-' + p['id'] + '.json', p) for p in projects]
        current = None
        for p, definition in zip(projects, definitions):
            output = folder / ('registry-' + p['id'] + '.json')
            more = ['--projects', current] if current else []
            cli('--project-create', p['id'], '--project-config', definition, '--output', output, *more)
            check(output.with_name(output.name + '.source-backup.json').exists(), 'configuration source backup preserved')
            current = output
        registry_before = current.read_bytes()
        cli('--project-create', 'a', '--project-config', definitions[0], '--projects', current, '--output', folder / 'duplicate.json', success=False)
        cli('--project-create', 'a', '--project-config', definitions[0], '--output', current, success=False)
        check(current.read_bytes() == registry_before, 'existing registry unchanged after rejected exports')
        configured = folder / 'configured.json'
        cli('--project-configure', 'a', '--project-config', definitions[0], '--projects', current, '--output', configured)
        updated = json.loads(configured.read_text(encoding='utf-8'))
        check(updated['projects'][0]['policy_revision'] == 2, 'configuration increments trusted revision')
        check(configured.with_name(configured.name + '.source-backup.json').read_bytes() == registry_before, 'backup contains exact previous registry bytes')
        for field, value in [('max_active_tasks', 9), ('max_task_records', 0), ('max_output_bytes', 262145), ('max_timeout_ms', 99),
                             ('max_approval_records', 65), ('worker_memory_mb', 15), ('unknown', 1)]:
            invalid = copy.deepcopy(updated)
            invalid['projects'][0]['resources'][field] = value
            cli('--validate-projects', save('invalid-' + field + '.json', invalid), success=False)
        invalid = copy.deepcopy(updated)
        invalid['projects'][0]['resources'].update(max_task_records=1, max_active_tasks=2)
        cli('--validate-projects', save('invalid-record-ratio.json', invalid), success=False)
        cli('--project-worker', '1', str(os.getpid()), success=False)
        # Physical aliases cannot reuse a project's private Data.
        (folder / 'data-a').mkdir()
        alias = folder / 'data-alias'
        linked = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(folder / 'data-a')], capture_output=True, creationflags=flags)
        check(linked.returncode == 0, 'physical Data alias fixture created: ' + (linked.stdout + linked.stderr).decode(errors='replace').strip())
        alias_registry = copy.deepcopy(updated)
        alias_registry['projects'][1]['data_dir'] = str(alias)
        cli('--manager', '--projects', save('physical-data-alias.json', alias_registry), '--manager-data-dir', folder / 'rejected-manager', success=False)
        protected = copy.deepcopy(updated)
        protected['projects'][3]['workspaces'][0]['path'] = str(folder / 'protected-manager')
        cli('--manager', '--projects', save('protected-data.json', protected), '--manager-data-dir', folder / 'protected-manager', success=False)
        m = launch(configured)
        check(len(control('--project-status')['projects']) == 5, 'registered projects begin stopped')
        for name in ('a', 'b', 'confirm', 'restricted', 'auto'):
            control('--project-start', name)
        on_disk = configured.read_bytes()
        changed = copy.deepcopy(updated)
        changed['projects'][0]['policy_revision'] = 100
        changed['projects'][0]['enabled'] = False
        try:
            configured.write_text(json.dumps(changed), encoding='utf-8')
            blocked=False
        except PermissionError:blocked=True
        check(blocked and configured.read_bytes()==on_disk,'active registry is pinned against disk edits')
        check(control('--project-status', 'a')['project']['policy_revision'] == 2, 'active manager retains the original policy revision')
        control('--project-start', 'missing', success=False)
        statuses = control('--project-status')
        by_id = {p['project_id']: p for p in statuses['projects']}
        check(len({p['worker_pid'] for p in by_id.values()}) == 5 and all(p['state'] == 'running' for p in by_id.values()), 'five projects have independent live worker processes')
        check(not statuses['elevated'] and not statuses['http_gateway'], 'manager is ordinary and M3 gateway remains unavailable')
        for name in ('a', 'b'):
            caps = tool(name, 'host_capabilities')
            check(caps['project_id'] == name and not caps['administrator_backend'], 'capabilities bound to project without administrator startup')
            check(caps['max_active_tasks'] == 1 and caps['max_output_bytes'] == 4096, 'project budgets reported')
            check(caps['execution_status']['max_active_tasks']==1 and caps['execution_status']['max_output_bytes_per_stream']==4096,'execution status uses project budgets')
            check(tool(name, 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt'})['text'] == 'content-' + name, 'same workspace id resolves inside its own project')
        tool('a', 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt', 'project_id': 'b'}, failed=True)
        tool('confirm', 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt'}, failed=True)
        tool('confirm', 'fs_search', {'workspace_id': 'same-workspace', 'query': 'content'}, failed=True)
        tool('confirm', 'image_view', {'workspace_id': 'same-workspace', 'path': 'unique.txt'}, failed=True)
        check(tool('auto', 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt'})['text'] == 'content-auto', 'workspace_auto permits its authorized workspace read')
        denied = tool('auto', 'fs_read', {'workspace_id': 'extra', 'path': 'secret.txt'}, failed=True)
        check('extra-directory-body' not in json.dumps(denied), 'pending extra-directory read reveals no file body')
        tool('auto', 'fs_write', {'workspace_id': 'extra', 'path': 'blocked.txt', 'text': 'blocked'}, failed=True)
        tool('auto', 'fs_search', {'workspace_id': 'same-workspace', 'query': 'content'}, failed=True)
        listed = rpc('auto', 'tools/list')['result']['tools']
        check('fs_search' not in {item['name'] for item in listed}, 'tool listing respects the worker project tool switches')
        tool('restricted', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['wait'], 'request_key': 'denied', 'host_instance_id': by_id['restricted']['host_instance_id']}, failed=True)
        tool('a', 'fs_write', {'workspace_id': 'same-workspace', 'path': 'm4-auto.txt', 'text': 'automatic write'})
        check((folder / 'workspace-a' / 'm4-auto.txt').read_text() == 'automatic write', 'never automatic write preserves configured project scope')
        for name in ('a', 'b'):
            instance = by_id[name]['host_instance_id']
            request = {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['append'], 'request_key': 'same-key', 'host_instance_id': instance}
            task = tool(name, 'task_start', request)
            check(task['created_new'], 'same idempotency key starts independently in ' + name)
            await_task(name, task['task_id'], instance)
            replay = tool(name, 'task_start', request)
            check(replay['task_id'] == task['task_id'] and not replay['created_new'], 'project replay returns its own receipt')
            check((folder / ('workspace-' + name) / 'once.txt').read_bytes() == b'ONE', 'idempotent task side effect occurs once')
            by_id[name]['task'] = task['task_id']
        tool('b', 'task_status', {'task_id': by_id['a']['task'], 'host_instance_id': by_id['b']['host_instance_id']}, failed=True)
        tool('b', 'task_output', {'task_id': by_id['a']['task'], 'host_instance_id': by_id['b']['host_instance_id']}, failed=True)
        tool('b', 'task_cancel', {'task_id': by_id['a']['task'], 'host_instance_id': by_id['b']['host_instance_id']}, failed=True)
        tool('b', 'task_status', {'task_id': by_id['b']['task'], 'host_instance_id': by_id['a']['host_instance_id']}, failed=True)
        instance = by_id['a']['host_instance_id']
        def start(name, key, mode='wait', timeout=10000):
            return tool(name, 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': [mode],
                                            'request_key': key, 'host_instance_id': by_id[name]['host_instance_id'], 'timeout_ms': 30000 if name == 'b' and timeout == 10000 else timeout})
        identity = start('a', 'identity', 'identity')
        await_task('a', identity['task_id'], instance)
        output = tool('a', 'task_output', {'task_id': identity['task_id'], 'host_instance_id': instance})
        actual = json.loads(base64.b64decode(output['data_base64']))
        check(not actual['elevated'] and not actual['project_tokens_inherited'] and not actual['canary_inherited'], 'ordinary task does not inherit project credentials or unknown environment secrets')
        check(Path(actual['cwd'].removeprefix('\\\\?\\')).resolve() == folder / 'workspace-a', 'task cwd belongs to its own worker')
        excessive = start('a', 'output-limit', 'escaped')
        exceeded = await_task('a', excessive['task_id'], instance)
        check(exceeded['stdout_bytes'] <= 4096 and exceeded['status'] == 'output_limit', 'output quota terminates only the overflowing task')
        timed = start('a', 'short-timeout', timeout=150)
        check(await_task('a', timed['task_id'], instance)['status'] == 'timed_out', 'project task deadline enforced')
        tool('a', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['wait'], 'request_key': 'over-timeout', 'host_instance_id': instance, 'timeout_ms': 10001}, failed=True)
        waits = {name: start(name, 'active-' + name) for name in ('a', 'b')}
        tool('a', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['wait'], 'request_key': 'over-active', 'host_instance_id': instance}, failed=True)
        check(alive(waits['a']['process_id']) and alive(waits['b']['process_id']), 'project tasks run concurrently')
        unrelated = subprocess.Popen([str(fixture), 'wait'], creationflags=flags)
        control('--project-stop', 'a')
        check(not alive(waits['a']['process_id']) and alive(waits['b']['process_id']) and unrelated.poll() is None, 'stopping A kills only its owned processes; B and unrelated process survive')
        bpid = by_id['b']['worker_pid']
        control('--project-start', 'a')
        anew = control('--project-status', 'a')['project']
        check(anew['host_instance_id'] != instance and anew['worker_pid'] != by_id['a']['worker_pid'], 'explicit restart uses a new project instance')
        by_id['a'] = anew
        tool('a', 'task_status', {'task_id': waits['a']['task_id'], 'host_instance_id': instance}, failed=True)
        restarted = start('a', 'crash-descendant')
        terminate_owned(anew['worker_pid'])
        check(control('--project-status', 'a')['project']['state'] == 'failed', 'crashed worker is reported failed without automatic restart')
        check(not alive(restarted['process_id']) and alive(bpid) and alive(waits['b']['process_id']) and unrelated.poll() is None, 'A crash cleans its own task while B remains live')
        control('--project-start', 'a')
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            responses = list(pool.map(lambda name: tool(name, 'host_info')['project_id'], ('a', 'b', 'confirm', 'restricted')))
        check(responses == ['a', 'b', 'confirm', 'restricted'], 'bounded concurrent owner requests retain project context')
        # Approval queues/backups remain independently hosted for explicitly migrated legacy projects.
        old_data = folder / 'old-data'
        old_data.mkdir()
        (old_data / 'preserved.txt').write_text('old-data', encoding='utf-8')
        legacy = {'roots': [{'id': 'same-workspace', 'path': str(folder / 'workspace-a'), 'read_only': False}],
                  'data_dir': str(old_data), 'execution': {'enabled': True, 'profile': 'administrator'}}
        migrated = folder / 'legacy-admin.json'
        cli('--migrate-config', save('old-admin.json', legacy), '--output', migrated, '--project-id', 'legacy-admin', '--project-data-dir', folder / 'data-legacy-admin')
        legacy_manager_data = folder / 'legacy-manager'
        legacy_registry = json.loads(migrated.read_text(encoding='utf-8'))
        for name in ('legacy-one', 'legacy-two'):
            workspace = folder / name
            workspace.mkdir()
            (workspace / 'edit.txt').write_text('before', encoding='utf-8')
            config = copy.deepcopy(legacy)
            config['roots'][0]['path'] = str(workspace)
            config['execution'] = {'enabled': False, 'profile': 'controlled', 'approval_mode': 'always'}
            config['http'] = {'token_env': name.replace('-', '_').upper() + '_TOKEN'}
            legacy_registry['projects'].append({'id': name, 'data_dir': str(folder / ('data-' + name)), 'legacy_config': config})
        legacy_combined = save('legacy-combined.json', legacy_registry)
        lm = launch(legacy_combined, legacy_manager_data)
        control('--project-start', 'legacy-admin', manager_data=legacy_manager_data)
        admin_status = control('--project-status', 'legacy-admin', manager_data=legacy_manager_data)['project']
        check(admin_status['state'] == 'running' and not admin_status['administrator_backend'], 'migrated administrator project starts only an ordinary worker without UAC')
        check(tool('legacy-admin', 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt'}, manager_data=legacy_manager_data)['text'] == 'content-a', 'legacy administrator configuration retains ordinary file reading without elevation')
        tool('legacy-admin', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['identity'],
                                          'request_key': 'no-admin', 'host_instance_id': admin_status['host_instance_id']}, failed=True, manager_data=legacy_manager_data)
        check((old_data / 'preserved.txt').read_text() == 'old-data', 'legacy Data remains untouched')
        approvals = {}
        edit_args = {'workspace_id': 'same-workspace', 'path': 'edit.txt', 'text': 'after', 'overwrite': True,
                     'expected_sha256': hashlib.sha256(b'before').hexdigest()}
        for name in ('legacy-one', 'legacy-two'):
            control('--project-start', name, manager_data=legacy_manager_data)
            pending = tool(name, 'fs_write', edit_args, failed=True, manager_data=legacy_manager_data)
            approvals[name] = pending['approval_id']
            check(pending['status'] == 'pending' and (folder / name / 'edit.txt').read_text() == 'before', 'legacy write remains pending without effects')
        foreign = control('--project-owner', 'legacy-two', {'command': 'inspect', 'id': approvals['legacy-one']}, manager_data=legacy_manager_data)['response']
        check('error' in foreign, 'approval id from another project cannot be inspected')
        tool('legacy-two', 'fs_write', {**edit_args, 'approval_id': approvals['legacy-one']}, failed=True, manager_data=legacy_manager_data)
        for name in ('legacy-one', 'legacy-two'):
            own = control('--project-owner', name, {'command': 'inspect', 'id': approvals[name]}, manager_data=legacy_manager_data)['response']
            check(own['approval_id'] == approvals[name], 'owner inspection is routed to its project queue')
            control('--project-owner', name, {'command': 'approve', 'id': approvals[name]}, manager_data=legacy_manager_data)
            applied = tool(name, 'fs_write', {**edit_args, 'approval_id': approvals[name]}, manager_data=legacy_manager_data)
            check(applied['status'] == 'completed' and (folder / name / 'edit.txt').read_text() == 'after', 'approved legacy edit applies only inside its own project')
            backups = list((folder / ('data-' + name)).glob('backup-*.bin'))
            check(len(backups) == 1 and backups[0].read_bytes() == b'before', 'private original backup stored inside its own project Data')
        # B has retained both task receipts. Cancelling frees concurrency, never receipt capacity.
        tool('b', 'task_cancel', {'task_id': waits['b']['task_id'], 'host_instance_id': by_id['b']['host_instance_id']})
        await_task('b', waits['b']['task_id'], by_id['b']['host_instance_id'])
        tool('b', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['identity'],
                               'request_key': 'record-overflow', 'host_instance_id': by_id['b']['host_instance_id']}, failed=True)
        check(control('--project-status', 'b')['project']['state'] == 'running', 'record quota failure leaves project worker running')

        control('--manager-stop', manager_data=legacy_manager_data)
        lm.wait(timeout=10)
        control('--manager-stop')
        m.wait(timeout=10)
        check(not alive(bpid) and not alive(waits['b']['process_id']) and unrelated.poll() is None, 'manager shutdown cleans owned workers and task trees without affecting unrelated processes')
        check(all((folder / ('data-' + name)).exists() for name in ('a', 'b')), 'project Data stored separately')
        manager_events = [json.loads(line) for path in data.glob('audit-*.jsonl') for line in path.read_text(encoding='utf-8').splitlines()]
        check(any(row.get('event') == 'project_stopped' for row in manager_events), 'manager lifecycle audit records project stop')
        check(any(row.get('event') == 'project_failed' for row in manager_events), 'manager lifecycle audit records project failure')
        check(any(row.get('project_id') == 'a' for row in manager_events) and any(row.get('project_id') == 'b' for row in manager_events), 'lifecycle records contain explicit project bindings')
        for name, other in [('a', 'b'), ('b', 'a')]:
            rows = [json.loads(line) for path in (folder / ('data-' + name)).glob('audit-*.jsonl') for line in path.read_text(encoding='utf-8').splitlines()]
            task_ids = {r.get('task_id') for r in rows if r.get('task_id')}
            check(not (by_id.get(other, {}).get('task') in task_ids), 'task audit receipts do not cross ' + name)
        # Abrupt manager loss closes its Job handles, including descendant jobs.
        crash_data = folder / 'crash-manager'
        crash_manager = launch(configured, crash_data)
        for name in ('a', 'b'):
            control('--project-start', name, manager_data=crash_data)
        crash_workers = {p['project_id']: p for p in control('--project-status', manager_data=crash_data)['projects']}
        crash_tasks = []
        for name in ('a', 'b'):
            receipt = tool(name, 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['wait'],
                                               'request_key': 'manager-crash', 'host_instance_id': crash_workers[name]['host_instance_id'],
                                               'timeout_ms': 10000}, manager_data=crash_data)
            crash_tasks.append(receipt['process_id'])
        crash_manager.kill()
        crash_manager.wait(timeout=8)
        deadline = time.monotonic() + 5
        owned = [crash_workers[name]['worker_pid'] for name in ('a', 'b')] + crash_tasks
        while time.monotonic() < deadline and any(alive(pid) for pid in owned):
            time.sleep(.05)
        check(all(not alive(pid) for pid in owned) and unrelated.poll() is None, 'manager crash closes only its owned worker and descendant Jobs')
        report['passed'] = True
    except Exception as exc:
        report['error'] = type(exc).__name__ + ': ' + str(exc)
        raise
    finally:
        for process, manager_data in managers:
            if process.poll() is None:
                try:
                    control('--manager-stop', manager_data=manager_data)
                    process.wait(timeout=12)
                except Exception:
                    process.kill()
                    process.wait(timeout=8)
            errors = process.stderr.read()
            if errors:
                report.setdefault('manager_diagnostics', []).append(errors[:2000])
        if unrelated and unrelated.poll() is None:
            unrelated.terminate()
            unrelated.wait(timeout=8)
        report['assertions'] = len(report['checks'])
        destination = Path(args.report)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
        print(json.dumps({'passed': report['passed'], 'assertions': report['assertions'], 'report': str(destination)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
