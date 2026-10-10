"""M5 uses isolated registries and the native fixture; elevation is opt-in."""
import argparse, base64, copy, ctypes, datetime, hashlib, http.client, json, os, shutil, socket, subprocess, time, uuid
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--administrator', action='store_true')
    parser.add_argument('--cancel-start', action='store_true')
    parser.add_argument('--administrator-shutdown', action='store_true')
    args = parser.parse_args()
    if sum([args.administrator, args.cancel_start, args.administrator_shutdown]) > 1:
        parser.error('choose one elevation scenario')
    exe = Path(args.exe).resolve()
    repo = Path(__file__).resolve().parents[1]
    folder = repo / 'build' / 'm5-tests' / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    report = {'stage': 'M5', 'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'artifacts': str(folder), 'checks': [], 'passed': False, 'uac_attempted': False,
              'uac_dialog_count': 'not_observed_automatically', 'mode': 'administrator' if args.administrator else 'cancel-start' if args.cancel_start else 'administrator-shutdown' if args.administrator_shutdown else 'ordinary'}
    flags = subprocess.CREATE_NO_WINDOW
    data = folder / 'manager'
    manager = None
    agent_pid = None
    tokens = {p: uuid.uuid4().hex + uuid.uuid4().hex for p in 'ABCDE'}

    def check(ok, name):
        report['checks'].append({'name': name, 'passed': bool(ok)})
        if not ok:
            raise AssertionError(name)

    def save(value):
        path = folder / (uuid.uuid4().hex + '.json')
        path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
        return path

    def control(command, project=None, request=None, failed=False):
        command_args = [str(exe), command] + ([project] if project else [])
        if request is not None:
            command_args += ['--request', str(save(request))]
        command_args += ['--manager-data-dir', str(data)]
        result = subprocess.run(command_args, capture_output=True, text=True, encoding='utf-8', timeout=40, creationflags=flags)
        check(result.returncode in [0, 2] if failed is None else (result.returncode != 0) == failed, command + ' expected exit: ' + result.stderr[:120])
        return json.loads(result.stdout)

    def rpc(project, method, name=None, arguments=None, allow_disconnect=False):
        params = {'_meta': {'io.modelcontextprotocol/protocolVersion': '2026-07-28', 'io.modelcontextprotocol/clientCapabilities': {}}}
        if name:
            params.update(name=name, arguments=arguments or {})
        response = control('--project-request', project, {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}, failed=None if allow_disconnect else False)
        return response.get('response', response)

    def tool(project, name, arguments=None, failed=False):
        response = rpc(project, 'tools/call', name, arguments)
        check('result' in response and response['result']['isError'] == failed, project + ' ' + name + ' expected tool result: ' + str(response)[:180])
        return response['result']['structuredContent']

    def http_tool(project, name, arguments, failed=False):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=15)
        try:
            connection.request('POST', '/projects/' + project + '/mcp', json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                               'params': {'name': name, 'arguments': arguments}}), {'Authorization': 'Bearer ' + tokens[project],
                               'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream',
                               'MCP-Protocol-Version': '2025-11-25', 'MCP-Method': 'tools/call', 'MCP-Name': name})
            response = connection.getresponse()
            value = json.loads(response.read())
            check(response.status == 200 and value['result']['isError'] == failed, 'HTTP ' + project + ' ' + name)
            return value['result']['structuredContent']
        finally:
            connection.close()

    def owner(project, command, approval_id=None, administrator=True):
        return control('--project-administrator-owner' if administrator else '--project-owner', project,
                       {'command': command, **({'id': approval_id} if approval_id else {})})['response']

    def task_start(project, mode, key, identity=None, pending=False, timeout=5000):
        value = {'workspace_id': 'work', 'executable': str(fixture), 'argv': [mode], 'request_key': key,
                 'host_instance_id': instances[project], 'timeout_ms': timeout}
        if identity:
            value['identity'] = identity
        task = tool(project, 'task_start', value, failed=pending)
        if pending:
            check(task['status'] == 'pending' and 'task_id' not in task, 'unapproved native task did not start')
            owner(project, 'approve', task['approval_id'], administrator=identity == 'administrator')
            value['approval_id'] = task['approval_id']
            task = tool(project, 'task_start', value)
        return task, value

    def poll(project, task, administrator=True):
        value = {'task_id': task['task_id'], 'host_instance_id': task['host_instance_id']}
        if administrator:
            value['identity'] = 'administrator'
        for _ in range(150):
            status = tool(project, 'task_status', value)
            if status['status'] not in ('running', 'starting'):
                return status
            time.sleep(.04)
        raise AssertionError('native task completion deadline')

    def output(project, task, administrator=True):
        value = {'task_id': task['task_id'], 'host_instance_id': task['host_instance_id']}
        if administrator:
            value['identity'] = 'administrator'
        result = tool(project, 'task_output', value)
        return base64.b64decode(result['data_base64'])

    def alive(pid):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == 258
        finally:
            kernel.CloseHandle(handle)

    def start_and_wait(cancelled=False):
        started = control('--administrator-start')
        check(started['administrator_launch_attempts'] == 1, 'owner submits one asynchronous startup attempt')
        report['startup_pending_observed'] = started['administrator_starting']
        # These requests must return even while ShellExecute waits on UAC.
        began = time.monotonic()
        tool('A', 'host_capabilities')
        check(time.monotonic() - began < 5, 'ordinary capabilities remain responsive during administrator startup')
        normal, _ = task_start('A', 'identity', 'normal-during-startup')
        check(poll('A', normal, False)['exit_code'] == 0 and not json.loads(output('A', normal, False))['elevated'], 'ordinary native tasks remain unelevated during startup')
        repeated = control('--administrator-start')
        check(repeated['administrator_launch_attempts'] == 1, 'repeated start while pending does not request another UAC')
        print(json.dumps({'waiting_for_uac': True, 'scenario': 'cancel' if cancelled else 'allow', 'artifacts': str(folder)}), flush=True)
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            status = control('--administrator-status')
            if not status['administrator_starting']:
                report['startup_completion'] = status
                check(status['administrator_launch_attempts'] == 1, 'status polling never retries startup')
                if cancelled:
                    check(not status['administrator_alive'] and 'cancelled' in status.get('startup_error', '').lower(), 'cancelled UAC leaves administrator backend unavailable')
                else:
                    check(status['administrator_alive'], 'owner starts one shared agent: ' + status.get('startup_error', ''))
                return status
            time.sleep(.3)
        raise AssertionError('UAC scenario did not complete within five minutes')

    try:
        check(not ctypes.windll.shell32.IsUserAnAdmin(), 'test driver has an ordinary token')
        runtime = folder / 'runtime'
        runtime.mkdir()
        fixture = runtime / 'ShellBot-TestFixture.exe'
        shutil.copy2(exe.with_name('ShellBot-TestFixture.exe'), fixture)
        report['fixture_sha256'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
        tools = ['host_info', 'host_capabilities', 'fs_list', 'fs_read', 'fs_search', 'image_view', 'fs_write', 'fs_patch',
                 'task_start', 'task_status', 'task_output', 'task_input', 'task_cancel']
        projects = []
        roots = {}
        for p in 'ABCDE':
            root = folder / ('work-' + p)
            root.mkdir()
            roots[p] = root
            (root / 'sample.txt').write_text('original ' + p, encoding='utf-8')
            projects.append({'id': p, 'token_env': 'SHELLBOT_' + p + '_TOKEN', 'data_dir': str(folder / ('data-' + p)),
                             'approval_ttl_seconds': 4, 'workspaces': [{'id': 'work', 'path': str(root), 'read_only': p == 'E'}],
                             'policy': {'approval_mode': 'always' if p == 'B' else 'never',
                                        'files': {'scope': 'configured'} if p == 'D' else {'scope': 'current_user', 'current_user_read_only': False},
                                        'execution': {'enabled': p != 'E', 'default_identity': 'restricted' if p == 'D' else 'current_user',
                                                      'administrator_allowed': p in 'AB'}, 'tools': tools}})
        projects[3]['policy']['execution']['runtime_files'] = [str(fixture)]
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        registry = save({'format_version': 1, 'gateway': {'port': port}, 'projects': projects})
        before = registry.read_bytes()
        environment = os.environ.copy()
        environment.update({p['token_env']: tokens[p['id']] for p in projects})
        environment['SHELLBOT_TOKEN'] = uuid.uuid4().hex + uuid.uuid4().hex
        environment['SHELLBOT_M2_CANARY'] = 'must-not-enter-a-native-task'
        manager = subprocess.Popen([str(exe), '--manager', '--http', '--projects', str(registry), '--manager-data-dir', str(data)],
                                   env=environment, stdout=(folder / 'manager.stdout').open('w'), stderr=(folder / 'manager.stderr').open('w'), creationflags=flags)
        for _ in range(160):
            if (folder / 'manager.stdout').exists() and 'manager_ready' in (folder / 'manager.stdout').read_text():
                break
            if manager.poll() is not None:
                raise AssertionError('manager startup: ' + (folder / 'manager.stderr').read_text())
            time.sleep(.05)
        else:
            raise AssertionError('manager readiness deadline')
        check(not control('--administrator-status')['administrator_alive'], 'manager startup does not launch an agent')
        instances = {}
        for p in 'ABCDE':
            status = control('--project-start', p)['project']
            instances[p] = status['host_instance_id']
            capabilities = tool(p, 'host_capabilities')
            check(not capabilities['execution_status']['host_identity']['elevated'] and not capabilities['administrator_backend'], p + ' worker stays ordinary before agent')
        schema = rpc('A', 'tools/list')['result']['tools']
        check(all('identity' in t['inputSchema']['properties'] for t in schema if t['name'] not in ['host_info', 'host_capabilities']), 'explicit identity is published for file and task operations')
        normal, _ = task_start('A', 'identity', 'normal-before')
        check(poll('A', normal, False)['exit_code'] == 0 and not json.loads(output('A', normal, False))['elevated'], 'default task is unelevated')
        basic = {'workspace_id': 'work', 'path': 'sample.txt', 'identity': 'administrator'}
        check('unavailable' in tool('A', 'fs_read', basic, True)['error'], 'permitted project needs an available agent')
        check(tool('C', 'fs_read', basic, True)['error'] == 'administrator_not_allowed', 'C cannot use administrator identity')
        tool('D', 'fs_read', basic, True)
        control('--project-administrator-allow', 'D', failed=True)
        for extra in [{'project_id': 'A'}, {'approved': True}, {'administrator_backend_verified': True}]:
            tool('C', 'fs_read', {**basic, **extra}, True)
        tool('A', 'fs_read', {**basic, 'identity': 'root'}, True)
        tool('A', 'fs_read', {**basic, 'identity': 'restricted'}, True)
        check('error' in rpc('C', 'administrator-start'), 'MCP cannot start a UAC agent')
        control('--project-administrator-allow', 'E')
        check(control('--project-status', 'E')['project']['administrator_allowed'], 'owner can explicitly grant a new project')
        control('--project-administrator-revoke', 'E')
        check(not control('--project-status', 'E')['project']['administrator_allowed'], 'owner can revoke before startup')
        check(registry.read_bytes() == before, 'runtime grants preserve the pinned registry')
        bogus = subprocess.run([str(exe), '--shared-administrator-agent', str(manager.pid), '0' * 32], capture_output=True, creationflags=flags)
        check(bogus.returncode != 0, 'ordinary process cannot pretend to be the elevated internal agent')
        if args.cancel_start:
            report['uac_attempted'] = True
            start_and_wait(cancelled=True)
            tool('A', 'fs_read', basic, True)
            check(not control('--administrator-status')['administrator_alive'], 'no implicit retry after cancelled startup')
        elif args.administrator_shutdown:
            report['uac_attempted'] = True
            started = start_and_wait()
            agent = started['execution_status']['administrator_identity']
            agent_pid = agent['process_id']
            report['agent_identity'] = agent
            check(agent['elevated'] and not agent['restricted'] and not agent['appcontainer'], 'shutdown scenario agent has an actual elevated token')
            active_a, _ = task_start('A', 'wait', 'manager-exit-A', 'administrator', timeout=60000)
            active_b, _ = task_start('B', 'wait', 'manager-exit-B', 'administrator', pending=True, timeout=60000)
            worker_pids = [control('--project-status', p)['project']['worker_pid'] for p in 'ABCDE']
            check(alive(agent_pid) and alive(active_a['process_id']) and alive(active_b['process_id']), 'two elevated jobs are active before manager exit')
            control('--manager-stop')
            manager.wait(timeout=12)
            check(all(not alive(pid) for pid in worker_pids + [agent_pid, active_a['process_id'], active_b['process_id']]), 'manager exit reclaims all ordinary workers, shared agent and active elevated jobs')
        elif args.administrator:
            report['uac_attempted'] = True
            started = start_and_wait()
            agent = started['execution_status']['administrator_identity']
            agent_pid = agent['process_id']
            report['agent_identity'] = agent
            report['manager_pid'] = manager.pid
            check(agent['elevated'] and agent['administrator_group_enabled'] and not agent['restricted'] and not agent['appcontainer'], 'agent token is actually elevated')
            again = control('--administrator-start')
            check(again['administrator_launch_attempts'] == 1 and again['administrator_generation'] == started['administrator_generation'] and again['execution_status']['administrator_identity']['process_id'] == agent_pid, 'explicit repeated start reuses the same agent')
            check(control('--project-administrator-boundary-probe', 'A')['bindings_rejected'] == 9, 'agent itself rejects forged project/version/instance/digest/expiry/sequence/actor and approved flag')
            for p in 'AB':
                task, task_args = task_start(p, 'identity', 'shared-' + p, 'administrator', pending=p == 'B')
                check(poll(p, task)['exit_code'] == 0, p + ' administrator native task exits')
                identity = json.loads(output(p, task))
                check(identity['elevated'] and not identity['appcontainer'] and not identity['token_inherited'] and not identity['project_tokens_inherited'] and not identity['canary_inherited'], p + ' task token is elevated without credentials')
                replay = tool(p, 'task_start', task_args)
                check(replay['task_id'] == task['task_id'] and not replay['created_new'], p + ' retry does not duplicate an elevated task')
                tool(p, 'task_status', {'task_id': task['task_id'], 'host_instance_id': instances[p]}, True)
                report[p + '_administrator_task'] = task['task_id']
            check(tool('A', 'host_capabilities')['administrator_backend'] and not tool('A', 'host_capabilities')['execution_status']['host_identity']['elevated'], 'capabilities distinguish shared agent from ordinary worker')
            tool('C', 'fs_read', basic, True)
            tool('B', 'task_status', {'identity': 'administrator', 'host_instance_id': instances['B'], 'task_id': report['A_administrator_task']}, True)
            # A native elevated task protects a fixture file, rather than changing an OS file.
            protected, _ = task_start('A', 'protect', 'protect-test-file', 'administrator')
            check(poll('A', protected)['exit_code'] == 0, 'native fixture creates a test-only administrator-protected file')
            protected_args = {'workspace_id': 'work', 'path': 'admin-only.txt'}
            tool('A', 'fs_read', protected_args, True)
            listed = tool('A', 'fs_list', {'workspace_id': 'work', 'identity': 'administrator'})
            check(any(item['name'] == 'admin-only.txt' for item in listed['entries']), 'administrator directory listing uses the file backend')
            search = tool('A', 'fs_search', {'workspace_id': 'work', 'identity': 'administrator', 'query': 'administrator fixture', 'mode': 'content'})
            check(search['matches'], 'administrator content search reads protected fixture')
            written = {'workspace_id': 'work', 'identity': 'administrator', 'path': 'created.txt', 'text': 'one write'}
            first = tool('A', 'fs_write', written)
            check(first['sha256'] == tool('A', 'fs_write', written)['sha256'] and (roots['A'] / 'created.txt').read_text() == 'one write', 'administrator write retries retain a single receipt')
            image_path = roots['A'] / 'small.png'
            import zlib, struct
            def chunk(name, body):
                return struct.pack('>I', len(body)) + name + body + struct.pack('>I', zlib.crc32(name + body) & 0xffffffff)
            image_path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b'\0\xff\0\0')) + chunk(b'IEND', b''))
            image = tool('A', 'image_view', {'workspace_id': 'work', 'identity': 'administrator', 'path': 'small.png'})
            check(image['width'] == 1 and image['height'] == 1, 'administrator image backend returns native content')
            stdin, _ = task_start('A', 'stdin', 'admin-stdin', 'administrator')
            value = {'identity': 'administrator', 'host_instance_id': instances['A'], 'task_id': stdin['task_id'], 'input_key': 'once', 'text': 'administrator stdin', 'eof': True}
            tool('A', 'task_input', value)
            tool('A', 'task_input', value)
            check(poll('A', stdin)['exit_code'] == 0 and output('A', stdin) == b'administrator stdin', 'administrator input/EOF retry is delivered only once')
            timed, _ = task_start('A', 'wait', 'admin-timeout', 'administrator', timeout=150)
            check(poll('A', timed)['status'] == 'timed_out', 'administrator task timeout terminates its Job')
            check(tool('A', 'fs_read', {**protected_args, 'identity': 'administrator'})['text'] == 'administrator fixture', 'administrator file backend reads a file denied to the ordinary worker')
            changed = tool('A', 'fs_patch', {**protected_args, 'identity': 'administrator', 'search': 'fixture', 'replacement': 'edited', 'expected_sha256': hashlib.sha256(b'administrator fixture').hexdigest()})
            check('backup_id' in changed, 'administrator edits retain a private backup')
            check(tool('A', 'fs_read', {**protected_args, 'identity': 'administrator'})['text'] == 'administrator edited', 'administrator patch writes protected test data')
            tool('A', 'fs_read', protected_args, True)
            # An administrator file request still needs agent-owned approval in always.
            pending = tool('B', 'fs_read', basic, True)
            check(pending['status'] == 'pending', 'administrator file read is pending in always')
            owner('B', 'approve', pending['approval_id'], administrator=False)
            tool('B', 'fs_read', {**basic, 'approval_id': pending['approval_id']}, True)
            inspection = owner('B', 'inspect', pending['approval_id'])
            check(inspection['status'] == 'pending', 'ordinary approval channel cannot approve administrator record')
            owner('B', 'approve', pending['approval_id'])
            read = tool('B', 'fs_read', {**basic, 'approval_id': pending['approval_id']})
            check(read['text'] == 'original B', 'agent independently consumes a real owner approval')
            http_pending = http_tool('B', 'fs_read', basic, True)
            owner('B', 'approve', http_pending['approval_id'])
            tool('B', 'fs_read', {**basic, 'approval_id': http_pending['approval_id']}, True)
            check(http_tool('B', 'fs_read', {**basic, 'approval_id': http_pending['approval_id']})['text'] == 'original B', 'HTTP administrator approval is bound to the project MCP actor')
            expired = tool('B', 'fs_read', {**basic, 'max_bytes': 8}, True)
            owner('B', 'approve', expired['approval_id'])
            time.sleep(4.2)
            tool('B', 'fs_read', {**basic, 'max_bytes': 8, 'approval_id': expired['approval_id']}, True)
            revoked = tool('B', 'fs_write', {'workspace_id': 'work', 'path': 'must-not-exist.txt', 'text': 'no', 'identity': 'administrator'}, True)
            owner('B', 'approve', revoked['approval_id'])
            waiting, _ = task_start('B', 'wait', 'cancel-on-revoke', 'administrator', pending=True, timeout=60000)
            control('--project-administrator-revoke', 'B')
            check(poll('B', waiting)['status'] == 'cancelled', 'revocation cancels only the project administrator tasks')
            check(not (roots['B'] / 'must-not-exist.txt').exists(), 'revoked queued modification did not execute')
            tool('B', 'fs_write', {'workspace_id': 'work', 'path': 'must-not-exist.txt', 'text': 'no', 'identity': 'administrator', 'approval_id': revoked['approval_id']}, True)
            control('--project-administrator-allow', 'B')
            tool('B', 'fs_write', {'workspace_id': 'work', 'path': 'must-not-exist.txt', 'text': 'no', 'identity': 'administrator', 'approval_id': revoked['approval_id']}, True)
            check(not (roots['B'] / 'must-not-exist.txt').exists(), 'regrant does not resurrect old approvals')
            restricted, _ = task_start('D', 'identity', 'restricted-with-agent')
            check(poll('D', restricted, False)['exit_code'] == 0, 'LPAC task runs with an elevated agent present')
            restricted_identity = json.loads(output('D', restricted, False))
            check(restricted_identity['appcontainer'] and not restricted_identity['elevated'] and not restricted_identity['project_tokens_inherited'], 'LPAC worker does not inherit elevation or project credentials')
            private = next((folder / 'data-A').glob('administrator-*/audit-*.jsonl'))
            owner_pipe = '\\\\.\\pipe\\ShellBot-owner-' + hashlib.sha256(('\\\\?\\' + str(data)).encode('utf-16le')).hexdigest()
            probe_args = {'workspace_id': 'work', 'executable': str(fixture), 'argv': ['sandbox', str(roots['C'] / 'sample.txt'), str(roots['E'] / 'sample.txt'), str(private), owner_pipe, str(agent_pid), str(port)], 'request_key': 'agent-lpac-boundaries', 'host_instance_id': instances['D'], 'timeout_ms': 5000}
            restricted_probe = tool('D', 'task_start', probe_args)
            check(poll('D', restricted_probe, False)['exit_code'] == 0, 'native LPAC management boundary probe completes')
            boundaries = json.loads(output('D', restricted_probe, False))
            report['restricted_agent_boundaries'] = boundaries
            check(all(not boundaries[key] for key in ['outside_read', 'readonly_write', 'private_read', 'change_acl', 'owner_pipe', 'process_injection', 'network', 'registry_read', 'children']) and boundaries['win32k_disabled'], 'LPAC cannot access administrator approvals, owner registration, agent process or outside resources')
            control('--project-administrator-allow', 'E')
            check(tool('E', 'fs_read', basic)['text'] == 'original E', 'new owner grant reuses the running agent')
            tool('E', 'fs_write', {**basic, 'text': 'forbidden'}, True)
            check((roots['E'] / 'sample.txt').read_text() == 'original E', 'administrator respects a readonly workspace')
            # Stop ownership: A must not cancel B or the shared agent.
            active_a, _ = task_start('A', 'wait', 'stop-project-A', 'administrator', timeout=60000)
            active_b, _ = task_start('B', 'wait', 'keep-project-B', 'administrator', pending=True, timeout=60000)
            control('--project-stop', 'A')
            check(not alive(active_a['process_id']) and alive(active_b['process_id']) and alive(agent_pid), 'stopping A kills A tasks while B and agent survive')
            tool('B', 'task_cancel', {'identity': 'administrator', 'host_instance_id': instances['B'], 'task_id': active_b['task_id']})
            old_instance = instances['A']
            instances['A'] = control('--project-start', 'A')['project']['host_instance_id']
            check(instances['A'] != old_instance, 'restart creates a new worker binding')
            tool('A', 'task_status', {'identity': 'administrator', 'host_instance_id': old_instance, 'task_id': active_a['task_id']}, True)
            ordinary_after, _ = task_start('A', 'identity', 'normal-after')
            check(not json.loads(output('A', ordinary_after, False) if poll('A', ordinary_after, False)['exit_code'] == 0 else '{}')['elevated'], 'ordinary default remains unelevated with a running agent')
            check(control('--administrator-status')['execution_status']['administrator_identity']['process_id'] == agent_pid, 'all A/B operations and grants share one agent PID')
            active, _ = task_start('A', 'wait', 'worker-crash-task', 'administrator', timeout=60000)
            worker_pid = control('--project-status', 'A')['project']['worker_pid']
            rpc('A', 'tools/call', 'task_start', {'workspace_id': 'work', 'executable': str(fixture), 'argv': ['exit-parent'], 'request_key': 'crash-own-worker', 'host_instance_id': instances['A'], 'timeout_ms': 5000}, allow_disconnect=True)
            for _ in range(100):
                if not alive(worker_pid) and not alive(active['process_id']):
                    break
                time.sleep(.05)
            check(not alive(worker_pid) and not alive(active['process_id']) and alive(agent_pid), 'worker crash cancels its elevated jobs without killing shared agent')
            instances['A'] = control('--project-start', 'A')['project']['host_instance_id']
            active, _ = task_start('A', 'wait', 'agent-crash-task', 'administrator', timeout=60000)
            rpc('A', 'tools/call', 'task_start', {'workspace_id': 'work', 'executable': str(fixture), 'argv': ['exit-parent'], 'identity': 'administrator', 'request_key': 'crash-own-agent', 'host_instance_id': instances['A'], 'timeout_ms': 5000}, allow_disconnect=True)
            for _ in range(100):
                if not alive(agent_pid) and not alive(active['process_id']):
                    break
                time.sleep(.05)
            check(not alive(agent_pid) and not alive(active['process_id']), 'abrupt agent death closes its task jobs')
            tool('A', 'fs_read', basic, True)
            check(not control('--administrator-status')['administrator_alive'], 'agent disappearance does not automatically request UAC or fallback')
            normal_after_death, _ = task_start('A', 'identity', 'normal-after-agent-death')
            check(poll('A', normal_after_death, False)['exit_code'] == 0 and not json.loads(output('A', normal_after_death, False))['elevated'], 'agent death leaves the ordinary backend available')
            control('--administrator-stop')
        if manager.poll() is None:
            control('--manager-stop')
            manager.wait(timeout=12)
        check(registry.read_bytes() == before, 'all administrator management preserved the original registry')
        if agent_pid:
            check(not alive(agent_pid), 'manager exit leaves no shared agent')
        # Only text receipts are scanned; binary fixtures contain no runtime bearer values.
        for path in folder.rglob('*'):
            if path.is_file() and path.suffix not in ['.exe', '.lock']:
                try:
                    body = path.read_bytes()
                except PermissionError:
                    continue  # administrator-only test data remains intentionally protected
                check(all(token.encode() not in body for token in tokens.values()), 'retained receipt excludes bearer values: ' + path.name)
        report['passed'] = True
    except Exception as exc:
        report['failure'] = repr(exc)
        raise
    finally:
        if manager and manager.poll() is None:
            try:
                control('--manager-stop')
                manager.wait(timeout=12)
            except Exception:
                report['cleanup_forced_manager_exit'] = True
                manager.terminate()
                try:
                    manager.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    report['cleanup_failure'] = 'owned test manager did not exit'
        report['checks_count'] = len(report['checks'])
        output_path = Path(args.report)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        shutil.copy2(output_path, folder / 'report.json')
        print(json.dumps({k: report[k] for k in ['mode', 'passed', 'checks_count', 'artifacts']}))


if __name__ == '__main__':
    main()
