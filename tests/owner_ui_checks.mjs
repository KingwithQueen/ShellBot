// Local Edge acceptance with an isolated profile; credentials arrive on stdin.
import {spawn} from 'node:child_process';
import {readFile,writeFile} from 'node:fs/promises';
import {join} from 'node:path';
let input='';for await(const part of process.stdin)input+=part;
const config=JSON.parse(input), checks=[];
const browser=spawn(config.browser,['--headless=new','--remote-debugging-port=0','--remote-debugging-address=127.0.0.1',
  '--no-first-run','--disable-background-networking','--disable-sync','--disable-extensions','--user-data-dir='+config.profile,'about:blank'],{windowsHide:true,stdio:'ignore'});
let ws;
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function check(ok,name){checks.push({name,passed:!!ok});if(!ok)throw Error(name);}
try{
  let port;for(let i=0;i<150;i++){try{port=Number((await readFile(join(config.profile,'DevToolsActivePort'),'utf8')).split('\n')[0]);break;}catch{}await delay(100);}
  check(port>0,'isolated browser starts');
  const target=await(await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(config.url)}`,{method:'PUT'})).json();
  ws=new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
  let sequence=0;const pending=new Map();
  ws.onmessage=event=>{const message=JSON.parse(event.data);if(message.id&&pending.has(message.id)){const {resolve,reject}=pending.get(message.id);pending.delete(message.id);message.error?reject(Error(message.error.message)):resolve(message.result);}};
  const send=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));});
  const evaluate=async expression=>{const r=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error('page evaluation failed');return r.result.value;};
  const until=async(expression)=>{for(let i=0;i<120;i++){if(await evaluate(expression))return true;await delay(100);}return false;};
  check(await until("!!document.getElementById('connect')"),'actual owner controls render');
  check(await until("location.hash==='' && typeof document.getElementById('connect').onsubmit==='function'"),'page removes fragment from browser history');
  await evaluate(`document.getElementById('token').value=${JSON.stringify(config.token)};document.getElementById('connect').requestSubmit();`);
  check(await until("document.getElementById('project').options.length===2 && document.getElementById('details').textContent.includes('operation_sha256')"),'owner connects and selected request details become visible');
  const details=await evaluate("JSON.parse(document.getElementById('details').textContent)");
  check(details.approval_id===config.id && details.operation_sha256===config.digest && details.status==='pending','visible details bind exact pending request');
  check(await evaluate("document.getElementById('token').value==='' && document.querySelectorAll('img,svg').length===0"),'credential field clears and untrusted markup renders as text');
  check(await evaluate("!document.getElementById('approve').disabled && !document.getElementById('deny').disabled"),'pending decision controls are clickable');
  await send('Page.enable');await send('Emulation.setDeviceMetricsOverride',{width:1280,height:960,deviceScaleFactor:1,mobile:false});
  const screenshot=await send('Page.captureScreenshot',{format:'png'});await writeFile(config.screenshot,Buffer.from(screenshot.data,'base64'));
  await evaluate("document.getElementById('approve').click()");
  check(await until("document.getElementById('details').textContent.includes('\"status\": \"approved\"')"),'actual owner button records approval');
  await evaluate("document.getElementById('forget').click()");
  check(await evaluate("document.getElementById('project').options.length===0 && document.getElementById('approve').disabled && document.getElementById('details').textContent===''"),'forget clears visible state and disables decisions');
  await send('Browser.close');console.log(JSON.stringify({passed:true,checks,screenshot:config.screenshot}));
}catch(error){console.log(JSON.stringify({passed:false,checks,error:error.message}));process.exitCode=1;}
finally{if(ws?.readyState===WebSocket.OPEN){ws.send(JSON.stringify({id:999999,method:'Browser.close'}));await delay(300);ws.close();}
  if(browser.exitCode===null){browser.kill();await delay(200);}}
