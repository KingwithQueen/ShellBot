"""M4 real project workers and owner approvals; all artifacts are isolated."""
import argparse, base64, copy, ctypes, datetime, hashlib, http.client, json, os, shutil, socket, subprocess, time, uuid, zlib
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('--exe',required=True);p.add_argument('--report',required=True);a=p.parse_args()
    exe=Path(a.exe).resolve();repo=Path(__file__).resolve().parents[1]
    folder=repo/'build'/'m4-tests'/(datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8]);folder.mkdir(parents=True)
    report={'stage':'M4','exe':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'artifacts':str(folder),'checks':[],'uac_attempted':False,'passed':False}
    runtime=folder/'runtime';runtime.mkdir();fixture=runtime/'ShellBot-TestFixture.exe';shutil.copy2(exe.with_name('ShellBot-TestFixture.exe'),fixture)
    security=ctypes.WinDLL('advapi32',use_last_error=True);kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    security.GetNamedSecurityInfoW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
    security.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.POINTER(ctypes.c_void_p),ctypes.c_void_p]
    kernel.LocalFree.argtypes=[ctypes.c_void_p]
    def sddl(path):
        descriptor=ctypes.c_void_p();text=ctypes.c_void_p()
        if security.GetNamedSecurityInfoW(str(path),1,7,None,None,None,None,ctypes.byref(descriptor)):raise OSError('cannot read test object ACL')
        try:
            if not security.ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor,1,7,ctypes.byref(text),None):raise ctypes.WinError(ctypes.get_last_error())
            return ctypes.wstring_at(text)
        finally:kernel.LocalFree(text);kernel.LocalFree(descriptor)
    process=None;flags=subprocess.CREATE_NO_WINDOW;data=folder/'manager'
    def check(ok,name):
        report['checks'].append({'name':name,'passed':bool(ok)})
        if not ok:raise AssertionError(name)
    def save(value):
        path=folder/(uuid.uuid4().hex+'.json');path.write_text(json.dumps(value,ensure_ascii=False),encoding='utf-8');return path
    def control(command,project=None,request=None):
        args=[str(exe),command]+([project] if project else [])+(['--request',str(save(request))] if request is not None else [])+['--manager-data-dir',str(data)]
        result=subprocess.run(args,capture_output=True,text=True,encoding='utf-8',timeout=35,creationflags=flags)
        check(result.returncode==0,command+' succeeds: '+result.stderr[:100]);return json.loads(result.stdout)
    def rpc(project,method,args=None,name=None):
        params={'_meta':{'io.modelcontextprotocol/protocolVersion':'2026-07-28','io.modelcontextprotocol/clientCapabilities':{}}}
        if name:params.update(name=name,arguments=args or {})
        return control('--project-request',project,{'jsonrpc':'2.0','id':1,'method':method,'params':params})['response']
    def tool(project,name,args=None,failed=False):
        response=rpc(project,'tools/call',args,name)
        check('result' in response and response['result']['isError']==failed,project+' '+name+(' denied/pending' if failed else ' succeeds')+' '+str(response)[:160])
        return response['result']['structuredContent']
    def owner(project,command,id=None):return control('--project-owner',project,{'command':command,**({'id':id} if id else {})})['response']
    def await_task(project,task):
        for _ in range(120):
            state=tool(project,'task_status',{'task_id':task['task_id'],'host_instance_id':task['host_instance_id']})
            if state['status'] not in ('running','starting'):return state
            time.sleep(.05)
        raise AssertionError('native task completion timeout')
    def task_output(project,task):
        result=tool(project,'task_output',{'task_id':task['task_id'],'host_instance_id':task['host_instance_id']})
        return base64.b64decode(result['data_base64'])
    def approved_start(project,argv,key):
        instance=tool(project,'host_capabilities')['execution_status']['host_instance_id']
        args={'workspace_id':'work','executable':str(fixture),'argv':argv,'request_key':key,'host_instance_id':instance,'timeout_ms':5000}
        pending=tool(project,'task_start',args,failed=True);check(pending['status']=='pending' and 'task_id' not in pending,'command stays pending')
        owner(project,'approve',pending['approval_id']);args['approval_id']=pending['approval_id']
        task=tool(project,'task_start',args);check(task['created_new'],'approved native command starts once')
        replay=tool(project,'task_start',args);check(replay['task_id']==task['task_id'] and not replay['created_new'],'approved command retry reuses receipt')
        return task
    def http_tool(project,name,args,failed):
        c=http.client.HTTPConnection('127.0.0.1',port,timeout=12)
        try:
            c.request('POST','/projects/'+project+'/mcp',json.dumps({'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':name,'arguments':args}}),
                {'Authorization':'Bearer '+tokens[project],'Content-Type':'application/json','Accept':'application/json, text/event-stream','MCP-Protocol-Version':'2025-11-25','MCP-Method':'tools/call','MCP-Name':name})
            response=c.getresponse();payload=json.loads(response.read());check(response.status==200 and payload['result']['isError']==failed,'HTTP project actor '+name);return payload['result']['structuredContent']
        finally:c.close()
    try:
        tools=['host_info','host_capabilities','fs_list','fs_read','fs_search','image_view','fs_write','fs_patch','task_start','task_status','task_output','task_input','task_cancel']
        projects=[]
        for mode in ['always','workspace_auto','never']:
            root=folder/mode;root.mkdir();(root/'sample.txt').write_text('alpha '+mode,encoding='utf-8');(root/'pages.txt').write_bytes(b'x'*120)
            extra=folder/(mode+'-extra');extra.mkdir();(extra/'private.txt').write_text('extra body',encoding='utf-8')
            ro=folder/(mode+'-ro');ro.mkdir();(ro/'protected.txt').write_text('protected',encoding='utf-8')
            projects.append({'id':mode,'token_env':'SHELLBOT_'+mode.upper(),'data_dir':str(folder/(mode+'-data')),'approval_ttl_seconds':10,
                'workspaces':[{'id':'work','path':str(root),'read_only':False},{'id':'ro','path':str(ro),'read_only':True}],
                'extra_directories':[{'id':'extra','path':str(extra),'read_only':False}],
                'policy':{'approval_mode':mode,'files':{'scope':'configured'},'tools':tools}})
        accountroot=folder/'account';accountroot.mkdir();outside=folder/'account-outside';outside.mkdir();(outside/'outside.txt').write_text('account body',encoding='utf-8')
        account=copy.deepcopy(projects[0]);account.update(id='account',token_env='SHELLBOT_ACCOUNT',data_dir=str(folder/'account-data'),workspaces=[{'id':'work','path':str(accountroot),'read_only':False},{'id':'ro','path':str(folder/'always-ro'),'read_only':True}],extra_directories=[])
        account['policy'].update(files={'scope':'current_user','current_user_read_only':False},execution={'enabled':True,'default_identity':'current_user'})
        # Separate readonly project is excluded from arbitrary current-user commands.
        account['workspaces']=account['workspaces'][:1];projects.append(account)
        restrictedroot=folder/'restricted';restrictedroot.mkdir();(restrictedroot/'allowed.txt').write_text('allowed',encoding='utf-8')
        restrictedro=folder/'restricted-ro';restrictedro.mkdir();(restrictedro/'readonly.txt').write_text('readonly',encoding='utf-8')
        restricted=copy.deepcopy(projects[0]);restricted.update(id='restricted',token_env='SHELLBOT_RESTRICTED',data_dir=str(folder/'restricted-data'),workspaces=[{'id':'work','path':str(restrictedroot),'read_only':False},{'id':'ro','path':str(restrictedro),'read_only':True}],extra_directories=[])
        restricted['policy']['execution']={'enabled':True,'default_identity':'restricted','runtime_files':[str(fixture)]};projects.append(restricted)
        restricted['policy']['approval_mode']='workspace_auto'
        quota=copy.deepcopy(projects[0]);quota.update(id='quota',token_env='SHELLBOT_QUOTA',data_dir=str(folder/'quota-data'),approval_ttl_seconds=1,extra_directories=[])
        quota['resources']={'max_approval_records':3};projects.append(quota)
        accountro=copy.deepcopy(account);accountro.update(id='account-ro',token_env='SHELLBOT_ACCOUNT_RO',data_dir=str(folder/'account-ro-data'))
        accountro['policy']['execution']['enabled']=False;accountro['workspaces'].append({'id':'ro','path':str(restrictedro),'read_only':True});projects.append(accountro)
        acl_before={str(path):sddl(path) for path in [restrictedroot,restrictedroot/'allowed.txt',restrictedro,restrictedro/'readonly.txt',fixture,exe.with_name('ShellBot-SandboxProbe.exe'),outside,outside/'outside.txt']}
        with socket.socket() as reservation:reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
        tokens={project['id']:uuid.uuid4().hex+uuid.uuid4().hex for project in projects}
        registry=save({'format_version':1,'gateway':{'port':port},'projects':projects})
        env={**os.environ,**{project['token_env']:tokens[project['id']] for project in projects}}
        protected=copy.deepcopy(restricted);protected['policy']['execution']['runtime_files']=[str(folder/'restricted-data'/'forbidden.exe')]
        forbidden=save({'format_version':1,'projects':[protected]})
        rejected=subprocess.run([str(exe),'--manager','--projects',str(forbidden),'--manager-data-dir',str(folder/'protected-runtime-manager')],capture_output=True,text=True,encoding='utf-8',timeout=15,creationflags=flags)
        check(rejected.returncode!=0 and 'runtime resources overlap protected' in rejected.stderr,'runtime declarations cannot grant access to private management Data')
        process=subprocess.Popen([str(exe),'--manager','--http','--projects',str(registry),'--manager-data-dir',str(data)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',creationflags=flags,env=env)
        check(json.loads(process.stdout.readline())['manager_ready'],'manager ready')
        try:
            with registry.open('r+b'):pass
            locked=False
        except PermissionError:locked=True
        check(locked,'active manager pins its registry against write or replacement')
        for project in projects:control('--project-start',project['id'])
        sandboxcaps=tool('restricted','host_capabilities');report['restricted_activation']=sandboxcaps['execution_status']
        check(sandboxcaps['execution_status']['controlled_isolation_verified'],'restricted activation passed: '+str(sandboxcaps['execution_status']))
        check(sandboxcaps['approval_mode']==sandboxcaps['execution_status']['approval_mode']=='workspace_auto','execution status reports the actual project approval mode')
        instance=sandboxcaps['execution_status']['host_instance_id']
        tool('restricted','task_start',{'workspace_id':'work','executable':str(exe),'argv':['--version'],'request_key':'unlisted-program','host_instance_id':instance},failed=True)
        tool('account-ro','fs_write',{'path':str(restrictedro/'readonly.txt'),'text':'forbidden','overwrite':True,'expected_sha256':hashlib.sha256(b'readonly').hexdigest()},failed=True)
        check((restrictedro/'readonly.txt').read_text()=='readonly','absolute account path obeys physical readonly override')
        for mode in ['always','workspace_auto','never']:
            base={'workspace_id':'work','path':'sample.txt'}
            value=tool(mode,'fs_read',base,failed=mode=='always')
            if mode=='always':
                check(value['status']=='pending' and 'text' not in value,'pending read exposes no body')
                id=value['approval_id'];view=owner(mode,'inspect',id);check('fs_read' in view['tool'] and 'sample.txt' in view['text'],'owner inspects operation')
                owner(mode,'approve',id);value=tool(mode,'fs_read',{**base,'approval_id':id})
                tool('never','fs_read',{**base,'approval_id':id},failed=True)
                tool(mode,'fs_read',{**base,'path':'pages.txt','approval_id':id},failed=True)
            check(value['text']=='alpha '+mode,'read correct project body')
            write={'workspace_id':'work','path':'new.txt','text':'created'}
            value=tool(mode,'fs_write',write,failed=mode=='always')
            if mode=='always':owner(mode,'approve',value['approval_id']);write['approval_id']=value['approval_id'];value=tool(mode,'fs_write',write)
            check((folder/mode/'new.txt').read_text()=='created','write executes after correct policy')
            again=tool(mode,'fs_write',write);check(again['sha256']==value['sha256'],'write receipt retry')
            edit={**write,'overwrite':True,'text':'changed','expected_sha256':hashlib.sha256(b'created').hexdigest()};edit.pop('approval_id',None)
            value=tool(mode,'fs_write',edit,failed=mode=='always')
            if mode=='always':owner(mode,'approve',value['approval_id']);edit['approval_id']=value['approval_id'];value=tool(mode,'fs_write',edit)
            check(value['backup_id'] and (folder/mode/'new.txt').read_text()=='changed','edit keeps backup and hash')
            tool(mode,'fs_write',{'workspace_id':'ro','path':'new.txt','text':'no'},failed=True)
            extra={'workspace_id':'extra','path':'private.txt'};v=tool(mode,'fs_read',extra,failed=mode!='never')
            if mode!='never':owner(mode,'deny',v['approval_id']);tool(mode,'fs_read',{**extra,'approval_id':v['approval_id']},failed=True)
        pages={'workspace_id':'work','path':'pages.txt','max_bytes':40}
        v=tool('always','fs_read',pages,failed=True);owner('always','approve',v['approval_id']);pages['approval_id']=v['approval_id']
        first=tool('always','fs_read',pages);check(first['next_offset']==40,'first bounded page')
        replay=tool('always','fs_read',pages);check(replay['text']==first['text'],'same page retry')
        tool('always','fs_read',{**pages,'offset':80},failed=True)
        second=tool('always','fs_read',{**pages,'offset':40,'encoding':first['encoding']});check(second['next_offset']==80,'page continues same permit')
        (folder/'always'/'pages.txt').write_text('new version',encoding='utf-8');tool('always','fs_read',{**pages,'offset':80},failed=True)
        revoke=tool('always','fs_read',{'workspace_id':'work','path':'sample.txt','max_bytes':32},failed=True);owner('always','approve',revoke['approval_id']);owner('always','revoke',revoke['approval_id']);tool('always','fs_read',{'workspace_id':'work','path':'sample.txt','max_bytes':32,'approval_id':revoke['approval_id']},failed=True)
        absolute={'path':str(outside/'outside.txt')};v=tool('account','fs_read',absolute,failed=True);owner('account','approve',v['approval_id']);v=tool('account','fs_read',{**absolute,'approval_id':v['approval_id']});check(v['text']=='account body','account file adapter approved body')
        tool('never','fs_read',absolute,failed=True)
        tool('always','fs_read',{'workspace_id':'work','path':'sample.txt','approved':True},failed=True)
        check(owner('always','list')['approvals'],'owner lists shared approval records')
        quotaargs={'workspace_id':'work','path':'sample.txt'};expired=tool('quota','fs_read',quotaargs,failed=True);time.sleep(1.15)
        check(owner('quota','approve',expired['approval_id']).get('error'),'owner cannot approve an expired operation')
        value=tool('quota','fs_read',{**quotaargs,'approval_id':expired['approval_id']},failed=True);check(value['status']=='expired' and 'text' not in value,'expired approval releases no content')
        fresh=tool('quota','fs_read',quotaargs,failed=True);check(fresh['approval_id']!=expired['approval_id'],'expired operation may request a fresh approval')
        owner('quota','deny',fresh['approval_id']);tool('quota','fs_read',{**quotaargs,'max_bytes':16},failed=True)
        exhausted=tool('quota','fs_read',{**quotaargs,'max_bytes':8},failed=True);check('budget' in exhausted['error'],'approval resource quota fails before file content')
        tool('quota','host_capabilities')
        # All remaining logical file operations wait for the same local owner.
        pixel=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+X2ioAAAAASUVORK5CYII=')
        (folder/'always'/'pixel.png').write_bytes(pixel)
        operations=[('fs_list',{'workspace_id':'work','max_entries':2}),('fs_search',{'workspace_id':'work','query':'alpha','mode':'content'}),('image_view',{'workspace_id':'work','path':'pixel.png'}),
            ('fs_patch',{'workspace_id':'work','path':'new.txt','search':'changed','replacement':'patched','expected_sha256':hashlib.sha256(b'changed').hexdigest()})]
        for name,args in operations:
            value=tool('always',name,args,failed=True);check(value['status']=='pending','logical '+name+' waits without exposing data')
            owner('always','approve',value['approval_id']);value=tool('always',name,{**args,'approval_id':value['approval_id']})
            check('error' not in value,'approved '+name+' returns actual result')
        # A valid image larger than the bounded receipt cache must still retry.
        body=b'comment\0'+b'x'*600000;chunk=b'tEXt'+body
        large=pixel[:-12]+len(body).to_bytes(4,'big')+chunk+zlib.crc32(chunk).to_bytes(4,'big')+pixel[-12:]
        (folder/'always'/'large.png').write_bytes(large)
        imageargs={'workspace_id':'work','path':'large.png'};pending=tool('always','image_view',imageargs,failed=True)
        owner('always','approve',pending['approval_id']);imageargs['approval_id']=pending['approval_id']
        for _ in range(2):
            response=rpc('always','tools/call',imageargs,'image_view')['result']
            check(not response['isError'] and base64.b64decode(response['content'][1]['data'])==large,'large image retry reproduces exactly the approved version')
        check(owner('always','inspect',pending['approval_id'])['consumed_units']==2097152,'large image retry does not consume the permit twice')
        check(owner('always','revoke',pending['approval_id'])['status']=='denied','owner can revoke a completed read permit before a retry')
        response=rpc('always','tools/call',imageargs,'image_view')['result']
        check(response['isError'] and all(item['type']!='image' for item in response['content']),'revoked large image permit releases no cached or reread content')
        actorargs={'workspace_id':'work','path':'actor.txt','text':'actor content'}
        pending=http_tool('always','fs_write',actorargs,True);check(pending['actor']=='project-mcp','gateway actor trusted binding')
        owner('always','approve',pending['approval_id']);actorargs['approval_id']=pending['approval_id']
        tool('always','fs_write',actorargs,failed=True);http_tool('always','fs_write',actorargs,False)
        maximum={'workspace_id':'work','path':'maximum.txt','text':'\n'*65536}
        pending=http_tool('always','fs_write',maximum,True);owner('always','approve',pending['approval_id']);maximum['approval_id']=pending['approval_id']
        http_tool('always','fs_write',maximum,False);check((folder/'always'/'maximum.txt').read_bytes()==b'\n'*65536,'HTTP approval accepts the full 64 KiB text limit')
        for project in ['restricted','account']:
            task=approved_start(project,['identity'],'native-identity');state=await_task(project,task);check(state['exit_code']==0,'native identity task completed')
            actual=json.loads(task_output(project,task));check(actual['appcontainer']==(project=='restricted') and not actual['elevated'],'actual token matches identity')
            check(not actual['project_tokens_inherited'] and not actual['token_inherited'],'task has no gateway credentials')
        # The same native operations distinguish account trust from OS isolation.
        private=folder/'restricted-data'/'lease.lock'
        # Existing private audit record; do not assume a lock-file spelling.
        private=next((folder/'restricted-data').glob('audit-*.jsonl'))
        pipe='\\\\.\\pipe\\ShellBot-owner-'+hashlib.sha256(('\\\\?\\'+str(data)).encode('utf-16le')).hexdigest()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));listener.listen(4)
            probe_args=['sandbox',str(outside/'outside.txt'),str(restrictedro/'readonly.txt'),str(private),pipe,str(process.pid),str(listener.getsockname()[1])]
            for project in ['restricted','account']:
                task=approved_start(project,probe_args,'native-boundaries');state=await_task(project,task);check(state['exit_code']==0,'native security task returns evidence')
                actual=json.loads(task_output(project,task));report[project+'_native_boundaries']=actual
                for field in ['outside_read','readonly_write','private_read','change_acl','owner_pipe','process_injection','registry_read','network','children']:
                    check(actual[field]==(project=='account'),project+' '+field+' enforces real identity')
                check(actual['win32k_disabled']==(project=='restricted'),'Win32k system calls disabled only in sandbox')
            junction=restrictedroot/'outside-junction'
            linked=subprocess.run(['cmd.exe','/d','/c','mklink','/J',str(junction),str(outside)],capture_output=True,creationflags=flags)
            check(linked.returncode==0,'isolated junction fixture created without UAC')
            tool('restricted','fs_read',{'workspace_id':'work','path':'outside-junction/outside.txt'},failed=True)
            task=approved_start('restricted',['sandbox',str(junction/'outside.txt'),*probe_args[2:]],'native-junction-boundary')
            check(await_task('restricted',task)['exit_code']==0,'native junction probe completes')
            check(not json.loads(task_output('restricted',task))['outside_read'],'OS sandbox denies an outside target reached through a new junction')
        task=approved_start('restricted',['stdin'],'approved-input')
        inputargs={'task_id':task['task_id'],'host_instance_id':task['host_instance_id'],'input_key':'once','text':'approved stdin','eof':True}
        pending=tool('restricted','task_input',inputargs,failed=True);check(pending['status']=='pending','interactive input waits independently')
        owner('restricted','approve',pending['approval_id']);inputargs['approval_id']=pending['approval_id']
        tool('restricted','task_input',{**inputargs,'text':'changed'},failed=True)
        tool('restricted','task_input',inputargs);tool('restricted','task_input',inputargs)
        check(await_task('restricted',task)['exit_code']==0 and task_output('restricted',task)==b'approved stdin','approved text/EOF delivered once')
        task=approved_start('restricted',['append'],'restricted-write');check(await_task('restricted',task)['exit_code']==0,'restricted task writes permitted workspace')
        check((restrictedroot/'once.txt').read_bytes()==b'ONE','native restricted task writes actual directory')
        # Manager stops by Job even during a running task; cleanup includes new files.
        task=approved_start('restricted',['wait'],'restricted-stop');stopped=control('--project-stop','restricted');check(stopped['project']['state']=='stopped','stopped worker completes sandbox recovery')
        profile_lines=[json.loads(line) for path in (folder/'restricted-data').glob('sandbox-*.jsonl') for line in path.read_text().splitlines()]
        sid=next(line['sid'] for line in profile_lines if 'sid' in line)
        for path in [restrictedroot,restrictedroot/'allowed.txt',restrictedroot/'once.txt',restrictedro,fixture]:
            result=subprocess.run(['icacls',str(path)],capture_output=True,text=True,encoding='utf-8',errors='replace',creationflags=flags)
            check(result.returncode==0 and sid not in result.stdout,'sandbox SID removed from '+path.name)
        for path,original in acl_before.items():check(sddl(path)==original,'original owner/group/ACL preserved after recovery: '+Path(path).name)
        control('--project-start','restricted');check(not tool('restricted','host_capabilities')['execution_status']['controlled_isolation_verified'],'activation refuses an existing junction without opening command execution')
        control('--project-stop','restricted')
        # Remove only the isolated junction object, never its target tree.
        check(junction.parent==restrictedroot and os.lstat(junction).st_reparse_tag==0xA0000003,'cleanup target is the named test junction');os.rmdir(junction)
        control('--project-start','restricted');check(tool('restricted','host_capabilities')['execution_status']['controlled_isolation_verified'],'explicit restart revalidates a fresh sandbox')
        control('--manager-stop');process.wait(timeout=10)
        with registry.open('r+b'):check(True,'manager exit releases the configuration pin')
        for path in folder.rglob('*'):
            if path.is_file():
                retained=path.read_bytes();check(all(token.encode() not in retained for token in tokens.values()),'retained artifact excludes bearer values: '+path.name)
        report['passed']=True
    except Exception as exc:
        report['failure']=repr(exc);raise
    finally:
        if process and process.poll() is None:
            try:control('--manager-stop')
            except Exception:process.terminate()
            process.wait(timeout=10)
        report['checks_count']=len(report['checks']);out=Path(a.report);out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps({k:report[k] for k in ['passed','checks_count','artifacts']}))
if __name__=='__main__':main()
