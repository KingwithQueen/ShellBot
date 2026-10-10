"""M6 local HTTP owner boundary and durable recovery. No real remote client claim."""
import argparse, base64, copy, ctypes, datetime, hashlib, http.client, json, os, secrets, socket, subprocess, time, uuid
from http import client as http_client
from pathlib import Path


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--exe',required=True,type=Path)
    parser.add_argument('--report',required=True,type=Path)
    parser.add_argument('--administrator',action='store_true')
    parser.add_argument('--ui',action='store_true')
    args=parser.parse_args()
    exe=args.exe.resolve(strict=True); fixture=exe.with_name('ShellBot-TestFixture.exe')
    folder=Path(__file__).resolve().parents[1]/'build'/'m6-tests'/(datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8]);folder.mkdir(parents=True)
    report={'stage':'M6','exe':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'artifacts':str(folder),'checks':[],
            'passed':False,'mode':'administrator' if args.administrator else 'ordinary','uac_attempted':False,'uac_dialog_count':'not_observed_automatically','real_remote_client':'pending'}
    tokens={key:secrets.token_hex(32) for key in ('a','b','owner')};flags=subprocess.CREATE_NO_WINDOW
    data=folder/'manager';process=None;instances={};workers={};agent_pid=None
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[ctypes.c_uint32,ctypes.c_int,ctypes.c_uint32];kernel.OpenProcess.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    kernel.TerminateProcess.argtypes=[ctypes.c_void_p,ctypes.c_uint32]
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def check(ok,name):
        report['checks'].append({'name':name,'passed':bool(ok)})
        if not ok:raise AssertionError(name)
    def save(name,value):
        path=folder/name;path.write_text(json.dumps(value,ensure_ascii=False),encoding='utf-8');return path
    def journal_rows(path):
        for line in path.read_bytes().splitlines():
            try:yield json.loads(line)
            except ValueError:pass  # The named torn-final-line fixture is retained.
    def control(command,project=None,request=None,failed=False):
        argv=[str(exe),command]+([project] if project else [])+['--manager-data-dir',str(data)]
        if request is not None:argv+=['--request',str(save('owner-'+uuid.uuid4().hex+'.json',request))]
        p=subprocess.run(argv,capture_output=True,timeout=40,creationflags=flags)
        check((p.returncode!=0)==failed,command+' expected exit')
        return json.loads(p.stdout)
    def http(path,payload=None,token=None,verb='POST',status=200,changes=None,raw=None):
        headers={'Content-Type':'application/json','Accept':'application/json'}
        if token is not None:headers['Authorization']='Bearer '+token
        if path.endswith('/mcp') and payload:
            headers.update({'MCP-Protocol-Version':'2026-07-28','Mcp-Method':payload['method']})
            if payload['method']=='tools/call':headers['Mcp-Name']=payload['params']['name']
        headers.update(changes or {})
        body=raw if raw is not None else json.dumps(payload).encode() if payload is not None else None
        connection=http_client.HTTPConnection('127.0.0.1',port,timeout=20)
        try:
            connection.request(verb,path,body=body,headers=headers);response=connection.getresponse();wire=response.read()
            check(response.status==status,'HTTP '+verb+' '+path+' status '+str(status))
            check(response.getheader('Cache-Control')=='no-store','owner/project responses never cache')
            return json.loads(wire) if 'application/json' in response.getheader('Content-Type','') else wire.decode('utf-8'),dict(response.getheaders())
        finally:connection.close()
    def rpc(project,name,arguments=None,caps=None,state=None,action=None,legacy=False):
        params={'name':name,'arguments':arguments or {}}
        if not legacy:params['_meta']={'io.modelcontextprotocol/protocolVersion':'2026-07-28','io.modelcontextprotocol/clientCapabilities':caps or {}}
        if state:params['requestState']=state
        if action:params['inputResponses']={state:{'action':action}}
        payload={'jsonrpc':'2.0','id':1,'method':'tools/call','params':params}
        changes={'MCP-Protocol-Version':'2025-11-25'} if legacy else {}
        return http('/projects/'+project+'/mcp',payload,tokens[project],changes=changes)[0]
    def tool(project,name,arguments=None,failed=False,**kwargs):
        response=rpc(project,name,arguments,**kwargs)
        check(response.get('result',{}).get('isError')==failed,project+' '+name+' expected tool result')
        return response['result']['structuredContent']
    def owner(command,project='a',backend='ordinary',**extra):
        payload={'command':command} if command=='projects' else {'command':command,'project_id':project,'backend':backend,**extra}
        return http('/owner/api',payload,tokens['owner'])[0]
    def inspect(id,project='a',backend='ordinary'):return owner('inspect',project,backend,id=id)
    def decide(command,id,project='a',backend='ordinary',**changes):
        item=inspect(id,project,backend)
        binding={'expected_sha256':item['operation_sha256'],'expected_host_instance_id':item['host_instance_id'],'expected_policy_revision':item['policy_revision'],**changes}
        return owner(command,project,backend,id=id,**binding)
    def launch(registry,credentials=None,valid=True,manager_data=None):
        env={**os.environ,'SHELLBOT_A_TOKEN':tokens['a'],'SHELLBOT_B_TOKEN':tokens['b'],'SHELLBOT_OWNER_TOKEN':tokens['owner'],**(credentials or {})}
        p=subprocess.Popen([str(exe),'--manager','--http','--projects',str(registry),'--manager-data-dir',str(manager_data or data)],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=flags,env=env)
        if valid:
            ready=p.stdout.readline();check(ready and json.loads(ready)['manager_ready'],'M6 manager starts unelevated')
        else:
            out,err=p.communicate(timeout=20);check(p.returncode==2 and not out and err,'invalid independent owner credential rejects startup')
            check(all(t.encode() not in err for t in tokens.values()),'startup error omits credentials')
        return p
    def start(project='a'):
        value=control('--project-start',project)['project'];instances[project]=value['host_instance_id'];workers[project]=value['worker_pid'];return value
    def task_arguments(mode,key,identity=None):
        value={'workspace_id':'work','executable':str(fixture),'argv':[mode],'request_key':key,'host_instance_id':instances['a'],'timeout_ms':5000}
        if identity:value['identity']=identity
        return value
    def approved_task(mode,key,identity=None,timeout=5000):
        value=task_arguments(mode,key,identity);value['timeout_ms']=timeout
        pending=tool('a','task_start',value,failed=True);id=pending['approval_id'];backend='administrator' if identity else 'ordinary'
        check(decide('approve',id,backend=backend)['status']=='approved','remote owner approves native task')
        value['approval_id']=id;return tool('a','task_start',value),value
    def await_task(task,identity=None):
        query={'task_id':task['task_id'],'host_instance_id':task['host_instance_id']}
        if identity:query['identity']=identity
        until=time.monotonic()+8
        while time.monotonic()<until:
            result=tool('a','task_status',query)
            if result['status'] not in ('starting','running'):return result
            time.sleep(.08)
        raise AssertionError('task completion deadline')
    def history_record(id,backend='ordinary'):
        items=owner('history',backend=backend,id=id)['records'];check(len(items)==1,'protected history finds exact record');return items[0]
    def alive(pid):
        handle=kernel.OpenProcess(0x100000,False,pid)
        if not handle:return False
        result=kernel.WaitForSingleObject(handle,0)==258;kernel.CloseHandle(handle);return result
    try:
        projects=[]
        for project in ('a','b'):
            workspace=folder/('workspace-'+project);workspace.mkdir();(workspace/'read.txt').write_text('private-'+project,encoding='utf-8')
            projects.append({'id':project,'data_dir':str(folder/('data-'+project)),'token_env':'SHELLBOT_'+project.upper()+'_TOKEN','policy_revision':4,
                             'workspaces':[{'id':'work','path':str(workspace),'read_only':False}],
                             'policy':{'approval_mode':'always','files':{'scope':'current_user','current_user_read_only':False},
                                       'execution':{'enabled':True,'default_identity':'current_user','administrator_allowed':bool(args.administrator)},
                                       'tools':['host_info','host_capabilities','fs_list','fs_read','fs_search','image_view','fs_write','fs_patch','task_start','task_status','task_output','task_input','task_cancel']}})
        registry=save('registry.json',{'format_version':1,'gateway':{'port':port,'owner_token_env':'SHELLBOT_OWNER_TOKEN'},'projects':projects})
        for bad in ('', 'short',tokens['a'],' '*64,'x'*257):
            launch(registry,{'SHELLBOT_OWNER_TOKEN':bad},False,folder/('bad-'+uuid.uuid4().hex))
        process=launch(registry)
        page,headers=http('/owner',verb='GET');script,_=http('/owner.js',verb='GET')
        check('所有者审批' in page and '/owner.js' in page and 'frame-ancestors' in headers['Content-Security-Policy'],'owner page renders public bounded shell with frame protection')
        check('textContent' in script and 'innerHTML' not in script and 'localStorage' not in script and 'credentials:\'omit\'' in script,'owner UI renders untrusted parameters as text without credential storage')
        http('/owner/api',{'command':'projects'},tokens['a'],status=401);http('/owner/api',{'command':'projects'},status=401)
        http('/owner/api',{'command':'projects'},tokens['owner'],changes={'Origin':'https://other.invalid'},status=403)
        http('/owner/api?token=ignored',{'command':'projects'},tokens['owner'],status=400)
        http('/%6fwner/api',{'command':'projects'},tokens['owner'],status=400)
        http('/owner/api',{'command':'project-start','project_id':'a','backend':'ordinary'},tokens['owner'],status=400)
        http('/owner/api',token=tokens['owner'],raw=b'{"command":"projects","command":"projects"}',status=400)
        http('/owner/api',{'command':'approve','project_id':'a','backend':'ordinary','id':'0'*32},tokens['owner'],status=400)
        check(owner('projects')['projects'][0]['state']=='stopped','remote endpoint does not start a worker')
        start();start('b')
        read={'workspace_id':'work','path':'read.txt','max_bytes':32}
        modern=rpc('a','fs_read',read,caps={'elicitation':{'url':{}}});required=modern['result'];id=required['requestState']
        check(required['resultType']=='input_required' and required['inputRequests'][id]['method']=='elicitation/create','verified URL capability receives standard input_required result')
        url=required['inputRequests'][id]['params']['url'];check('/owner#project=a&backend=ordinary&approval='+id in url and all(t not in url for t in tokens.values()),'approval URL contains binding hints and no credential')
        pending=tool('a','fs_read',read,failed=True,state=id,action='accept');check(pending['status']=='pending','client accept alone cannot approve')
        for caps in ({},{'elicitation':{}},{'elicitation':{'form':{}}},{'elicitation':{'url':True}}):
            response=rpc('a','fs_read',read,caps=caps);check(response['result']['resultType']=='complete' and response['result']['structuredContent']['status']=='pending','missing URL capability uses explicit pending fallback')
            check(json.loads(response['result']['content'][0]['text'])['approval_url']==response['result']['structuredContent']['approval_url'],'text-only client sees the same actionable approval URL')
        check(tool('a','fs_read',read,failed=True,legacy=True)['approval_url'].startswith('http://127.0.0.1:'),'2025 client receives an approval link without nested RPC')
        metadata=tool('a','approval_status',{'approval_id':id});check(metadata['status']=='pending' and 'text' not in metadata,'caller queries only bounded metadata')
        tool('b','approval_status',{'approval_id':id},failed=True)
        local=control('--project-request','a',{'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':'approval_status','arguments':{'approval_id':id},'_meta':{'io.modelcontextprotocol/protocolVersion':'2026-07-28','io.modelcontextprotocol/clientCapabilities':{}}}})
        check(local['response']['result']['isError'],'local MCP actor cannot inspect remote caller request')
        full=inspect(id);check(json.loads(full['text'])['path']=='read.txt' and full['actor']=='project-mcp','owner sees exact normalized parameters and trusted actor')
        for changes in ({'expected_sha256':'0'*64},{'expected_host_instance_id':'0'*32},{'expected_policy_revision':5}):
            check('error' in decide('approve',id,**changes),'stale inspected binding rejects owner decision')
        check(decide('approve',id)['status']=='approved','independent owner authorizes exact read')
        result=tool('a','fs_read',read,state=id,action='accept');check(result['text']=='private-a','approved continuation returns actual content')
        tool('a','fs_read',{**read,'path':'missing.txt'},failed=True,state=id,action='accept')
        other=tool('a','fs_read',{**read,'max_bytes':16},failed=True)
        cancelled=tool('a','fs_read',{**read,'max_bytes':16},state=other['approval_id'],action='decline');check(cancelled['status']=='cancelled','client decline cancels only own pending request')
        check('error' in decide('approve',other['approval_id']),'cancelled request cannot be approved')
        denied=tool('a','fs_list',{'workspace_id':'work'},failed=True);check(decide('deny',denied['approval_id'])['status']=='denied','remote explicit deny is retained')
        write={'workspace_id':'work','path':'created.txt','text':'M6 exact bytes'}
        pending_write=tool('a','fs_write',write,failed=True);check('approval_id' in pending_write,'write creates pending request: '+str(pending_write));check(not (folder/'workspace-a'/'created.txt').exists(),'write has no effect before owner decision')
        decide('approve',pending_write['approval_id']);completed=tool('a','fs_write',{**write,'approval_id':pending_write['approval_id']})
        check((folder/'workspace-a'/'created.txt').read_text()=='M6 exact bytes','approved file write has exact real effect')
        tool('a','fs_write',{**write,'approval_id':pending_write['approval_id']})
        if args.ui:
            uiargs={'workspace_id':'work','path':'ui.txt','text':'<img src=x onerror=alert(1)> literal owner inspection'}
            ui=tool('a','fs_write',uiargs,failed=True);item=inspect(ui['approval_id'])
            browser=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft/Edge/Application/msedge.exe'
            config={'browser':str(browser),'profile':str(folder/'ui-profile'),'url':f'http://127.0.0.1:{port}/owner#project=a&backend=ordinary&approval='+ui['approval_id'],
                    'token':tokens['owner'],'id':ui['approval_id'],'digest':item['operation_sha256'],'screenshot':str(folder/'owner-ui.png')}
            p=subprocess.run(['node',str(Path(__file__).with_name('owner_ui_checks.mjs'))],input=json.dumps(config).encode(),capture_output=True,timeout=45,creationflags=flags)
            result=json.loads(p.stdout);report['browser_ui']=result;check(p.returncode==0 and result['passed'],'actual Edge owner UI: '+str(result))
            tool('a','fs_write',{**uiargs,'approval_id':ui['approval_id']});check((folder/'workspace-a'/'ui.txt').read_text()==uiargs['text'],'UI approval authorizes exact inspected bytes')
        task,_=approved_task('identity','identity');check(await_task(task)['exit_code']==0,'ordinary native task finishes')
        output=tool('a','task_output',{'task_id':task['task_id'],'host_instance_id':task['host_instance_id'],'stream':'stdout'})
        identity=json.loads(base64.b64decode(output['data_base64']));check(not identity['owner_token_inherited'] and not identity['project_tokens_inherited'] and not identity['elevated'],'ordinary task inherits no owner/project credential and stays unelevated')
        task_history=history_record(task['task_id']);check(json.loads(base64.b64decode(task_history['stdout_base64']))==identity,'protected task history retains completed real stdout')
        interactive,_=approved_task('stdin','interactive')
        inp={'task_id':interactive['task_id'],'host_instance_id':interactive['host_instance_id'],'input_key':'one','text':'M6 input once','eof':True}
        ip=tool('a','task_input',inp,failed=True);decide('approve',ip['approval_id']);inp['approval_id']=ip['approval_id']
        tool('a','task_input',inp);tool('a','task_input',inp);await_task(interactive)
        record=history_record(interactive['task_id']);check(base64.b64decode(record['stdout_base64'])==b'M6 input once' and record['input_receipts'][0]['status']=='delivered','stdin and EOF retained as exactly one delivered receipt')
        timed,_=approved_task('wait','timeout',timeout=200);check(await_task(timed)['status']=='timed_out','timeout completion is observed')
        check(history_record(timed['task_id'])['status']=='timed_out','timeout result persists')
        stranded=tool('a','fs_read',{**read,'max_bytes':8},failed=True);decide('approve',stranded['approval_id'])
        live,_=approved_task('wait','abrupt',timeout=60000);old_instance=instances['a'];old_worker=workers['a']
        kernel.TerminateProcess(process._handle,125);process.wait(timeout=10);process=None
        until=time.monotonic()+5
        while alive(old_worker) and time.monotonic()<until:time.sleep(.05)
        check(not alive(old_worker) and not alive(live['process_id']),'abrupt manager exit kills verified worker and task')
        process=launch(registry);start();start('b');check(instances['a']!=old_instance,'explicit restart has a fresh host identity')
        recovered=history_record(stranded['approval_id']);check(recovered['status']=='interrupted' and recovered['previous_status']=='approved' and not recovered['active_instance'],'restart marks old approval interrupted without restoring permission')
        ended=history_record(live['task_id']);check(ended['status']=='interrupted' or (ended['status']=='cancelled' and 'exit_code' in ended),'abrupt task shows interrupted or confirmed cancellation, never presumed success')
        check(tool('a','approval_status',{'approval_id':stranded['approval_id']})['status']=='interrupted','requester can query its recovered metadata')
        check('error' in owner('approve',id=stranded['approval_id'],expected_sha256=recovered['operation_sha256'],expected_host_instance_id=old_instance,expected_policy_revision=4),'old instance approval cannot be consumed after restart')
        check(history_record(pending_write['approval_id'])['status']=='completed','completed file receipt remains available after restart')
        check(history_record(interactive['task_id'])['input_receipts'][0]['status']=='delivered','input receipts survive restart without automatic dispatch')
        repeated=tool('a','task_start',task_arguments('append','identity'),failed=True)
        check('error' in repeated and 'approval_id' not in repeated and not (folder/'workspace-a'/'once.txt').exists(),'retained ordinary request key cannot start again after restart')
        if args.administrator:
            report['uac_attempted']=True;control('--administrator-start')
            until=time.monotonic()+120
            while time.monotonic()<until:
                value=control('--administrator-status')
                if value.get('administrator_alive'):break
                if not value.get('administrator_starting'):raise AssertionError('administrator UAC was not accepted')
                time.sleep(.3)
            check(value.get('administrator_alive'),'user accepted real UAC for M6 agent')
            agent_pid=value['execution_status']['administrator_identity']['process_id']
            admin_read={**read,'identity':'administrator'};ap=tool('a','fs_read',admin_read,failed=True)
            check('error' in inspect(ap['approval_id']),'ordinary owner pool cannot inspect admin request')
            decide('approve',ap['approval_id'],backend='administrator');check(tool('a','fs_read',admin_read,state=ap['approval_id'],action='accept')['text']=='private-a','admin continuation crosses manager canonical operation check')
            admin_task,_=approved_task('identity','admin-identity','administrator');await_task(admin_task,'administrator')
            admin_record=history_record(admin_task['task_id'],'administrator');ident=json.loads(base64.b64decode(admin_record['stdout_base64']))
            check(ident['elevated'] and not ident['owner_token_inherited'] and not ident['project_tokens_inherited'],'actual elevated task has no independent owner credential')
            pending_admin=tool('a','fs_list',{'workspace_id':'work','identity':'administrator'},failed=True)
            control('--project-stop','a');start()
            check(history_record(pending_admin['approval_id'],'administrator')['status']=='interrupted','admin history survives ordinary worker restart in fixed private Data')
            check(history_record(admin_task['task_id'],'administrator')['status']=='completed','admin task output survives project restart')
            live_admin,_=approved_task('wait','admin-shutdown','administrator',timeout=60000)
            agent_pending=tool('a','fs_read',{**read,'max_bytes':12,'identity':'administrator'},failed=True)
            retained_instance=instances['a'];state_before=history_record(agent_pending['approval_id'],'administrator')['state_instance_id']
            control('--administrator-stop');check(not alive(agent_pid) and not alive(live_admin['process_id']),'agent stop cancels real elevated job before releasing durable state');agent_pid=None
            print('M6: explicit administrator restart; accept the second UAC prompt.',flush=True)
            control('--administrator-start');until=time.monotonic()+120
            while time.monotonic()<until:
                value=control('--administrator-status')
                if value.get('administrator_alive'):break
                if not value.get('administrator_starting'):raise AssertionError('administrator restart UAC was not accepted')
                time.sleep(.3)
            check(value.get('administrator_alive'),'administrator explicitly restarts after accepted UAC');agent_pid=value['execution_status']['administrator_identity']['process_id']
            check(instances['a']==retained_instance,'agent restart leaves ordinary project identity unchanged')
            past=history_record(agent_pending['approval_id'],'administrator')
            check(past['status']=='interrupted' and not past['active_instance'] and past['state_instance_id']==state_before,'old agent pending state is historical even with same ordinary host id')
            blocked=tool('a','task_start',task_arguments('append','admin-shutdown','administrator'),failed=True)
            check('error' in blocked and 'approval_id' not in blocked and not (folder/'workspace-a'/'once.txt').exists(),'retained admin key cannot replay in a fresh agent')
            fresh,_=approved_task('identity','fresh-agent-key','administrator');check(await_task(fresh,'administrator')['exit_code']==0,'new key executes only after new owner approval')
            control('--administrator-stop');check(not alive(agent_pid),'restarted agent exits cleanly');agent_pid=None
        # A torn final append is retained. Earlier flushed records still recover.
        control('--project-stop','a')
        journal=next(path for path in (folder/'data-a').glob('state-*.jsonl') if any(row.get('kind')=='approval' for row in journal_rows(path)));with_torn=journal.read_bytes()+b'{"incomplete":';journal.write_bytes(with_torn)
        start();check(history_record(interactive['task_id'])['status']=='completed','final torn append recovers earlier flushed receipt');control('--project-stop','a')
        journal.write_bytes(with_torn.replace(b'"project_id":"a"',b'"project_id":"z"',1))
        control('--project-start','a',failed=True);check(journal.read_bytes().startswith(b'{'),'corrupt journal blocks startup and remains preserved')
        journal.write_bytes(with_torn);start()
        control('--project-stop','a')
        journal.write_bytes(with_torn+b'x'*(32*1024*1024))
        control('--project-start','a',failed=True);check(journal.stat().st_size>32*1024*1024,'persistent aggregate byte budget blocks startup without deleting history')
        journal.write_bytes(with_torn);start();control('--project-stop','a')
        hardlink=journal.with_name('state-'+uuid.uuid4().hex+'.jsonl')
        try:os.link(journal,hardlink)
        except OSError as error:
            if error.winerror!=50:raise
            report['hardlink_fixture']='skipped: filesystem reports ERROR_NOT_SUPPORTED'
        else:
            try:control('--project-start','a',failed=True);check(hardlink.exists(),'hard-linked state is rejected and preserved')
            finally:hardlink.unlink()
            report['hardlink_fixture']='passed'
        start()
        control('--project-stop','a')
        security=ctypes.WinDLL('advapi32',use_last_error=True);needed=ctypes.c_uint32()
        security.GetFileSecurityW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)]
        security.SetFileSecurityW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_void_p]
        security.GetFileSecurityW(str(journal),4,None,0,ctypes.byref(needed));descriptor=ctypes.create_string_buffer(needed.value)
        check(security.GetFileSecurityW(str(journal),4,descriptor,needed.value,ctypes.byref(needed)),'original protected state DACL retained')
        changed=subprocess.run(['icacls',str(journal),'/grant','*S-1-1-0:(R)'],capture_output=True,creationflags=flags)
        check(changed.returncode==0,'public read ACE applied only to named isolated state fixture')
        try:control('--project-start','a',failed=True)
        finally:check(security.SetFileSecurityW(str(journal),4|0x80000000,descriptor),'exact protected DACL restored')
        start()
        # Load 256 protected historical records, then refuse the 257th intent.
        control('--project-stop','a')
        archived=[]
        for path in (folder/'data-a').glob('state-*.jsonl'):
            retained=path.with_name('retained-'+path.name);path.rename(retained);archived.append((path,retained))
        original=next(retained for path,retained in archived if path==journal).read_bytes()
        template=next(row for row in journal_rows(next(retained for path,retained in archived if path==journal)) if row.get('kind')=='approval')
        rows=[]
        for i in range(256):
            row=copy.deepcopy(template);id=uuid.uuid4().hex;row['record_id']=id;row['sequence']=i+1;row['status']='pending'
            row['body']['approval_id']=id;row['body']['status']='pending';row.pop('sha256')
            wire=json.dumps(row,ensure_ascii=False,separators=(',',':')).encode();row['sha256']=hashlib.sha256(wire).hexdigest()
            rows.append(json.dumps(row,ensure_ascii=False,separators=(',',':')).encode()+b'\n')
        # Reuse the protected inode; never rely on default inherited permissions.
        retained=next(retained for path,retained in archived if path==journal);retained.rename(journal);journal.write_bytes(b''.join(rows))
        try:
            start();paged=owner('history',offset=0,limit=64)
            check(len(paged['records'])==64 and paged['has_more'] and all(r['status']=='interrupted' for r in paged['records']),'bounded history pages 256 records without restoring grants')
            refused=tool('a','fs_write',{'workspace_id':'work','path':'budget-no-effect.txt','text':'blocked'},failed=True)
            check('error' in refused and not (folder/'workspace-a'/'budget-no-effect.txt').exists(),'257th persistent record blocks new write before side effect')
            check(not tool('a','host_capabilities')['state_healthy'],'record-budget failure is reported')
            control('--project-stop','a')
        finally:
            journal.rename(journal.with_name('record-budget-fixture.jsonl'))
            journal.write_bytes(original)
            # Restore its original DACL, then restore untouched retained inodes.
            check(security.SetFileSecurityW(str(journal),4|0x80000000,descriptor),'record-budget original protected DACL restored')
            for path,retained in archived:
                if retained.exists():retained.rename(path)
        start()
        audit=[json.loads(line) for path in (folder/'data-a').glob('audit-*.jsonl') for line in path.read_text().splitlines()]
        events={r['event'] for r in audit}
        check({'approval_requested','approval_approved','approval_cancelled','approval_denied','operation_completed','file_call_started','file_call_completed','task_start_intent','task_started','task_input_queued','task_input_started','task_input_delivered','task_timed_out','task_completed'}<=events,'full approval/file/task/input/timeout lifecycle is audited')
        check(any(r['actor']=='remote-owner' and r['event']=='approval_approved' for r in audit),'decision audit records trusted remote owner identity')
        control('--manager-stop');process.wait(timeout=10);process=None
        # Protected ACL and integrity keep history from being trusted after tampering.
        check(all(t.encode() not in p.read_bytes() for p in folder.rglob('*') if p.is_file() for t in tokens.values()),'all retained configs/state/audit omit bearer values')
        report['passed']=True
    except Exception as error:report['failure']=repr(error);raise
    finally:
        if process and process.poll() is None:
            try:control('--manager-stop')
            except Exception:process.terminate()
            process.wait(timeout=10)
        report['checks_count']=len(report['checks']);args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps({key:report[key] for key in ('passed','checks_count','artifacts')}))


if __name__=='__main__':main()
