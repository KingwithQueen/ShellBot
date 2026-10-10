"""Real process and job integration tests, not command-parser unit replicas."""
import base64
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
kernel.OpenProcess.restype = ctypes.c_void_p
kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]

def task_checks(executable, folder, Stdio, initialize, check):
    fixture = Path(__file__).with_name('task_fixture.py').resolve()
    root = folder / 'task cwd 中文 & !'
    root.mkdir()
    outside = folder / 'task outside'
    outside.mkdir()
    subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(root/'escape'), str(outside)], capture_output=True, check=True)
    configuration = folder / 'tasks.json'
    state = folder / 'tasks-state'
    configuration.write_text(json.dumps({'roots':[{'id':'tasks','path':str(root),'read_only':True}], 'execution':{'enabled':True,'profile':'user'}}),encoding='utf-8')
    baseline = subprocess.run([str(executable),'--execution-status'],capture_output=True,check=True)
    if json.loads(baseline.stdout)['host_identity']['elevated']:
        raise AssertionError('Task acceptance requires an ordinary unelevated test process.')
    old_secret = os.environ.get('SHELLBOT_TEST_SECRET')
    os.environ['SHELLBOT_TEST_SECRET'] = 'fixture-credential-not-for-child'
    bot = Stdio(executable,['--config',str(configuration),'--data-dir',str(state)])
    if old_secret is None:
        os.environ.pop('SHELLBOT_TEST_SECRET',None)
    else:
        os.environ['SHELLBOT_TEST_SECRET'] = old_secret
    unrelated = None
    def bind(current):
        instance=current.tool('host_capabilities')['structuredContent']['execution_status']['host_instance_id']
        original=current.tool
        def scoped(name,**args):
            if name.startswith('task_'): args.setdefault('host_instance_id',instance)
            return original(name,**args)
        current.tool=scoped
        return instance
    def call(name, **args):
        result = bot.tool(name,**args)
        check(not result['isError'], name + ': ' + json.dumps(result.get('structuredContent')))
        return result['structuredContent']
    def start(key, mode, *args, **extra):
        return call('task_start',workspace_id='tasks',executable=sys.executable,argv=[str(fixture),mode,*args],request_key=key,**extra)
    def wait(task_id, timeout=12):
        end = time.monotonic()+timeout
        while time.monotonic()<end:
            value = call('task_status',task_id=task_id)
            if value['status']!='running': return value
            time.sleep(.025)
        raise AssertionError('Task did not reach a terminal state.')
    def output(task_id, stream='stdout', max_bytes=16384):
        data, offset = b'', 0
        while True:
            page = call('task_output',task_id=task_id,stream=stream,offset=offset,max_bytes=max_bytes)
            chunk = base64.b64decode(page['data_base64'])
            check(page['next_offset']==offset+len(chunk), 'raw byte pagination advances exactly')
            data += chunk; offset=page['next_offset']
            if page['eof']: return data
            check(bool(chunk), 'completed task output must make progress')
    try:
        bot.call(initialize());bot.send({'jsonrpc':'2.0','method':'notifications/initialized'})
        original_instance=bind(bot)
        capabilities = call('host_capabilities')
        check(capabilities['execution'] and capabilities['execution_status']['readiness']=='ready', 'explicit ordinary user task readiness')
        check(not capabilities['execution_status']['host_identity']['elevated'], 'host is actually unelevated')
        strange=['','中文 😀','quote"inside','trailing'+chr(92),'&!^%$(x)']
        echoed = start('argv','echo',*strange)
        check(echoed['created_new'] and not echoed['elevated'] and echoed['profile']=='user', 'new task reports ordinary identity')
        check(wait(echoed['task_id'])['status']=='completed','real process completes')
        values=json.loads(output(echoed['task_id']))
        check(values['argv']==strange,'native Unicode empty quote backslash and metacharacter argv roundtrip')
        check(os.path.samefile(values['cwd'],root),'actual configured working directory')
        check(not values['elevated'] and not values['bearer_inherited'],'child actual token is unelevated and secret environment is not inherited')
        raw=start('raw','raw'); result=wait(raw['task_id'])
        check(result['exit_code']==7,'nonzero exit code is preserved')
        check(output(raw['task_id'],max_bytes=3)==b'\x00\xff'+'中文 😀'.encode(),'binary stdout and Unicode pages are lossless')
        check(output(raw['task_id'],'stderr')==b'error\x00\xfe','stderr is independent and lossless')
        escaped=start('escaped','escaped');wait(escaped['task_id'])
        page=call('task_output',task_id=escaped['task_id'])
        check(base64.b64decode(page['data_base64'])==b'\x01'*16384 and page['text_utf8']=='\x01'*16384,'max page survives sixfold JSON escaping without losing raw bytes')
        written=start('append','append','once.txt','ONE')
        wait(written['task_id'])
        retried=start('append','append','once.txt','ONE')
        check(retried['task_id']==written['task_id'] and not retried['created_new'] and (root/'once.txt').read_text()=='ONE','retry never appends twice')
        check(bot.tool('task_start',workspace_id='tasks',executable=sys.executable,argv=[str(fixture),'append','once.txt','TWO'],request_key='append')['isError'],'request key rejects changed argv')
        for invalid in [{'path':'../task outside'},{'path':'escape'},{'path':str(outside)},{'profile':'administrator'},{'approved':True},{'actor':'owner'},{'timeout_ms':True}]:
            check(bot.tool('task_start',workspace_id='tasks',executable=sys.executable,argv=[],request_key='invalid',**invalid)['isError'],'task cannot change profile or bypass cwd validation')
        for program in ['python.exe',str(fixture),'C:\\bad.exe:stream']:
            check(bot.tool('task_start',workspace_id='tasks',executable=program,argv=[],request_key='invalid')['isError'],'only explicit native executable paths')
        interactive=start('input','input')
        call('task_input',task_id=interactive['task_id'],input_key='line',text='输入 😀\n')
        call('task_input',task_id=interactive['task_id'],input_key='line',text='输入 😀\n')
        check(bot.tool('task_input',task_id=interactive['task_id'],input_key='line',text='CHANGED')['isError'],'input receipt binds exact data')
        call('task_input',task_id=interactive['task_id'],input_key='eof',text='',eof=True)
        wait(interactive['task_id'])
        check(output(interactive['task_id'])=='输入 😀\n'.encode(),'queued stdin retry never duplicates data and EOF is delivered')
        check(bot.tool('task_input',task_id=interactive['task_id'],input_key='late',text='late')['isError'],'no new stdin after closure')
        unrelated=subprocess.Popen([sys.executable,str(fixture),'wait'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        sleeper=start('cancel','wait')
        handle=kernel.OpenProcess(0x100000,False,sleeper['process_id'])
        check(bool(handle),'capture real task process handle')
        call('task_cancel',task_id=sleeper['task_id']);call('task_cancel',task_id=sleeper['task_id'])
        check(wait(sleeper['task_id'])['status']=='cancelled','cancel has a distinct terminal reason')
        check(kernel.WaitForSingleObject(handle,3000)==0,'cancel terminates owned process');kernel.CloseHandle(handle)
        check(unrelated.poll() is None,'cancel preserves unrelated process using same executable')
        timed=start('timeout','wait',timeout_ms=300)
        check(wait(timed['task_id'])['status']=='timed_out','wall clock timeout has distinct terminal reason')
        flooded=start('flood','flood')
        check(wait(flooded['task_id'])['status']=='output_limit','overflow terminates bounded task')
        check(len(output(flooded['task_id']))==262144,'output retains exact bounded prefix')
        tree=start('tree','tree')
        end=time.monotonic()+8;child_id=None
        while time.monotonic()<end:
            page=call('task_output',task_id=tree['task_id'])
            lines=base64.b64decode(page['data_base64']).splitlines()
            if lines: child_id=int(lines[0]);break
            time.sleep(.025)
        check(child_id is not None,'job descendant fixture started')
        child_handle=kernel.OpenProcess(0x100000,False,child_id)
        check(child_handle and call('task_status',task_id=tree['task_id'])['status']=='running','task remains active after root exit while its child lives')
        call('task_cancel',task_id=tree['task_id']);wait(tree['task_id'])
        check(kernel.WaitForSingleObject(child_handle,3000)==0,'cancel terminates descendant after root exit');kernel.CloseHandle(child_handle)
        active=[start('active-'+str(i),'wait') for i in range(8)]
        check(bot.tool('task_start',workspace_id='tasks',executable=sys.executable,argv=[str(fixture),'wait'],request_key='over-active')['isError'],'active task cap is enforced')
        for task in active: call('task_cancel',task_id=task['task_id']);wait(task['task_id'])
        check(bot.tool('task_status',task_id='0'*32)['isError'],'unknown task cannot be queried')
        # Records are retained for idempotency and never silently evicted.
        filled=0
        for i in range(40):
            response=bot.tool('task_start',workspace_id='tasks',executable=str(executable),argv=['--version'],request_key='record-'+str(i))
            if response['isError']:
                check('record' in response['structuredContent']['error'],'finite record budget, no automatic eviction');break
            filled+=1;wait(response['structuredContent']['task_id'])
        check(filled>0 and start('append','append','once.txt','ONE')['task_id']==written['task_id'],'old receipt survives budget exhaustion')
    finally:
        bot.close()
        if unrelated is not None:
            unrelated.terminate();unrelated.wait(timeout=5)
    journals=list((state/'audit').glob('*.jsonl'))
    if not journals: journals=list(state.rglob('audit-*.jsonl'))
    rows=[json.loads(line) for path in journals for line in path.read_text().splitlines()]
    check(any(row['event']=='task_started' for row in rows),'task startup is durably recorded')
    check(sum(row['event']=='task_started' and row.get('request_key')=='append' for row in rows)==1,'new task startup result is recorded once')
    check('fixture-credential-not-for-child' not in '\n'.join(path.read_text() for path in journals),'audit omits credentials')
    check(all('argv' not in row and 'text' not in row and 'stdout' not in row for row in rows),'task audit omits arguments stdin and output bodies')
    # Host EOF must close its task jobs, including children, independently of MCP polling.
    state2=folder/'task-shutdown-state'
    bot=Stdio(executable,['--config',str(configuration),'--data-dir',str(state2)])
    try:
        bot.call(initialize());bot.send({'jsonrpc':'2.0','method':'notifications/initialized'})
        bind(bot)
        check(bot.tool('task_start',host_instance_id=original_instance,workspace_id='tasks',executable=str(executable),argv=['--version'],request_key='stale')['isError'],'host restart rejects stale request before any launch')
        check(bot.tool('task_status',task_id=written['task_id'])['isError'],'host restart does not restore old task receipts')
        last=bot.tool('task_start',workspace_id='tasks',executable=sys.executable,argv=[str(fixture),'wait'],request_key='shutdown')['structuredContent']
        handle=kernel.OpenProcess(0x100000,False,last['process_id'])
        check(bool(handle),'capture shutdown task process')
    finally:
        bot.close()
    check(kernel.WaitForSingleObject(handle,3000)==0,'host shutdown kills its task job');kernel.CloseHandle(handle)
    # Disabled and controlled modes fail closed regardless of remote parameters.
    for index,settings in enumerate([{'enabled':False,'profile':'user'},{'enabled':True,'profile':'controlled'}]):
        configuration.write_text(json.dumps({'roots':[{'id':'tasks','path':str(root)}],'execution':settings}),encoding='utf-8')
        bot=Stdio(executable,['--config',str(configuration),'--data-dir',str(folder/('task-disabled-'+str(index)))])
        try:
            bot.call(initialize());bot.send({'jsonrpc':'2.0','method':'notifications/initialized'})
            bind(bot)
            check(bot.tool('task_start',workspace_id='tasks',executable=sys.executable,argv=[str(fixture),'append','never.txt','NO'],request_key='never')['isError'],'disabled or unfinished controlled backend cannot launch')
            check(not (root/'never.txt').exists(),'disabled execution has no command file side effects')
        finally: bot.close()
