import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time

def task_http_checks(executable,folder,check):
    root=folder/'http-task-workspace';root.mkdir()
    configuration=folder/'http-tasks.json';state=folder/'http-task-state'
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    configuration.write_text(json.dumps({'roots':[{'id':'test','path':str(root)}],'execution':{'enabled':True,'profile':'user'},'http':{'port':port}}),encoding='utf-8')
    environment=os.environ.copy();token=secrets.token_hex(32);environment['SHELLBOT_TOKEN']=token
    host=subprocess.Popen([str(executable),'--http','--config',str(configuration),'--data-dir',str(state)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=environment)
    def request(name,args):
        connection=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
        try:
            body=json.dumps({'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':name,'arguments':args}})
            connection.request('POST','/mcp',body,{'Authorization':'Bearer '+token,'Content-Type':'application/json','Accept':'application/json, text/event-stream','MCP-Protocol-Version':'2025-11-25'})
            response=connection.getresponse();raw=response.read()
            if response.status!=200: raise AssertionError('Task HTTP request failed without exposing credentials.')
            value=json.loads(raw)
            if 'result' not in value: raise AssertionError('Task HTTP RPC failed.')
            return value['result']
        finally: connection.close()
    try:
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                capabilities=request('host_capabilities',{})['structuredContent'];break
            except (ConnectionError,OSError): time.sleep(.05)
        else: raise AssertionError('Task HTTP host did not become ready.')
        instance=capabilities['execution_status']['host_instance_id']
        fixture=Path(__file__).with_name('task_fixture.py').resolve()
        args={'host_instance_id':instance,'workspace_id':'test','executable':sys.executable,'argv':[str(fixture),'append','once.txt','ONCE'],'request_key':'parallel-reconnect'}
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            replies=list(pool.map(lambda _:request('task_start',args),range(12)))
        check(all(not value['isError'] for value in replies),'concurrent authenticated HTTP task starts succeed')
        ids={value['structuredContent']['task_id'] for value in replies}
        check(len(ids)==1,'fresh HTTP connections bind one task for the same caller request key')
        check(sum(value['structuredContent']['created_new'] for value in replies)==1,'exactly one concurrent request creates the task')
        task_id=ids.pop();deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            result=request('task_status',{'host_instance_id':instance,'task_id':task_id})['structuredContent']
            if result['status']!='running':break
            time.sleep(.025)
        check(result['status']=='completed' and (root/'once.txt').read_text()=='ONCE','HTTP retry does not duplicate command side effects')
        replay=request('task_start',args)['structuredContent']
        check(replay['task_id']==task_id and not replay['created_new'],'reconnect returns original completed receipt')
        check(request('task_output',{'host_instance_id':instance,'task_id':task_id})['structuredContent']['text_utf8'].strip()=='appended','UTF-8 task stdout is directly readable through MCP')
        wrong=dict(args,host_instance_id='0'*32,request_key='wrong-instance')
        check(request('task_start',wrong)['isError'],'stale HTTP host instance cannot start another task')
    finally:
        stopped=subprocess.run([str(executable),'--stop','--data-dir',str(state)],capture_output=True,timeout=10)
        if host.poll() is None:
            try:host.wait(timeout=10)
            except subprocess.TimeoutExpired:host.kill();host.wait(timeout=5)
        host.stdout.close();host.stderr.close()
        check(stopped.returncode==0 and host.returncode==0,'HTTP task host stops through local owner channel')
