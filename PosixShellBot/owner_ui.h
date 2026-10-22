#ifndef SB_OWNER_UI_H
#define SB_OWNER_UI_H
/* Credentials are held only in this page's memory, never in links or storage. */
static const char sb_owner_html[]=
"<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ShellBot 所有者审批</title><link rel='stylesheet' href='/owner.css'><body><main><h1>ShellBot 所有者审批</h1>"
"<p>核对项目、执行身份、路径、完整参数和摘要后再决定。打开本页不会批准请求；重启中断的操作须先核对实际结果。</p>"
"<form id='connect'><label>所有者凭据 <input id='token' type='password' autocomplete='off' required minlength='32' maxlength='256'></label>"
"<button>连接</button><button id='forget' type='button'>清除凭据</button></form>"
"<p id='message' role='status'></p><label>项目 <select id='project'></select></label>"
"<label>执行身份 <select id='backend'><option value='ordinary'>普通</option><option value='administrator'>管理员</option></select></label>"
"<button id='refresh'>刷新请求</button><button id='history'>查看历史</button><button id='more' hidden>下一页历史</button>"
"<ul id='requests'></ul><h2>请求详情</h2><pre id='details'></pre>"
"<button id='approve' disabled>批准本次请求</button><button id='deny' disabled>拒绝本次请求</button><button id='revoke' disabled>撤销批准</button>"
"</main><script src='/owner.js' defer></script></body></html>";
static const char sb_owner_css[]=
"body{margin:0;background:#f5f7fa;color:#172331;font:16px/1.6 'Segoe UI','Microsoft YaHei',sans-serif}main{max-width:1100px;margin:auto;padding:24px}"
"h1{margin:0 0 16px}p{max-width:100%;overflow-wrap:anywhere}label{display:inline-block;margin:4px 12px 4px 0}input,select,button{font:inherit;border:1px solid #bac4d0;border-radius:5px;padding:6px 10px;background:white;color:inherit}"
"button{cursor:pointer;margin:4px}button:disabled{opacity:.5;cursor:default}button:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #6fa4e5}#token{width:min(280px,70vw)}"
"#requests{padding-left:20px;max-height:230px;overflow:auto}#requests button{white-space:normal;text-align:left;overflow-wrap:anywhere;max-width:100%}"
"#details{white-space:pre-wrap;overflow-wrap:anywhere;max-height:380px;overflow:auto;background:white;border:1px solid #d8dfe8;padding:16px;border-radius:5px;font:14px/1.5 Consolas,monospace}#message{min-height:26px;color:#35577e}";
static const char sb_owner_js[]=
"'use strict';(()=>{const el=id=>document.getElementById(id);let token='',selected=null,offset=0;"
"const fragment=new URLSearchParams(location.hash.slice(1));history.replaceState(null,'',location.pathname);"
"const project=el('project'),backend=el('backend');backend.value=fragment.get('backend')==='administrator'?'administrator':'ordinary';"
"function reset(){selected=null;el('details').textContent='';for(const id of ['approve','deny','revoke'])el(id).disabled=true;}"
"function message(text){el('message').textContent=text;}"
"async function call(command,extra={}){if(!token)throw Error('请先输入独立所有者凭据');"
"const body=command==='projects'?{command}:{command,project_id:project.value,backend:backend.value,...extra};"
"const response=await fetch('/owner/api',{method:'POST',headers:{Authorization:'Bearer '+token,'Content-Type':'application/json',Accept:'application/json'},body:JSON.stringify(body),cache:'no-store',credentials:'omit',redirect:'error'});"
"const result=await response.json();if(!response.ok||result.error)throw Error(result.error||'请求失败：'+response.status);return result;}"
"async function inspect(id,historical=false){reset();const value=await call(historical?'history':'inspect',{id});"
"const item=historical?value.records?.[0]:value;el('details').textContent=JSON.stringify(item||value,null,2);"
"if(item&&!historical){selected={project:project.value,backend:backend.value,item};el('approve').disabled=item.status!=='pending';el('deny').disabled=item.status!=='pending';el('revoke').disabled=!['pending','approved','completed'].includes(item.status);}message(historical?'历史记录仅供核对；不会重新执行。':'请核对完整参数与摘要后作出决定。');}"
"async function list(historical=false,next=false){reset();el('requests').replaceChildren();if(!next)offset=0;"
"const value=await call(historical?'history':'list',historical?{offset,limit:32}:{});const items=historical?value.records:value.approvals;"
"offset=value.next_offset||0;el('more').hidden=!historical||!value.has_more;"
"for(const item of items||[]){const row=document.createElement('li'),button=document.createElement('button');"
"const id=item.approval_id||item.task_id;button.textContent=[item.record_kind||item.tool||'请求',item.status,id].join(' · ');"
"button.onclick=()=>run(()=>inspect(id,historical));row.append(button);el('requests').append(row);}message((items||[]).length+' 条记录');}"
"async function decide(command){if(!selected||selected.project!==project.value||selected.backend!==backend.value)throw Error('请重新选择并核对请求');"
"const item=selected.item;await call(command,{id:item.approval_id,expected_sha256:item.operation_sha256||item.sha256,expected_host_instance_id:item.host_instance_id,expected_policy_revision:item.policy_revision});"
"await inspect(item.approval_id);message('已记录'+({approve:'批准',deny:'拒绝',revoke:'撤销'}[command])+'；客户端可查询或重试原请求。');}"
"async function run(action){try{await action();}catch(error){message(error.message);}}"
"el('connect').onsubmit=event=>{event.preventDefault();run(async()=>{token=el('token').value;el('token').value='';reset();"
"const value=await call('projects');project.replaceChildren();for(const item of value.projects||[]){const option=document.createElement('option');option.value=item.project_id;option.textContent=item.project_id+'（'+item.state+'）';project.append(option);}"
"if([...project.options].some(o=>o.value===fragment.get('project')))project.value=fragment.get('project');await list();if(fragment.get('approval'))await inspect(fragment.get('approval'));});};"
"el('forget').onclick=()=>{token='';el('token').value='';reset();project.replaceChildren();el('requests').replaceChildren();message('凭据已清除');};"
"project.onchange=backend.onchange=()=>{reset();el('requests').replaceChildren();};el('refresh').onclick=()=>run(()=>list());"
"el('history').onclick=()=>run(()=>list(true));el('more').onclick=()=>run(()=>list(true,true));"
"for(const id of ['approve','deny','revoke'])el(id).onclick=()=>run(()=>decide(id));})();";
#endif
