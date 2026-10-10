"""M3 real loopback routes, bearer isolation and bounded worker dispatch.

Uses fresh registries/Data and fixed native fixtures. No UAC, DNS, tunnel or
real MCP client acceptance. Credentials never appear in files or receipts.
"""
import argparse
import base64
import concurrent.futures
import copy
import ctypes
from datetime import datetime, timezone
import hashlib
from http import client as http_client
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    fixture = exe.with_name('ShellBot-TestFixture.exe')
    folder = Path(__file__).resolve().parents[1] / 'build' / 'm3-tests' / (datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    report = {'stage': 'M3', 'started_at_utc': datetime.now(timezone.utc).isoformat(), 'exe': str(exe),
              'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'artifacts': str(folder), 'checks': [],
              'uac_attempted': False, 'real_mcp_client': 'pending', 'public_tunnel': 'pending', 'passed': False}
    flags = subprocess.CREATE_NO_WINDOW
    tokens = {name: secrets.token_hex(32) for name in ('a', 'b', 'legacy')}
    managers = []
    slow = None
    data = folder / 'manager-data'
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    native = ctypes.WinDLL('ntdll')
    native.NtSuspendProcess.argtypes = [ctypes.c_void_p]

    def check(condition, label):
        report['checks'].append({'name': label, 'passed': bool(condition)})
        if not condition:
            raise AssertionError(label)

    def save(name, value):
        path = folder / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
        return path

    def alive(pid):
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == 258
        finally:
            kernel.CloseHandle(handle)

    def suspend_owned(pid):
        # Exact worker receipt from this test's manager, never process-name cleanup.
        handle = kernel.OpenProcess(0x800 | 0x100000, False, pid)
        if not handle:
            raise AssertionError('owned worker suspension handle unavailable')
        try:
            check(native.NtSuspendProcess(handle) == 0, 'owned test worker suspended for deadline validation')
        finally:
            kernel.CloseHandle(handle)

    def control(command, project=None, request=None, manager_data=data, success=True):
        arguments = [command] + ([project] if project else [])
        if request is not None:
            arguments += ['--request', str(save('owner-' + uuid.uuid4().hex + '.json', request))]
        p = subprocess.run([str(exe), *arguments, '--manager-data-dir', str(manager_data)],
                           capture_output=True, timeout=35, creationflags=flags)
        check((p.returncode == 0) == success, command + (' succeeds' if success else ' rejects'))
        return json.loads(p.stdout) if p.stdout.strip() else None

    def modern(method, name=None, arguments=None):
        params = {'_meta': {'io.modelcontextprotocol/protocolVersion': '2026-07-28', 'io.modelcontextprotocol/clientCapabilities': {}}}
        if name:
            params.update(name=name, arguments=arguments or {})
        return {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}

    def headers(payload=None, token=None):
        result = {'Authorization': 'Bearer ' + (tokens['a'] if token is None else token), 'Content-Type': 'application/json',
                  'Accept': 'application/json', 'MCP-Protocol-Version': '2026-07-28'}
        if isinstance(payload, dict):
            result['Mcp-Method'] = payload.get('method', '')
            if payload.get('method') == 'tools/call':
                result['Mcp-Name'] = payload.get('params', {}).get('name', '')
        return result

    def http(path='/projects/a/mcp', payload=None, token=None, verb='POST', changes=None, expected=200, raw=None):
        payload = modern('ping') if payload is None else payload
        body = raw if raw is not None else json.dumps(payload, ensure_ascii=False).encode('utf-8')
        fields = headers(payload, token)
        for name, value in (changes or {}).items():
            if value is None:
                fields.pop(name, None)
            else:
                fields[name] = value
        connection = http_client.HTTPConnection('127.0.0.1', port, timeout=15)
        try:
            connection.request(verb, path, body=body if verb != 'GET' else None, headers=fields)
            response = connection.getresponse()
            received = response.read()
            check(response.status == expected, 'HTTP ' + verb + ' ' + path + ' returns ' + str(expected))
            check(response.getheader('Cache-Control') == 'no-store', 'gateway replies are not cacheable')
            if expected == 401:
                check(response.getheader('WWW-Authenticate', '').startswith('Bearer '), 'project authentication challenge')
            return json.loads(received) if received else None
        finally:
            connection.close()

    def tool(project, name, arguments=None, failed=False):
        response = http('/projects/' + project + '/mcp', modern('tools/call', name, arguments), tokens[project])
        if response.get('result', {}).get('isError') != failed:
            print(json.dumps(response, ensure_ascii=False))
        check(response.get('result', {}).get('isError') == failed, project + ' ' + name + (' denied' if failed else ' succeeds'))
        return response['result']['structuredContent']

    def await_task(project, task):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            result = tool(project, 'task_status', {'task_id': task['task_id'], 'host_instance_id': task['host_instance_id']})
            if result['status'] not in ('starting', 'running'):
                return result
            time.sleep(.07)
        raise AssertionError('native fixture task deadline exceeded')

    def launch(registry, manager_data=data, credentials=None, success=True):
        env = {**os.environ, 'SHELLBOT_A_TOKEN': tokens['a'], 'SHELLBOT_B_TOKEN': tokens['b'],
               'SHELLBOT_LEGACY_TOKEN': tokens['legacy'], 'SHELLBOT_M2_CANARY': 'gateway-exclusion-canary', **(credentials or {})}
        p = subprocess.Popen([str(exe), '--manager', '--http', '--projects', str(registry), '--manager-data-dir', str(manager_data)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=flags, env=env)
        managers.append((p, manager_data))
        if success:
            ready = p.stdout.readline()
            check(ready and json.loads(ready).get('http_gateway') is True, 'ordinary manager announces project HTTP readiness')
        else:
            out, err = p.communicate(timeout=15)
            check(p.returncode == 2 and not out and err, 'gateway startup fails closed with invalid credentials or occupied port')
            check(not any(value.encode() in err for value in tokens.values()), 'startup diagnostics omit bearer values')
        return p

    def receive_raw(connection):
        head = b''
        while b'\r\n\r\n' not in head:
            piece = connection.recv(4096)
            if not piece:
                raise AssertionError('raw HTTP reply header missing')
            head += piece
        header, body = head.split(b'\r\n\r\n', 1)
        length = next(int(line.split(b':', 1)[1]) for line in header.split(b'\r\n') if line.lower().startswith(b'content-length:'))
        while len(body) < length:
            piece = connection.recv(min(4096, length - len(body)))
            if not piece:
                raise AssertionError('raw HTTP reply body incomplete')
            body += piece
        return int(header.split(b' ')[1]), body[:length]

    def wait_busy():
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            state = control('--project-status', success=True)['projects']
            if next(p['state'] for p in state if p['project_id'] == 'a') == 'busy':
                return
            time.sleep(.04)
        raise AssertionError('test request never reserved project A')

    try:
        projects = []
        tools = ['host_info', 'host_capabilities', 'fs_list', 'fs_read', 'fs_search', 'image_view', 'fs_write', 'fs_patch',
                 'task_start', 'task_status', 'task_output', 'task_input', 'task_cancel']
        for name in ('a', 'b', 'legacy', 'disabled'):
            workspace = folder / ('workspace-' + name)
            workspace.mkdir()
            (workspace / 'unique.txt').write_text('content-' + name, encoding='utf-8')
            if name == 'legacy':
                projects.append({'id': name, 'data_dir': str(folder / ('data-' + name)),
                                 'legacy_config': {'roots': [{'id': 'same-workspace', 'path': str(workspace), 'read_only': False}],
                                                   'http': {'token_env': 'SHELLBOT_LEGACY_TOKEN'}}})
                continue
            projects.append({'id': name, 'enabled': name != 'disabled', 'token_env': 'SHELLBOT_' + name.upper() + '_TOKEN',
                             'data_dir': str(folder / ('data-' + name)), 'policy_revision': 7,
                             'workspaces': [{'id': 'same-workspace', 'path': str(workspace), 'read_only': False}],
                             'policy': {'approval_mode': 'never', 'files': {'scope': 'current_user', 'current_user_read_only': False},
                                        'execution': {'enabled': True, 'default_identity': 'current_user', 'administrator_allowed': False},
                                        'tools': tools if name != 'b' else [t for t in tools if t != 'fs_search']},
                             'resources': {'max_active_tasks': 4, 'max_task_records': 24, 'worker_memory_mb': 128}})
        value = {'format_version': 1, 'gateway': {'port': port, 'public_url': 'https://m3.invalid'}, 'projects': projects}
        registry = save('registry.json', value)
        original = registry.read_bytes()
        launch(registry, folder / 'bad-duplicate-data', {'SHELLBOT_B_TOKEN': tokens['a']}, False)
        for bad in ('', 'short', ' ' * 64, '\u00e9' * 64, 'x' * 257):
            launch(registry, folder / ('bad-credential-' + uuid.uuid4().hex), {'SHELLBOT_B_TOKEN': bad}, False)
        m = launch(registry)
        launch(registry, folder / 'occupied-port-data', success=False)
        http(expected=503)  # No automatic project startup from an authenticated MCP request.
        instances = {}
        workers = {}
        for name in ('a', 'b', 'legacy'):
            state = control('--project-start', name)['project']
            instances[name] = state['host_instance_id']
            workers[name] = state['worker_pid']
        manager_status = control('--project-status')
        check(manager_status['http_gateway'] and manager_status['http_workers'] == 4 and manager_status['http_connection_queue'] == 4,
              'global HTTP threads and accepted connection queue have explicit limits')
        links = control('--project-links', 'a')['links']
        check(links['local_mcp_url'] == f'http://127.0.0.1:{port}/projects/a/mcp' and
              links['public_mcp_url'] == 'https://m3.invalid/projects/a/mcp' and not links['default_route'], 'links use configured origin without DNS or implicit default')
        check(not any(token in json.dumps(links) for token in tokens.values()), 'owner link generation contains references but no bearer values')
        http('/mcp', expected=404)
        http('/healthz', verb='GET', expected=404)
        for path in ('/projects/missing/mcp', '/projects/A/mcp', '/projects/a/mcp/', '/projects/a/owner', '/projects/a/mcp/extra'):
            http(path, expected=404)
        for path in ('/projects/%61/mcp', '/projects/a%2fb/mcp', '/projects/b/../a/mcp', '/projects//a/mcp', '/projects/a/mcp?project_id=b', '/projects/a\\mcp'):
            http(path, expected=400)
        http('/projects/disabled/mcp', expected=503)
        for path, token in (('/projects/b/mcp', tokens['a']), ('/projects/a/mcp', tokens['b']), ('/projects/a/healthz', tokens['b'])):
            http(path, token=token, verb='GET' if path.endswith('healthz') else 'POST', expected=401)
        for bad in ('', 'invalid', tokens['a'][:-1], tokens['a'] + 'x'):
            http(token=bad, expected=401)
        http(changes={'Authorization': None}, expected=401)
        http(changes={'Authorization': 'Basic ' + tokens['a']}, expected=401)
        http('/projects/a/healthz', verb='GET')
        http('/projects/a/mcp', verb='GET', expected=405)
        http('/projects/a/mcp', verb='DELETE', expected=405)
        http('/projects/a/healthz', verb='POST', expected=405)
        http(changes={'Host': 'unconfigured.invalid'}, expected=403)
        http(changes={'Origin': 'https://unconfigured.invalid'}, expected=403)
        http(changes={'Host': 'm3.invalid', 'Origin': 'https://m3.invalid'})
        http(changes={'Content-Type': 'text/plain'}, expected=415)
        http(changes={'Accept': 'text/event-stream'}, expected=406)
        http(changes={'MCP-Protocol-Version': 'unsupported'}, expected=400)
        http(changes={'Mcp-Method': 'tools/list'}, expected=400)
        http(changes={'Mcp-Method': None}, expected=400)
        http(changes={'MCP-Protocol-Version': None}, expected=400)
        http(raw=b'not json', expected=400)
        http(raw=b'[]', expected=400)
        http(raw=b'{"jsonrpc":"2.0","id":1,"method":"ping","method":"tools/list"}', expected=400)
        http(raw=b'x' * (1024 * 1024 + 1), expected=413)
        discover = http(payload=modern('server/discover'))
        check(discover['result']['cacheScope'] == 'private', 'stateless discovery survives manager routing')
        notification = modern('notifications/initialized')
        notification.pop('id')
        check(http(payload=notification, expected=202) is None, 'notification retains empty HTTP 202 through worker framing')
        old = {'jsonrpc': '2.0', 'id': 4, 'method': 'initialize', 'params': {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'M3', 'version': '1'}}}
        check(http(payload=old, changes={'MCP-Protocol-Version': '2025-11-25'})['result']['protocolVersion'] == '2025-11-25', 'legacy initialization remains available on project HTTP')
        for name in ('a', 'b'):
            capabilities = tool(name, 'host_capabilities')
            check(capabilities['project_id'] == name and capabilities['policy_revision'] == 7, 'authenticated route binds immutable project and policy revision')
            read = tool(name, 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt'})
            check(read['text'] == 'content-' + name, 'identical workspace ids resolve inside ' + name)
        listed = http('/projects/b/mcp', modern('tools/list'), tokens['b'])['result']['tools']
        check('fs_search' not in [t['name'] for t in listed] and 'fs_read' in [t['name'] for t in listed], 'HTTP tool discovery applies the authenticated project switches')
        tool('a', 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt', 'project_id': 'b'}, failed=True)
        tool('a', 'fs_read', {'workspace_id': 'same-workspace', 'path': 'unique.txt', 'actor': 'manager-owner'}, failed=True)
        tool('b', 'fs_search', {'workspace_id': 'same-workspace', 'query': 'content'}, failed=True)
        encoded = '=?base64?' + base64.b64encode(b'host_info').decode() + '?='
        http(payload=modern('tools/call', 'host_info'), changes={'Mcp-Name': encoded})
        http(payload=modern('tools/call', 'host_info'), changes={'Mcp-Name': 'host_capabilities'}, expected=400)
        with socket.create_connection(('127.0.0.1', port), timeout=10) as duplicate:
            body = json.dumps(modern('ping')).encode()
            field_lines = ''.join(name + ': ' + content + '\r\n' for name, content in headers(modern('ping')).items())
            duplicate.sendall((f'POST /projects/a/mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n' + field_lines +
                               'Authorization: Bearer invalid\r\nContent-Length: ' + str(len(body)) + '\r\n\r\n').encode() + body)
            check(receive_raw(duplicate)[0] == 400, 'duplicate authorization headers rejected before routing')
        # Hold an authenticated A request while its body is still incomplete.
        slow = socket.create_connection(('127.0.0.1', port), timeout=10)
        body = json.dumps(modern('ping')).encode()
        field_lines = ''.join(name + ': ' + content + '\r\n' for name, content in headers(modern('ping')).items())
        slow.sendall((f'POST /projects/a/mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n' + field_lines +
                      'Content-Length: ' + str(len(body)) + '\r\n\r\n').encode() + body[:1])
        wait_busy()
        http(expected=429)
        start = time.monotonic()
        http('/projects/b/mcp', token=tokens['b'])
        check(time.monotonic() - start < 2, 'slow project A body does not block project B')
        control('--project-stop', 'a', success=False)
        slow.sendall(body[1:])
        check(receive_raw(slow)[0] == 200, 'reserved project request finishes after body delivery')
        slow.close()
        slow = None
        tasks = {}
        for name in ('a', 'b'):
            arguments = {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['append'], 'request_key': 'shared-key', 'host_instance_id': instances[name]}
            tasks[name] = tool(name, 'task_start', arguments)
            await_task(name, tasks[name])
            replay = tool(name, 'task_start', arguments)
            check(not replay['created_new'] and replay['task_id'] == tasks[name]['task_id'] and
                  (folder / ('workspace-' + name) / 'once.txt').read_bytes() == b'ONE', 'repeated requests return project-local receipt without duplicate side effects')
        for operation in ('task_status', 'task_output', 'task_cancel'):
            tool('b', operation, {'task_id': tasks['a']['task_id'], 'host_instance_id': instances['b']}, failed=True)
        owner_result = control('--project-request', 'a', modern('tools/call', 'task_status', {'task_id': tasks['a']['task_id'], 'host_instance_id': instances['a']}))['response']
        check(owner_result['result']['isError'], 'local owner MCP actor cannot claim project bearer task ownership')
        owner_arguments = {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['append'], 'request_key': 'shared-key', 'host_instance_id': instances['a']}
        owner_task = control('--project-request', 'a', modern('tools/call', 'task_start', owner_arguments))['response']['result']['structuredContent']
        check(owner_task['created_new'] and owner_task['task_id'] != tasks['a']['task_id'], 'owner and bearer idempotency keys have separate actor bindings')
        tool('a', 'task_status', {'task_id': owner_task['task_id'], 'host_instance_id': instances['a']}, failed=True)
        identity = tool('a', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['identity'], 'request_key': 'identity', 'host_instance_id': instances['a']})
        await_task('a', identity)
        output = tool('a', 'task_output', {'task_id': identity['task_id'], 'host_instance_id': instances['a']})
        actual = json.loads(base64.b64decode(output['data_base64']))
        check(not actual['elevated'] and not actual['project_tokens_inherited'] and not actual['canary_inherited'], 'real HTTP task runs unelevated without manager credentials')
        stdin_task = tool('a', 'task_start', {'workspace_id': 'same-workspace', 'executable': str(fixture), 'argv': ['stdin'], 'request_key': 'input', 'host_instance_id': instances['a']})
        input_args = {'task_id': stdin_task['task_id'], 'host_instance_id': instances['a'], 'input_key': 'input-once', 'text': 'gateway-input', 'eof': True}
        tool('b', 'task_input', {**input_args, 'host_instance_id': instances['b']}, failed=True)
        tool('a', 'task_input', input_args)
        tool('a', 'task_input', input_args)
        tool('a', 'task_input', {**input_args, 'text': 'altered'}, failed=True)
        await_task('a', stdin_task)
        stdin_output = tool('a', 'task_output', {'task_id': stdin_task['task_id'], 'host_instance_id': instances['a']})
        check(base64.b64decode(stdin_output['data_base64']) == b'gateway-input', 'HTTP input retries write exact bytes once and preserve EOF')
        pending = tool('legacy', 'fs_write', {'workspace_id': 'same-workspace', 'path': 'approved.txt', 'text': 'legacy-gateway-body'}, failed=True)
        check(pending['status'] == 'pending' and not (folder / 'workspace-legacy' / 'approved.txt').exists(), 'legacy HTTP write waits for local owner approval')
        http('/projects/legacy/mcp', modern('approve'), tokens['legacy'], expected=404)
        control('--project-owner', 'legacy', {'command': 'approve', 'id': pending['approval_id']})
        owner_write = control('--project-request', 'legacy', modern('tools/call', 'fs_write', {'workspace_id': 'same-workspace', 'path': 'approved.txt', 'text': 'legacy-gateway-body', 'approval_id': pending['approval_id']}))['response']
        check(owner_write['result']['isError'] and not (folder / 'workspace-legacy' / 'approved.txt').exists(), 'owner bridge cannot consume a bearer-bound approval as another actor')
        written = tool('legacy', 'fs_write', {'workspace_id': 'same-workspace', 'path': 'approved.txt', 'text': 'legacy-gateway-body', 'approval_id': pending['approval_id']})
        check(written['status'] == 'completed', 'local owner approval remains bound to originating project bearer actor')
        replay_write = tool('legacy', 'fs_write', {'workspace_id': 'same-workspace', 'path': 'approved.txt', 'text': 'legacy-gateway-body', 'approval_id': pending['approval_id']})
        check(replay_write['status'] == 'completed' and (folder / 'workspace-legacy' / 'approved.txt').read_bytes() == b'legacy-gateway-body', 'approved HTTP write retry returns the original completion')
        # A stuck worker has one deadline; B stays available, and no request is replayed.
        suspend_owned(workers['a'])
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            started = time.monotonic()
            failed = pool.submit(http, expected=503)
            wait_busy()
            http('/projects/b/mcp', token=tokens['b'])
            failure = failed.result(timeout=12)
            check(time.monotonic() - started < 11 and 'No automatic replay' in failure['error'], 'stuck worker deadline returns explicit uncertain-result error')
        check(not alive(workers['a']) and alive(workers['b']), 'deadline kills only manager-owned project A worker')
        http(expected=503)
        restarted = control('--project-start', 'a')['project']
        check(restarted['host_instance_id'] != instances['a'], 'only explicit owner restart creates a new project instance')
        tool('a', 'task_status', {'task_id': tasks['a']['task_id'], 'host_instance_id': instances['a']}, failed=True)
        check(registry.read_bytes() == original, 'gateway never changes source registry')
        control('--manager-stop')
        m.wait(timeout=15)
        check(m.returncode == 0 and not alive(workers['b']) and not alive(restarted['worker_pid']), 'normal gateway shutdown cleans its owned workers')
        # Default route is owner configuration, frozen until an explicit restart.
        default_value = copy.deepcopy(value)
        default_value['gateway']['default_project_id'] = 'a'
        default_value['gateway']['public_url'] = ''
        default_registry = save('registry-default.json', default_value)
        default_data = folder / 'default-manager-data'
        dm = launch(default_registry, default_data)
        control('--project-start', 'a', manager_data=default_data)
        http('/mcp')
        http('/mcp', token=tokens['b'], expected=401)
        http('/healthz', verb='GET')
        default_links = control('--project-links', 'a', manager_data=default_data)['links']
        check(default_links['default_route'] and 'public_mcp_url' not in default_links, 'default compatibility path and links need no public origin')
        http('/mcp', changes={'Host': 'existing-proxy.invalid', 'Origin': 'https://existing-proxy.invalid'})
        changed = copy.deepcopy(default_value)
        changed['gateway']['default_project_id'] = 'b'
        try:
            default_registry.write_text(json.dumps(changed), encoding='utf-8')
            blocked=False
        except PermissionError:blocked=True
        check(blocked,'active gateway registry refuses changes to the default route')
        http('/mcp')
        http('/mcp', token=tokens['b'], expected=401)
        control('--manager-stop', manager_data=default_data)
        dm.wait(timeout=15)
        # Only non-secret registry and audit artifacts are preserved.
        for path in folder.rglob('*'):
            if path.is_file():
                content = path.read_bytes()
                check(not any(token.encode() in content for token in tokens.values()), 'artifact excludes bearer values: ' + path.name)
        report['passed'] = True
    except Exception as exc:
        report['failure'] = str(exc)
        raise
    finally:
        if slow:
            slow.close()
        for process, manager_data in managers:
            if process.poll() is None:
                try:
                    control('--manager-stop', manager_data=manager_data)
                    process.wait(timeout=20)
                except Exception:
                    process.kill()
                    process.wait(timeout=8)
            errors = '' if process.stderr.closed else process.stderr.read().decode('utf-8', errors='replace')
            if any(token in errors for token in tokens.values()):
                report['passed'] = False
                report['failure'] = 'manager diagnostics exposed a test credential'
        report['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
        report['checks_passed'] = sum(c['passed'] for c in report['checks'])
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'stage': 'M3', 'passed': report['passed'], 'checks': report['checks_passed'], 'report': str(args.report)}))
    if not report['passed']:
        raise AssertionError(report.get('failure', 'gateway validation failed'))


if __name__ == '__main__':
    main()
