#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IR Hub</title>
<style>
:root{--bg:#14171c;--card:#1d222a;--line:#2b323d;--fg:#e6e9ee;--mut:#8b94a3;--acc:#4da3ff;--ok:#3ecf8e;--bad:#ff6b6b}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
header{padding:14px 16px;border-bottom:1px solid var(--line);display:flex;gap:12px;align-items:baseline;flex-wrap:wrap}
h1{font-size:18px;margin:0}
#st{color:var(--mut);font-size:13px}
main{max-width:760px;margin:0 auto;padding:12px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
h2{font-size:15px;margin:0 0 10px}
input,button{font:inherit;color:var(--fg);background:#10131800;border:1px solid var(--line);border-radius:6px;padding:7px 10px}
input{background:#101318;min-width:0}
button{background:#262d38;cursor:pointer}
button:hover{border-color:var(--acc)}
button.pri{background:var(--acc);color:#001526;border-color:var(--acc)}
button.del{color:var(--bad)}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.cmd{display:flex;gap:8px;align-items:center;justify-content:space-between;padding:8px 0;border-top:1px solid var(--line);flex-wrap:wrap}
.cmd:first-child{border-top:0}
.nm{font-weight:600}.sm{color:var(--mut);font-size:12px}
.ch button{min-width:34px;padding:6px 8px}
label{display:block;color:var(--mut);font-size:12px;margin:8px 0 3px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
#toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#000c;padding:8px 14px;border-radius:8px;display:none}
</style></head><body>
<header><h1>IR Hub</h1><span id="st">loading…</span></header>
<main>

<section>
<h2>Learn a command</h2>
<div class="row">
<input id="lname" placeholder="name, e.g. TV_POWER" maxlength="40" style="flex:1">
<button class="pri" id="lbtn">Learn</button>
<button id="lcancel">Cancel</button>
</div>
<div class="sm" id="lstat" style="margin-top:8px">Press Learn, then point the original remote at the receiver and press a button.</div>
</section>

<section>
<h2>Commands</h2>
<div class="row" style="margin-bottom:6px">
<span class="sm">Repeat</span><input id="rep" type="number" min="0" max="20" value="0" style="width:64px">
<span class="sm">Tap a channel number to send on that output.</span>
</div>
<div id="list"></div>
<div class="row" style="margin-top:10px">
<button id="exp">Export</button>
<button id="imp">Import</button>
<input id="impf" type="file" accept=".json" style="display:none">
</div>
</section>

<section>
<h2>Settings</h2>
<div class="grid">
<div><label>Wi-Fi SSID</label><input id="c_ssid"></div>
<div><label>Wi-Fi password (blank = keep)</label><input id="c_pass" type="password"></div>
<div><label>MQTT host</label><input id="c_mh"></div>
<div><label>MQTT port</label><input id="c_mp" type="number"></div>
<div><label>MQTT user</label><input id="c_mu"></div>
<div><label>MQTT password (blank = keep)</label><input id="c_mw" type="password"></div>
<div><label>MQTT topic prefix</label><input id="c_pre"></div>
<div><label>MQTT enabled</label><input id="c_me" type="checkbox" style="width:20px;height:20px"></div>
</div>
<div class="row" style="margin-top:12px"><button class="pri" id="save">Save &amp; reboot</button></div>
</section>

</main>
<div id="toast"></div>
<script>
const $=s=>document.querySelector(s);
const NCH=4;
function toast(m){const t=$('#toast');t.textContent=m;t.style.display='block';clearTimeout(toast.h);toast.h=setTimeout(()=>t.style.display='none',2200)}
async function api(p,o){const r=await fetch(p,o);let j={};try{j=await r.json()}catch(e){}return j}
function post(p,b){return api(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})})}

async function status(){
  const s=await api('/api/status');
  $('#st').textContent=(s.ap?'SETUP AP ':'')+s.ip+' · MQTT '+s.mqtt+' · TCP '+s.tcpPort+' · up '+s.uptime+'s';
}
async function refresh(){
  const c=await api('/api/commands');
  const l=$('#list');l.innerHTML='';
  const names=Object.keys(c).sort();
  if(!names.length){l.innerHTML='<div class="sm">No commands yet. Learn one above.</div>';return}
  for(const n of names){
    const info=c[n];
    const d=document.createElement('div');d.className='cmd';
    const i=document.createElement('div');
    i.innerHTML='<div class="nm"></div><div class="sm"></div>';
    i.children[0].textContent=n+(info.builtin?' · built-in':info.overridden?' · custom (overrides built-in)':'');
    i.children[1].textContent=info.summary;
    const b=document.createElement('div');b.className='row ch';
    for(let k=1;k<=NCH;k++){
      const x=document.createElement('button');x.textContent=k;x.title='Send on channel '+k;
      x.onclick=async()=>{const r=await post('/api/send',{ch:k,cmd:n,repeat:+$('#rep').value||0});toast(r.ok?'Sent '+n+' on ch '+k:(r.error||'error'))};
      b.appendChild(x);
    }
    if(!info.builtin||info.overridden){
      const del=document.createElement('button');del.textContent='✕';del.className='del';
      del.title=info.overridden?'Revert to built-in':'Delete';
      del.onclick=async()=>{if(confirm((info.overridden?'Revert ':'Delete ')+n+'?')){await api('/api/delete?name='+encodeURIComponent(n),{method:'POST'});refresh()}};
      b.appendChild(del);
    }
    d.appendChild(i);d.appendChild(b);l.appendChild(d);
  }
}
let poll=null;
function pollLearn(){
  clearInterval(poll);
  poll=setInterval(async()=>{
    const s=await api('/api/learn');
    if(s.active){$('#lstat').textContent='Waiting for a button press… ('+s.name+')';return}
    clearInterval(poll);
    $('#lstat').textContent=s.state==='learned'?'Learned '+s.name+'.':'Learn '+s.state+'.';
    if(s.state==='learned')refresh();
  },500);
}
$('#lbtn').onclick=async()=>{
  const r=await post('/api/learn',{name:$('#lname').value});
  if(!r.ok){toast(r.error||'error');return}
  $('#lstat').textContent='Waiting for a button press…';pollLearn();
};
$('#lcancel').onclick=async()=>{await post('/api/learn/cancel')};
$('#exp').onclick=()=>{location.href='/api/export'};
$('#imp').onclick=()=>$('#impf').click();
$('#impf').onchange=async e=>{
  const f=e.target.files[0];if(!f)return;
  const r=await api('/api/import',{method:'POST',headers:{'Content-Type':'application/json'},body:await f.text()});
  toast(r.ok?'Imported '+r.imported:(r.error||'error'));refresh();
};
async function loadCfg(){
  const c=await api('/api/config');
  $('#c_ssid').value=c.ssid||'';$('#c_mh').value=c.mqttHost||'';$('#c_mp').value=c.mqttPort||1883;
  $('#c_mu').value=c.mqttUser||'';$('#c_pre').value=c.mqttPrefix||'irhub';$('#c_me').checked=!!c.mqttEnabled;
}
$('#save').onclick=async()=>{
  const r=await post('/api/config',{ssid:$('#c_ssid').value,pass:$('#c_pass').value,mqttHost:$('#c_mh').value,
    mqttPort:+$('#c_mp').value||1883,mqttUser:$('#c_mu').value,mqttPass:$('#c_mw').value,
    mqttPrefix:$('#c_pre').value,mqttEnabled:$('#c_me').checked});
  toast(r.ok?'Saved. Rebooting…':(r.error||'error'));
};
status();refresh();loadCfg();setInterval(status,5000);
</script>
</body></html>)rawliteral";
