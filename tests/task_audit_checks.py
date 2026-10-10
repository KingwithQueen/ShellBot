"""Task audit failure must block new effects while allowing cancellation."""
import json
import sys
import time
from pathlib import Path


def task_audit_checks(executable, folder, Stdio, initialize, check):
    workspace = folder / 'task-audit-workspace'
    workspace.mkdir()
    state = folder / 'task-audit-state'
    config = folder / 'task-audit.json'
    config.write_text(json.dumps({'roots':[{'id':'tasks','path':str(workspace)}],
        'audit_max_bytes':4096,'execution':{'enabled':True,'profile':'user'}}), encoding='utf-8')
    fixture = Path(__file__).with_name('task_fixture.py').resolve()
    bot = Stdio(executable, ['--config',str(config),'--data-dir',str(state)])
    try:
        bot.call(initialize())
        bot.send({'jsonrpc':'2.0','method':'notifications/initialized'})
        instance = bot.tool('host_capabilities')['structuredContent']['execution_status']['host_instance_id']
        def tool(name, **args):
            return bot.tool(name, host_instance_id=instance, **args)
        response = tool('task_start',workspace_id='tasks',executable=sys.executable,
            argv=[str(fixture),'input'],request_key='audit-active')
        check(not response['isError'], 'start a real stdin task before audit budget exhaustion')
        task_id = response['structuredContent']['task_id']
        for index in range(40):
            rejected = tool('task_start',workspace_id='tasks',executable='invalid.exe',argv=[],request_key='consume-'+str(index))
            check(rejected['isError'], 'invalid program never launches while consuming bounded operation audit')
            if not bot.tool('host_capabilities')['structuredContent']['audit_healthy']:
                break
        check(not bot.tool('host_capabilities')['structuredContent']['audit_healthy'], 'task operation intents exhaust the audit budget')
        check(tool('task_start',workspace_id='tasks',executable=sys.executable,
            argv=[str(fixture),'append','blocked.txt','NO'],request_key='blocked')['isError'], 'audit failure blocks new tasks')
        check(not (workspace/'blocked.txt').exists(), 'blocked task has no file side effect')
        check(tool('task_input',task_id=task_id,input_key='blocked-input',text='NOT_DELIVERED')['isError'], 'audit failure blocks stdin changes')
        check(not tool('task_status',task_id=task_id)['isError'], 'audit failure preserves task status reads')
        check(not tool('task_output',task_id=task_id)['isError'], 'audit failure preserves output reads')
        check(not tool('task_cancel',task_id=task_id)['isError'], 'audit failure still allows terminating owned tasks')
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            status = tool('task_status',task_id=task_id)['structuredContent']['status']
            if status != 'running': break
            time.sleep(.025)
        check(status == 'cancelled', 'cancel after audit failure reaches the real terminal state')
    finally:
        bot.close()
    check(all('NOT_DELIVERED' not in path.read_text() for path in state.glob('audit-*.jsonl')), 'failed stdin text never enters journal bodies')
