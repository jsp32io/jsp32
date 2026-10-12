'use strict';
const $ = selector => document.querySelector(selector);
const modeLabels = {ap: '仅 AP', sta: '仅 STA', apsta: 'APSTA'};
const stateLabels = {initializing: '初始化', ready: '热点已开启', connecting: '正在连接 Wi-Fi', online: 'Wi-Fi 已连接', applying: '正在应用', fallback: '已恢复 AP', error: '连接失败', stopping: '正在停止'};
const labels = {
  board:'核心板',target:'目标芯片',uptimeMs:'运行时间',resetReason:'复位原因',resetReasonCode:'复位原因代码',
  chip:'芯片',model:'型号',modelCode:'型号代码',cores:'核心数',revision:'芯片修订',cpuFrequencyMHz:'CPU 频率 (MHz)',
  features:'功能',wifi2_4GHz:'2.4 GHz Wi-Fi',bluetoothLE:'蓝牙 LE',bluetoothClassic:'经典蓝牙',embeddedFlash:'内置 Flash',embeddedPsram:'内置 PSRAM',
  flash:'Flash',psram:'PSRAM',size:'容量',configuredSize:'配置容量',mode:'模式',frequency:'频率',initialized:'已初始化',frequencyMHz:'频率 (MHz)',
  firmware:'固件',project:'项目',version:'版本',buildDate:'构建日期',buildTime:'构建时间',idfVersion:'ESP-IDF 版本',secureVersion:'安全版本',elfSha256:'ELF SHA-256',
  mac:'MAC 地址',station:'STA',softAp:'AP',bluetooth:'蓝牙',network:'网络',ip:'IP 地址',netmask:'子网掩码',gateway:'网关',hostname:'主机名',ssid:'网络名称',rssi:'信号强度 (dBm)',
  wifiMaxTxPowerQdbm:'最大发射功率 (¼ dBm)',wifiMaxTxPowerDbm:'最大发射功率 (dBm)',savedMode:'已保存模式',state:'状态',modeSaved:'已设置过模式',pending:'正在应用',scanning:'正在扫描',fallback:'已回退 AP',attemptId:'操作编号',disconnectReason:'断线原因代码',errorCode:'错误代码',error:'错误',enabled:'已启用',connected:'已连接',connectionEnabled:'连接已启用',open:'匿名开放',clients:'热点连接数',
  heap:'内存',free:'空闲',minimumFree:'历史最小空闲',internal:'内部内存',total:'总量',largestBlock:'最大空闲块',partitions:'分区',label:'名称',type:'类型',subtype:'子类型',address:'地址',encrypted:'加密'
};
let config = null, status = null, applying = null, loadingConfig = false, scanInFlight = false;
let infoInFlight = false, statusInFlight = false;
const selectedMode = () => $('input[name=mode]:checked').value;
const openAP = () => $('input[name=ap-auth]:checked').value === 'open';
const bytes = n => n >= 1048576 ? `${(n/1048576).toFixed(2)} MiB` : n >= 1024 ? `${(n/1024).toFixed(1)} KiB` : `${n} B`;
function uptime(ms) { const s=Math.floor(ms/1000); return `${Math.floor(s/86400)} 天 ${Math.floor(s/3600)%24} 时 ${Math.floor(s/60)%60} 分 ${s%60} 秒`; }
function node(tag, text, className) { const element=document.createElement(tag); if(text !== undefined) element.textContent=text; if(className) element.className=className; return element; }
function message(selector, text, error=false) { const element=$(selector); element.textContent=text; element.classList.toggle('error', error); }
function row(list, name, value) { const element=node('div', undefined, 'row'); element.append(node('dt',name),node('dd',value)); list.append(element); }
async function api(path, options={}) {
  const controller=new AbortController(), timeout=setTimeout(()=>controller.abort(),8000);
  try {
    const response=await fetch(path,{cache:'no-store',...options,signal:controller.signal});
    if(!response.ok) throw new Error((await response.text()).slice(0,300) || `HTTP ${response.status}`);
    return await response.json();
  } finally { clearTimeout(timeout); }
}
function displayValue(key,value,parent) {
  if(value === null) return '—';
  if(typeof value === 'boolean') return value ? '是' : '否';
  if(key === 'uptimeMs') return uptime(value);
  if(key === 'address') return `0x${value.toString(16).toUpperCase()}`;
  if(['size','configuredSize','free','minimumFree','total','largestBlock'].includes(key) && typeof value === 'number') return bytes(value);
  if((key === 'mode' && parent === 'network') || key === 'savedMode') return modeLabels[value] || value;
  if(key === 'state') return stateLabels[value] || value;
  return String(value);
}
function renderObject(container, object, parent='', path=parent) {
  const list=node('dl'); container.append(list);
  for(const [key,value] of Object.entries(object)) {
    if(value !== null && typeof value === 'object') {
      const details=node('details'); details.dataset.path=`${path}/${key}`; details.append(node('summary', labels[key] || key));
      if(Array.isArray(value)) value.forEach((item,index)=>{const group=node('details');group.dataset.path=`${path}/${key}/${index}`;group.append(node('summary',item.label || String(index+1)));renderObject(group,item,key,group.dataset.path);details.append(group);});
      else renderObject(details,value,key,details.dataset.path);
      container.append(details);
    } else row(list,labels[key] || key,displayValue(key,value,parent));
  }
  if(!list.childElementCount) list.remove();
}
function renderInfo(info) {
  const expanded=new Set([...$('#system-info').querySelectorAll('details[open]')].map(element=>element.dataset.path));
  $('#summary').replaceChildren();
  const entries=[['设备', info.board],['固件',info.firmware?.version],['运行时间',uptime(info.uptimeMs)],['空闲内存',bytes(info.heap?.free || 0)]];
  for(const [label,value] of entries){const card=node('div');card.append(node('span',label),node('strong',value || '—'));$('#summary').append(card);}
  const target=$('#system-info');target.replaceChildren();
  const device={};for(const key of ['board','target','uptimeMs','resetReason','resetReasonCode'])if(key in info)device[key]=info[key];
  const basics=node('section',undefined,'panel');basics.append(node('h2','设备'));renderObject(basics,device);target.append(basics);
  for(const [key,value] of Object.entries(info)) {
    if(key in device) continue;
    const panel=node('section',undefined,'panel');panel.append(node('h2',labels[key] || key));
    if(Array.isArray(value))value.forEach(item=>{const details=node('details');details.dataset.path=`${key}/${item.label}`;details.append(node('summary',item.label || '分区'));renderObject(details,item,key,details.dataset.path);panel.append(details);});
    else if(value && typeof value === 'object')renderObject(panel,value,key);
    else panel.append(node('p',displayValue(key,value,'')));
    target.append(panel);
  }
  for(const detail of target.querySelectorAll('details'))detail.open=expanded.has(detail.dataset.path);
}
async function refreshInfo() {
  if(infoInFlight) return;
  infoInFlight=true;$('#refresh-info').disabled=true;
  try{renderInfo(await api('/api/v1/system/info'));message('#overview-message','');$('#connection').textContent='已连接';}
  catch(e){message('#overview-message','暂时无法读取设备信息，请检查连接后刷新。',true);$('#connection').textContent='连接中断';}
  finally{infoInFlight=false;$('#refresh-info').disabled=false;}
}
function updateFields() {
  const mode=selectedMode();
  $('#ap-fields').hidden=mode === 'sta';$('#ap-fields').disabled=mode === 'sta';
  $('#sta-fields').hidden=mode === 'ap';$('#sta-fields').disabled=mode === 'ap';
  $('#ap-password-field').hidden=openAP();
  $('#sta-password-field').hidden=$('#sta-open').checked;
  const scanDisabled=!status || status.pending || status.scanning || status.state === 'connecting' || scanInFlight;
  $('#scan').disabled=scanDisabled;
  $('#scan').textContent=scanInFlight || status?.scanning ? '正在扫描…' : '扫描';
  $('#scan-hint').textContent=status?.state === 'connecting' ? '正在连接，完成后可扫描。' : '扫描不会改变已保存的模式。';
  $('#apply').disabled=!config || status?.pending || status?.scanning || Boolean(applying);
  $('#apply').textContent=applying || status?.pending ? '正在应用…' : '应用更改';
}
async function loadConfig() {
  if(loadingConfig)return;loadingConfig=true;
  try {
    config=await api('/api/v1/network/config');
    $(`input[name=mode][value=${config.mode}]`).checked=true;
    $('#ap-ssid').value=config.ap.ssid;$(`input[name=ap-auth][value=${config.ap.open?'open':'protected'}]`).checked=true;
    $('#ap-password').value='';$('#ap-password').placeholder=config.ap.passwordSet?'留空保留已保存密码':'';
    $('#ap-password-hint').textContent=config.ap.passwordSet?'8–63 个字符；留空保留原密码':'8–63 个字符';
    $('#sta-ssid').value=config.station.ssid;$('#sta-password').value='';$('#sta-open').checked=Boolean(config.station.ssid) && !config.station.passwordSet;
    $('#sta-password').placeholder=config.station.passwordSet?'留空保留已保存密码':'';
    $('#sta-password-hint').textContent=config.station.passwordSet?'留空保留原密码；更换网络时请输入新密码':'8–63 个字符，或 64 位十六进制 PSK';
    updateFields();
  } catch(e) {message('#network-message','无法读取网络设置，请检查连接后重试。',true);}
  finally{loadingConfig=false;}
}
function renderStatus(value) {
  status=value;$('#network-state').textContent=stateLabels[value.state] || value.state;
  const list=$('#network-status');list.replaceChildren();
  row(list,'实际运行模式',modeLabels[value.mode] || value.mode);
  row(list,'已保存模式',value.modeSaved ? modeLabels[value.savedMode] : '尚未设置 · 默认匿名 AP');
  if(value.softAp?.enabled)row(list,'设备热点',`${value.softAp.ssid || '—'} · ${value.softAp.open?'匿名':'密码保护'} · ${value.softAp.ip || '192.168.4.1'}`);
  if(value.station?.connectionEnabled)row(list,'STA',value.station.connected ? `${value.station.ssid} · ${value.station.ip}` : '未连接');
  if(value.fallback)message('#network-message','STA 连接或配置保存失败，已恢复 AP。已保存的配置保持不变，请检查设置后重新应用。',true);
  else if(value.state === 'error')message('#network-message',`网络连接失败（${value.error || value.errorCode}），热点仍可用于管理。`,true);
  if(applying && value.attemptId === applying.id && !value.pending) {
    const succeeded=!value.errorCode && !value.fallback;
    applying=null;
    if(succeeded)message('#network-message','更改已应用并保存。');
    else message('#network-message','更改失败，已恢复原 AP 设置；原配置未被覆盖。',true);
    loadConfig();
  }
  updateFields();
}
async function refreshStatus() {
  if(statusInFlight)return;statusInFlight=true;
  try{renderStatus(await api('/api/v1/network/status'));$('#connection').textContent='已连接';}
  catch(e){$('#connection').textContent='连接中断';if(!applying)message('#network-message','设备连接中断，请检查网络连接。',true);}
  finally{statusInFlight=false;}
}
$('#network-form').addEventListener('change',updateFields);
$('#scan').onclick=async()=>{
  if(scanInFlight)return;scanInFlight=true;updateFields();message('#network-message','正在扫描附近网络…');
  try {
    await api('/api/v1/network/scan',{method:'POST'});
    let done=false;
    for(let i=0;i<60;i++){await new Promise(resolve=>setTimeout(resolve,500));const value=await api('/api/v1/network/status');renderStatus(value);if(!value.scanning){if(value.errorCode)throw new Error(value.error || '扫描失败');done=true;break;}}
    if(!done)throw new Error('扫描超时');
    const result=await api('/api/v1/network/networks');$('#networks').replaceChildren();
    for(const network of result.networks){const button=node('button',undefined,'network-choice');button.type='button';button.append(node('span',network.ssid),node('small',`${network.rssi} dBm · ${network.open?'开放':'加密'}`));button.onclick=()=>{$('#sta-ssid').value=network.ssid;$('#sta-password').value='';$('#sta-open').checked=network.open;updateFields();if(selectedMode()==='ap')message('#network-message',`已选择“${network.ssid}”。选择仅 STA 或 APSTA 后可连接此网络。`);};$('#networks').append(button);}
    message('#network-message',result.networks.length?'选择一个网络，或手动输入名称。':'未发现网络，请手动输入名称。');
  } catch(e){message('#network-message','扫描失败，请稍后重试。',true);}
  finally{scanInFlight=false;updateFields();}
};
$('#network-form').onsubmit=async event=>{
  event.preventDefault();if(!config || applying || status?.pending)return;
  const mode=selectedMode(),body={mode},encoder=new TextEncoder();
  if(mode !== 'sta') {
    const ssid=$('#ap-ssid').value,password=$('#ap-password').value;
    if(encoder.encode(ssid).length>32 || !ssid){message('#network-message','热点名称需要为 1–32 字节。',true);return;}
    body.ap={ssid,open:openAP()};
    if(!openAP()){
      if(password && (encoder.encode(password).length<8 || encoder.encode(password).length>63)){message('#network-message','热点密码需要为 8–63 字节。',true);return;}
      if(!password && !config.ap.passwordSet){message('#network-message','请输入热点密码。',true);return;}
      if(password)body.ap.password=password;
    }
  }
  if(mode !== 'ap') {
    const ssid=$('#sta-ssid').value,password=$('#sta-password').value;
    if(!ssid || encoder.encode(ssid).length>32){message('#network-message','Wi-Fi 名称需要为 1–32 字节。',true);return;}
    body.station={ssid};
    if($('#sta-open').checked)body.station.password='';
    else if(password){if(!((encoder.encode(password).length>=8 && encoder.encode(password).length<=63) || /^[a-fA-F0-9]{64}$/.test(password))){message('#network-message','Wi-Fi 密码格式无效。',true);return;}body.station.password=password;}
    else if(ssid !== config.station.ssid || !config.station.passwordSet){message('#network-message','请输入 Wi-Fi 密码；开放网络请选择“无需密码”。',true);return;}
  }
  let instructions='更改已提交，正在应用。页面可能暂时断开。';
  if(mode === 'sta')instructions+='\n连接成功后，通过局域网中的设备 STA 地址访问。失败时，请重新连接原设备热点并打开 http://192.168.4.1/。';
  else instructions+=`\n若连接中断，请连接热点“${body.ap.ssid}”，打开 http://192.168.4.1/。应用失败时恢复原热点“${config.ap.ssid}”。`;
  if(!window.confirm(`应用 ${modeLabels[mode]}？\n${mode === 'sta'?'设备热点将关闭。':'修改热点配置可能断开当前连接。'}\n重启后继续使用此模式。`))return;
  applying={id:null};updateFields();message('#network-message',instructions);
  try {
    const result=await api('/api/v1/network/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    applying={id:result.attemptId};$('#ap-password').value='';$('#sta-password').value='';
    await refreshStatus();
  } catch(e){applying=null;message('#network-message',`无法确认更改结果：${e.message}\n${instructions}`,true);}
  finally{updateFields();}
};
const pluginState={items:[],selected:null,detail:null,generation:0,loading:false,saving:false,rebooting:false,telemetryView:null};
const pluginFile=id=>`/api/v1/fs/file?path=${encodeURIComponent(`/plugins/${id}/config.json`)}`;
const pluginTelemetryFile=id=>`/api/v1/fs/file?path=${encodeURIComponent(`/plugins/${id}/telemetry.json`)}`;
const pluginMetaFile='/api/v1/fs/file?path=%2Fplugins%2Fmeta.json';
function pluginMessage(text,error=false){message('#plugins-message',text,error);}
async function pluginEval(source,operation='执行插件调用'){
  const expression=source.length>512?source.slice(0,512)+'…（表达式已截断）':source;
  let result;
  try{
    result=await api('/api/v1/quickjs/eval',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({source,mode:'expr',await:true})});
  }catch(error){throw new Error(`${operation}失败\n接口：POST /api/v1/quickjs/eval\n错误：${error.message}\n表达式：${expression}`);}
  if(!result?.ok || result.truncated){
    const reason=result?.error?`${result.error.name || 'Error'}: ${result.error.message || '执行失败'}`:
      result?.truncated?'返回结果超过大小限制':'返回结果无效或执行失败';
    const stack=result?.error?.stack?`\n堆栈：\n${result.error.stack.slice(0,2000)}`:'';
    throw new Error(`${operation}失败\n错误：${reason}\n表达式：${expression}${stack}`);
  }
  return result.value;
}
async function readPluginFile(url){
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),8000);
  try{
    const response=await fetch(url,{cache:'no-store',signal:controller.signal});
    if(response.status===404)return {value:{},etag:null};
    if(!response.ok)throw new Error(`读取文件失败：HTTP ${response.status}`);
    const value=JSON.parse(await response.text());
    if(!value || typeof value!=='object' || Array.isArray(value))throw new Error('配置文件必须是 JSON 对象');
    return {value,etag:response.headers.get('ETag')};
  }catch(error){throw new Error(`读取 ${decodeURIComponent(url)} 失败：${error.name || 'Error'}: ${error.message}`);
  }finally{clearTimeout(timeout);}
}
async function writePluginFile(url,value,etag){
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),8000);
  try{
    const response=await fetch(url,{method:'PUT',cache:'no-store',signal:controller.signal,
      headers:{'Content-Type':'application/json',[etag?'If-Match':'If-None-Match']:etag || '*'},
      body:JSON.stringify(value,null,2)+'\n'});
    if(!response.ok)throw new Error(response.status===412?'文件已被其他客户端修改，请刷新后重试':`保存失败：HTTP ${response.status}`);
  }finally{clearTimeout(timeout);}
}
async function deletePluginFile(url,etag){
  if(!etag)return;
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),8000);
  try{
    const response=await fetch(url,{method:'DELETE',cache:'no-store',signal:controller.signal,headers:{'If-Match':etag}});
    if(!response.ok && response.status!==404)throw new Error(response.status===412?'文件已被其他客户端修改，请刷新后重试':`恢复默认失败：HTTP ${response.status}`);
  }finally{clearTimeout(timeout);}
}
function renderPluginList(){
  const target=$('#plugin-list');target.replaceChildren();
  if(!pluginState.items.length){target.append(node('p','未发现插件。','muted'));return;}
  for(const item of pluginState.items){
    const button=node('button',undefined,'plugin-choice');button.type='button';
    button.setAttribute('aria-pressed',String(item.id===pluginState.selected));
    button.append(node('strong',item.id),node('span',item.status));
    button.onclick=()=>selectPlugin(item.id);target.append(button);
  }
}
function pluginField(key,rule,value){
  const label=node('label',undefined,'field'),caption=node('span',undefined,'field-caption');
  caption.append(node('span',rule.name || key),node('small',key,'muted'));
  let input;
  if(rule.choices){
    input=node('select');rule.choices.forEach((choice,index)=>{const option=node('option',String(choice));option.value=String(index);input.append(option);});
    const index=rule.choices.findIndex(choice=>choice===value);input.value=String(index<0?0:index);
  }else if(rule.type==='boolean'){
    input=node('input');input.type='checkbox';input.checked=Boolean(value);
  }else{
    input=node('input');input.type=rule.type==='string'?'text':'number';
    if(rule.type!=='string')input.step=rule.type==='integer'?'1':'any';
    if(rule.min!==undefined)input.min=String(rule.min);
    if(rule.max!==undefined)input.max=String(rule.max);
    input.value=value===undefined?'':String(value);
    input.required=true;
  }
  input.name=key;
  if(input.type==='checkbox'){label.className='field field-check';label.append(input,caption);}
  else label.append(caption,input);
  return {label,input};
}
function readPluginFields(schema,fields){
  const values={};
  for(const [key,rule] of Object.entries(schema)){
    const input=fields.get(key);let value;
    if(rule.choices)value=rule.choices[Number(input.value)];
    else if(rule.type==='boolean')value=input.checked;
    else if(rule.type==='string')value=input.value;
    else{
      if(input.value.trim()==='')throw new Error(`${rule.name || key}不能为空`);
      value=Number(input.value);
    }
    if(rule.type==='integer'?!Number.isSafeInteger(value):rule.type==='number'?!Number.isFinite(value):false)
      throw new Error(`${rule.name || key}的数字格式无效`);
    if((rule.min!==undefined && value<rule.min)||(rule.max!==undefined && value>rule.max))
      throw new Error(`${rule.name || key}超出允许范围`);
    if(rule.choices && !rule.choices.includes(value))throw new Error(`${rule.name || key}选项无效`);
    values[key]=value;
  }
  return values;
}
function readTelemetryFields(controls,rows){
  const number=(input,name)=>{
    const value=input.value.trim()===''?NaN:Number(input.value);
    if(!Number.isFinite(value))throw new Error(`${name}必须是有效数字`);
    return value;
  };
  const timing=(input,name)=>{
    const value=number(input,name);
    if(!Number.isInteger(value) || value<10 || value>86400000)throw new Error(`${name}必须为 10～86400000 毫秒的整数`);
    return value;
  };
  const values={enabled:controls.enabled.checked,topic:controls.topic.value.trim(),mode:controls.mode.value,
    qos:Number(controls.qos.value),retain:controls.retain.checked,fields:{}};
  const names=new Set();
  if(values.topic && (new TextEncoder().encode(values.topic).length>256 || /[+#\u0000]/.test(values.topic)))
    throw new Error('Topic 最多 256 字节，不能包含 +、# 或空字符');
  if(![0,1,2].includes(values.qos))throw new Error('QoS 无效');
  if(values.mode==='interval')values.intervalMs=timing(controls.intervalMs,'发送间隔');
  else if(values.mode==='change'){
    values.minIntervalMs=timing(controls.minIntervalMs,'最短发送间隔');
    values.heartbeatMs=timing(controls.heartbeatMs,'心跳间隔');
    if(values.heartbeatMs<values.minIntervalMs)throw new Error('心跳间隔不能小于最短发送间隔');
  }else throw new Error('发送模式无效');
  for(const row of rows){
    const name=row.key.value.trim(),address=row.address.value.trim().toUpperCase(),type=row.type.value;
    if(!/^[A-Za-z][A-Za-z0-9_]{0,62}$/.test(name))throw new Error('字段名需由字母开头，只含字母、数字和下划线，最多 63 字符');
    if(names.has(name))throw new Error(`字段名重复：${name}`);names.add(name);
    if(!/^[XYMD](0|[1-9]\d*)$/.test(address) || Number(address.slice(1))>65535)
      throw new Error(`${name} 的寄存器地址无效`);
    if(!['boolean','uint16','int16','uint32','int32','float32'].includes(type))throw new Error(`${name} 的类型无效`);
    if((type==='boolean')!==(address[0]!=='D'))throw new Error(`${name}：位类型使用 X/Y/M，数值类型使用 D`);
    const field={address,type,trigger:row.trigger.checked};
    const displayName=row.name.value.trim();
    if(displayName){
      if(new TextEncoder().encode(displayName).length>128 || /\u0000/.test(displayName))throw new Error(`${name} 的显示名最多 128 字节，不能包含空字符`);
      field.name=displayName;
    }
    if(type!=='boolean'){
      field.scale=number(row.scale,`${name} 倍率`);field.offset=number(row.offset,`${name} 偏移`);
      if(type.endsWith('32')){
        field.wordOrder=row.wordOrder.value;
        if(!['highFirst','lowFirst'].includes(field.wordOrder))throw new Error(`${name} 的字序无效`);
      }
    }
    values.fields[name]=field;
  }
  const count=Object.keys(values.fields).length;
  if(count>64)throw new Error('最多上报 64 个字段');
  return values;
}
function telemetryValuePlan(row,info){
  const address=row.address.value.trim().toUpperCase(),type=row.type.value;
  const match=/^([XYMD])(0|[1-9]\d*)$/.exec(address);
  if(!match || !['boolean','uint16','int16','uint32','int32','float32'].includes(type))throw new Error('寄存器地址或类型无效');
  if((type==='boolean')!==(match[1]!=='D'))throw new Error('位类型使用 X/Y/M，数值类型使用 D');
  const count=type.endsWith('32')?2:1,index=Number(match[2]);
  if(index+count>info.areas[match[1]])throw new Error('寄存器地址超出设备容量');
  if(count>info.maxTransaction)throw new Error('字段超过设备读取事务上限');
  const wordOrder=row.wordOrder.value;
  if(count===2 && !['highFirst','lowFirst'].includes(wordOrder))throw new Error('字序无效');
  const scale=Number(row.scale.value),offset=Number(row.offset.value);
  if(type!=='boolean' && (!row.scale.value.trim() || !row.offset.value.trim() || !Number.isFinite(scale) || !Number.isFinite(offset)))
    throw new Error('倍率或偏移无效');
  return {row,type,wordOrder,scale,offset,addresses:Array.from({length:count},(_,i)=>`${match[1]}${index+i}`),
    signature:telemetryValueSignature(row)};
}
function telemetryValueSignature(row){
  return [row.address.value,row.type.value,row.wordOrder.value,row.scale.value,row.offset.value].join('\0');
}
function decodeTelemetryValue(plan,values){
  const words=plan.addresses.map(address=>values.get(address));
  if(plan.type==='boolean'){
    if(typeof words[0]!=='boolean')throw new Error('无效的位寄存器响应');
    return words[0];
  }
  if(words.some(word=>!Number.isInteger(word) || word<0 || word>65535))throw new Error('无效的数据寄存器响应');
  let value=words[0];
  if(plan.type==='int16' && value>=0x8000)value-=65536;
  if(plan.addresses.length===2){
    const bits=plan.wordOrder==='lowFirst'?words[1]*65536+words[0]:words[0]*65536+words[1];
    if(plan.type==='uint32')value=bits;
    else if(plan.type==='int32')value=bits>=0x80000000?bits-4294967296:bits;
    else {const data=new DataView(new ArrayBuffer(4));data.setUint32(0,bits);value=data.getFloat32(0);}
  }
  value=value*plan.scale+plan.offset;
  return Number.isFinite(value)?value:null;
}
function stopTelemetryValues(){
  const view=pluginState.telemetryView;
  if(view?.timer)clearTimeout(view.timer);
  pluginState.telemetryView=null;
}
function telemetryValueVisible(view){
  return pluginState.telemetryView===view && pluginState.detail===view.detail &&
    location.pathname==='/plugins' && !document.hidden && !pluginState.rebooting;
}
async function refreshTelemetryValues(view){
  if(!telemetryValueVisible(view) || view.busy)return;
  view.busy=true;
  try{
    if(!view.info){
      const info=await api('/api/v1/registers');
      if(!telemetryValueVisible(view))return;
      if(!info.areas || !['X','Y','M','D'].every(area=>Number.isInteger(info.areas[area]) && info.areas[area]>0) ||
          !Number.isInteger(info.maxTransaction) || info.maxTransaction<1)throw new Error('寄存器容量响应无效');
      view.info=info;
    }
    const batches=[];let batch={plans:[],addresses:new Set()};
    for(const row of [...view.rows]){
      let plan;
      try{plan=telemetryValuePlan(row,view.info);}catch(error){row.live.textContent='配置无效';row.live.title=error.message;continue;}
      const combined=new Set([...batch.addresses,...plan.addresses]);
      if(combined.size>view.info.maxTransaction){batches.push(batch);batch={plans:[],addresses:new Set(plan.addresses)};}
      else batch.addresses=combined;
      batch.plans.push(plan);
    }
    if(batch.plans.length)batches.push(batch);
    for(const batch of batches){
      const result=await api('/api/v1/registers/read',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({addresses:[...batch.addresses]})});
      if(!telemetryValueVisible(view))return;
      if(!result.ok || !Array.isArray(result.values))throw new Error('寄存器读取响应无效');
      const values=new Map(result.values.map(item=>[item.address,item.value]));
      for(const plan of batch.plans){
        if(!view.rows.includes(plan.row))continue;
        if(telemetryValueSignature(plan.row)!==plan.signature){plan.row.live.textContent='—';plan.row.live.title='字段设置已改变，等待刷新';continue;}
        const value=decodeTelemetryValue(plan,values);
        plan.row.live.textContent=typeof value==='number'?String(Number(value.toPrecision(12))):String(value);
        plan.row.live.title='当前寄存器值，按此行设置解码';
      }
    }
    view.status.textContent='实时值每秒刷新，按当前表单中的字段设置解码。';
  }catch(error){
    if(telemetryValueVisible(view)){
      for(const row of view.rows){row.live.textContent='—';row.live.title='实时值读取失败';}
      view.status.textContent=`读取实时值失败：${error.message}`;
    }
  }finally{view.busy=false;}
}
function startTelemetryValues(detail){
  stopTelemetryValues();
  if(!detail?.telemetryRows || pluginState.detail!==detail || location.pathname!=='/plugins' || document.hidden)return;
  const view={detail,rows:detail.telemetryRows,status:detail.telemetryValueStatus,info:null,busy:false,timer:null};
  pluginState.telemetryView=view;
  const poll=async()=>{
    if(!telemetryValueVisible(view))return;
    await refreshTelemetryValues(view);
    if(telemetryValueVisible(view))view.timer=setTimeout(poll,1000);
  };
  poll();
}
function renderTelemetryForm(detail,target){
  if(!detail.telemetry)return;
  const defaults=detail.telemetry.defaults;
  const values=detail.telemetryFile.etag?detail.telemetryFile.value:defaults;
  const form=node('form',undefined,'plugin-form telemetry-form'),controls={},rows=[];
  form.append(node('h3','遥测数据'),node('p','表格中的字段都会上报；删除字段后不再上报。保存后重启设备生效。','muted'));
  const control=(key,label,type,value,choices)=>{
    const field=node('label',undefined,'field'),caption=node('span',label);
    const input=node(choices?'select':'input');
    if(choices)for(const [key,text] of choices){const option=node('option',text);option.value=String(key);input.append(option);}
    else input.type=type;
    if(type==='checkbox')input.checked=value;else input.value=String(value);
    if(type==='number'){input.min='10';input.max='86400000';input.step='1';}
    controls[key]=input;
    if(type==='checkbox'){field.className='field field-check';field.append(input,caption);}
    else field.append(caption,input);
    form.append(field);return field;
  };
  control('enabled','启用此插件的数据上报','checkbox',values.enabled ?? true);
  control('topic','Topic（留空使用设备默认路径）','text',values.topic ?? '');
  controls.topic.placeholder=`/jsp32/<设备ID>/${detail.item.id}`;
  control('mode','发送模式','text',values.mode ?? 'interval',[['interval','定时发送'],['change','变化发送']]);
  const interval=control('intervalMs','发送间隔（毫秒）','number',values.intervalMs ?? 1000);
  const minimum=control('minIntervalMs','最短发送间隔（毫秒）','number',values.minIntervalMs ?? 200);
  const heartbeat=control('heartbeatMs','心跳间隔（毫秒）','number',values.heartbeatMs ?? 10000);
  const modeChanged=()=>{const change=controls.mode.value==='change';interval.hidden=change;minimum.hidden=heartbeat.hidden=!change;
    for(const row of rows)row.trigger.disabled=!change;};
  controls.mode.onchange=modeChanged;modeChanged();
  control('qos','QoS','text',values.qos ?? 0,[[0,'0'],[1,'1'],[2,'2']]);
  control('retain','保留最后一条消息','checkbox',values.retain ?? false);
  const wrap=node('div',undefined,'register-table-wrap'),table=node('table',undefined,'telemetry-table'),head=node('tr'),body=node('tbody');
  form.append(node('p','“变化触发”仅用于变化上报；未勾选时，该字段仍随其他字段触发或状态同步一起发送。','muted'));
  for(const label of ['变化触发','显示名','JSON 字段键','寄存器','类型','字序','倍率','偏移','实时值','操作'])head.append(node('th',label));
  const columns=node('colgroup');
  for(const key of ['trigger','name','key','address','type','word-order','scale','offset','value','actions']){
    const column=node('col');column.className=`telemetry-col-${key}`;columns.append(column);
  }
  const thead=node('thead');thead.append(head);table.append(columns,thead,body);wrap.append(table);form.append(wrap);
  const addRow=(name,definition)=>{
    const row={};row.element=node('tr');
    for(const [key,value,choices] of [['trigger',definition.trigger ?? true],['name',definition.name ?? ''],['key',name],['address',definition.address ?? 'D0'],
      ['type',definition.type ?? 'uint16',['boolean','uint16','int16','uint32','int32','float32']],
      ['wordOrder',definition.wordOrder ?? 'highFirst',['highFirst','lowFirst']],['scale',definition.scale ?? 1],['offset',definition.offset ?? 0]]){
      const cell=node('td'),input=node(choices?'select':'input');
      if(choices)for(const choice of choices){const option=node('option',choice);option.value=choice;input.append(option);}
      else input.type=key==='trigger'?'checkbox':['scale','offset'].includes(key)?'number':'text';
      if(key==='trigger')input.checked=value;else input.value=String(value);
      if(key==='name')input.placeholder=name;
      if(['scale','offset'].includes(key))input.step='any';
      input.setAttribute('aria-label',`${name || '新字段'} ${key}`);row[key]=input;cell.append(input);row.element.append(cell);
    }
    const update=()=>{const bit=row.type.value==='boolean';row.scale.disabled=row.offset.disabled=bit;row.wordOrder.disabled=!row.type.value.endsWith('32');
      row.trigger.disabled=controls.mode.value!=='change';};
    row.type.onchange=update;update();
    row.live=node('td','—','telemetry-live');row.element.append(row.live);
    const cell=node('td'),remove=node('button','删除');remove.type='button';remove.onclick=()=>{rows.splice(rows.indexOf(row),1);row.element.remove();};
    cell.append(remove);row.element.append(cell);
    rows.push(row);body.append(row.element);
  };
  const selected=values.fields ?? {};
  for(const [name,definition] of Object.entries(selected))addRow(name,definition);
  const add=node('button','添加字段');add.type='button';add.onclick=()=>{addRow('',{});rows.at(-1).key.focus();};
  form.append(add);
  detail.telemetryRows=rows;detail.telemetryValueStatus=node('p','正在读取实时值…','muted');form.append(detail.telemetryValueStatus);
  const actions=node('div',undefined,'actions'),save=node('button','保存配置','primary'),reset=node('button','恢复插件默认');
  save.type='submit';reset.type='button';actions.append(save,reset);form.append(actions);
  const persist=async (settings,restore=false)=>{
    if(pluginState.saving)return;pluginState.saving=true;save.disabled=reset.disabled=true;
    try{
      await pluginEval(`(await import("plugins")).plugins.validateTelemetry(${JSON.stringify(detail.item.id)},${JSON.stringify(settings)})`,
        `插件 ${detail.item.id}：校验上报配置（plugins.validateTelemetry）`);
      if(restore)await deletePluginFile(pluginTelemetryFile(detail.item.id),detail.telemetryFile.etag);
      else await writePluginFile(pluginTelemetryFile(detail.item.id),settings,detail.telemetryFile.etag);
      detail.telemetryFile=await readPluginFile(pluginTelemetryFile(detail.item.id));
      detail.telemetry.values=null;renderPluginDetail();pluginMessage('上报设置已保存，重启设备后生效。');
    }catch(error){pluginMessage(`无法保存上报设置：${error.message}`,true);}
    finally{pluginState.saving=false;save.disabled=reset.disabled=false;}
  };
  form.onsubmit=event=>{event.preventDefault();try{return persist(readTelemetryFields(controls,rows));}catch(error){pluginMessage(error.message,true);}};
  reset.onclick=()=>persist(defaults,true);target.append(form);startTelemetryValues(detail);
}
function renderPluginDetail(){
  stopTelemetryValues();
  const target=$('#plugin-detail');target.replaceChildren();
  const detail=pluginState.detail;if(!detail)return;
  const {item,config,file,meta}=detail;
  const heading=node('div',undefined,'plugin-detail-heading');
  heading.append(node('h2',item.id),node('p',`状态：${item.status}${item.error?` · ${item.error}`:''}`,
    `plugin-status${item.error || item.status?.endsWith('failed')?' error':''}`));
  const enabled=node('label',undefined,'check plugin-enabled');
  const toggle=node('input');toggle.type='checkbox';toggle.checked=!(meta.value[item.id]?.disabled ?? item.disabled);
  enabled.append(toggle,node('span','启用插件'));
  let savedEnabled=toggle.checked;
  toggle.onchange=async()=>{
    if(pluginState.saving){toggle.checked=savedEnabled;return;}
    const nextEnabled=toggle.checked;
    pluginState.saving=true;toggle.disabled=true;
    try{
      const latest=await readPluginFile(pluginMetaFile);
      const next={...latest.value,[item.id]:{...latest.value[item.id],disabled:!nextEnabled}};
      await writePluginFile(pluginMetaFile,next,latest.etag);
      savedEnabled=nextEnabled;
      detail.meta=await readPluginFile(pluginMetaFile);
      pluginMessage('启用状态已保存，重启设备后生效。');
    }catch(error){toggle.checked=savedEnabled;pluginMessage(error.message,true);}
    finally{pluginState.saving=false;toggle.disabled=false;}
  };
  heading.append(enabled);target.append(heading);
  if(item.deps?.length)target.append(node('p',`依赖：${item.deps.join('、')}`,'muted'));
  const form=node('form');form.className='plugin-form';
  const fields=new Map();let rawEditor;
  if(config.schema){
    const values={...config.values,...file.value};
    for(const [key,rule] of Object.entries(config.schema)){
      const {label,input}=pluginField(key,rule,values[key]===undefined?rule.default:values[key]);
      fields.set(key,input);form.append(label);
    }
    if(!fields.size)form.append(node('p','此插件没有可配置项。','muted'));
  }else{
    form.append(node('p','当前无法读取配置声明，可编辑原始 JSON。','muted'));
    rawEditor=node('textarea');rawEditor.value=JSON.stringify(file.value,null,2);rawEditor.rows=10;
    rawEditor.setAttribute('aria-label','插件配置 JSON');form.append(rawEditor);
  }
  const actions=node('div',undefined,'actions'),save=node('button','保存配置','primary');
  save.type='submit';actions.append(save);form.append(actions);
  form.onsubmit=async event=>{
    event.preventDefault();if(pluginState.saving)return;
    let values;
    try{
      values=config.schema?readPluginFields(config.schema,fields):JSON.parse(rawEditor.value);
      if(!values || typeof values!=='object' || Array.isArray(values))throw new Error('配置必须是 JSON 对象');
    }catch(error){pluginMessage(error.message,true);return;}
    pluginState.saving=true;save.disabled=true;
    try{
      await writePluginFile(pluginFile(item.id),values,detail.file.etag);
      detail.file=await readPluginFile(pluginFile(item.id));
      pluginMessage('配置已保存，重启设备后生效。');
    }catch(error){pluginMessage(error.message,true);}
    finally{pluginState.saving=false;save.disabled=false;}
  };
  target.append(form);
  renderTelemetryForm(detail,target);
}
async function selectPlugin(id){
  if(!/^[a-z][a-z0-9_-]{0,31}$/.test(id))return;
  stopTelemetryValues();
  pluginState.selected=id;pluginState.detail=null;renderPluginList();
  $('#plugin-detail').replaceChildren(node('p','正在读取插件设置…','muted'));
  const generation=++pluginState.generation;
  try{
    const item=pluginState.items.find(entry=>entry.id===id);
    const [config,file,meta,telemetry]=await Promise.all([
      pluginEval(`(await import("plugins")).plugins.getConfig(${JSON.stringify(id)})`,
        `插件 ${id}：读取配置声明（plugins.getConfig）`),
      readPluginFile(pluginFile(id)),readPluginFile(pluginMetaFile),
      pluginEval(`(await import("plugins")).plugins.getTelemetryConfig(${JSON.stringify(id)})`,
        `插件 ${id}：读取遥测设置（plugins.getTelemetryConfig）`)]);
    const telemetryFile=telemetry?await readPluginFile(pluginTelemetryFile(id)):null;
    if(generation!==pluginState.generation || location.pathname!=='/plugins')return;
    pluginState.detail={item,config:config || {schema:null,values:null},file,meta,telemetry,telemetryFile};
    renderPluginDetail();pluginMessage('');
  }catch(error){if(generation===pluginState.generation)pluginMessage(`无法读取插件 ${id} 设置：\n${error.message}`,true);}
}
async function loadPlugins(){
  if(pluginState.loading || pluginState.rebooting)return;pluginState.loading=true;$('#refresh-plugins').disabled=true;
  pluginMessage('正在读取插件…');
  try{
    const items=await pluginEval('(await import("plugins")).plugins.list()','读取插件列表（plugins.list）');
    if(!Array.isArray(items))throw new Error('插件列表格式无效');
    if(location.pathname!=='/plugins')return;
    pluginState.items=items;renderPluginList();
    const selected=items.some(item=>item.id===pluginState.selected)?pluginState.selected:items[0]?.id;
    if(selected)await selectPlugin(selected);
    else{$('#plugin-detail').replaceChildren(node('p','尚未安装插件。','muted'));pluginMessage('');}
    $('#connection').textContent='已连接';
  }catch(error){pluginMessage(`无法读取插件：${error.message}`,true);}
  finally{pluginState.loading=false;$('#refresh-plugins').disabled=false;}
}
$('#refresh-plugins').onclick=loadPlugins;
async function rebootDevice(button,showMessage,confirmation){
  if(pluginState.rebooting || pluginState.saving)return;
  if(!window.confirm(confirmation))return;
  button.disabled=true;pluginState.rebooting=true;
  try{
    const before=await api('/api/v1/system/info');
    const started=Date.now();
    await api('/api/v1/ota/reboot',{method:'POST'});
    showMessage('设备正在重启，等待重新连接…');$('#connection').textContent='正在重启';
    for(let attempt=0;attempt<40;attempt++){
      await new Promise(resolve=>setTimeout(resolve,750));
      try{
        const current=await api('/api/v1/system/info');
        if(current.uptimeMs+100<before.uptimeMs+Date.now()-started){location.reload();return;}
      }catch(error){/* 重启期间连接暂时不可用。 */}
    }
    showMessage('尚未确认设备重启完成，请稍后刷新页面。',true);
  }catch(error){showMessage(`无法确认重启请求：${error.message}`,true);}
  finally{pluginState.rebooting=false;button.disabled=false;}
}
$('#reboot-plugins').onclick=()=>rebootDevice($('#reboot-plugins'),pluginMessage,
  '现在重启设备并应用已保存的插件设置？当前连接会暂时断开。');
// End of plugin page code.
const mqttState={loaded:false,loading:false,saving:false,statusBusy:false,fields:{}};
function mqttMessage(text,error=false){message('#mqtt-message',text,error);}
function readMqttFields(){
  const fields=mqttState.fields;
  const url=fields.url.value.trim(),username=fields.username.value;
  if(url && (!/^mqtts?:\/\/[^/\s@?#]+(?:\/[^\s@?#]*)?$/.test(url) || url.length>256))
    throw new Error('地址格式无效，请使用 mqtt:// 或 mqtts://');
  if(fields.enabled.checked && !url)throw new Error('启用 MQTT 时必须填写地址');
  if(username.length>128 || fields.password.value.length>128)throw new Error('用户名或密码过长');
  const config={enabled:fields.enabled.checked,url,username};
  if(fields.clear.checked)config.password='';
  else if(fields.password.value)config.password=fields.password.value;
  return config;
}
async function refreshMqttStatus(){
  if(mqttState.statusBusy)return;mqttState.statusBusy=true;
  try{
    const result=await api('/api/v1/mqtt/status');
    if(location.pathname!=='/mqtt')return;
    const target=$('#mqtt-status');target.replaceChildren();
    const names={disabled:'未启用',connecting:'连接中',connected:'已连接',disconnected:'已断开',error:'连接失败'};
    target.append(node('p',`设备 ID：${result.deviceId}`,'muted'));
    target.append(node('p',`${names[result.state] || result.state} · 已入队 ${result.queued} · 入队失败 ${result.rejected}`));
    for(const report of result.reports)target.append(node('p',`${report.pluginId} → ${report.topic}（${report.mode==='change'?'变化发送':'定时发送'}，${report.fieldCount} 个字段）`,'muted'));
    $('#connection').textContent='已连接';
  }catch(error){mqttMessage(`无法读取 MQTT 状态：${error.message}`,true);}
  finally{mqttState.statusBusy=false;}
}
async function loadMqtt(){
  if(mqttState.loading || mqttState.saving)return;
  if(mqttState.loaded){refreshMqttStatus();return;}
  mqttState.loading=true;
  try{
    const config=await api('/api/v1/mqtt/config');
    const target=$('#mqtt-config');target.replaceChildren();
    const panel=node('fieldset',undefined,'panel');panel.append(node('legend','连接配置'));
    const fields={};
    for(const [key,label,type] of [['enabled','启用','checkbox'],['url','Broker 地址','text'],['username','用户名','text'],['password','密码','password'],['clear','清除已保存密码','checkbox']]){
      const field=node('label',undefined,type==='checkbox'?'check':'field'),input=node('input');input.type=type;
      if(type==='checkbox'){input.checked=key==='enabled' && config.enabled;field.append(input,node('span',label));}
      else{input.maxLength=key==='url'?256:128;input.value=config[key] || '';input.autocomplete=key==='password'?'new-password':'off';field.append(node('span',label),input);}
      if(key==='url')input.placeholder='mqtt://192.168.1.10:1883';
      if(key==='password')field.append(node('small',config.passwordSet?'已设置密码；留空保留原密码':'未设置密码；可留空'));
      fields[key]=input;panel.append(field);
    }
    mqttState.fields=fields;target.append(panel);
    mqttState.loaded=true;$('#save-mqtt').disabled=false;mqttMessage('');
    await refreshMqttStatus();
  }catch(error){mqttMessage(`无法读取 MQTT 配置：${error.message}`,true);}
  finally{mqttState.loading=false;}
}
$('#mqtt-form').onsubmit=async event=>{
  event.preventDefault();if(mqttState.saving || !mqttState.loaded)return;
  let config;try{config=readMqttFields();}catch(error){mqttMessage(error.message,true);return;}
  mqttState.saving=true;$('#save-mqtt').disabled=true;$('#reboot-mqtt').disabled=true;
  try{
    await api('/api/v1/mqtt/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(config)});
    mqttState.loaded=false; mqttState.saving=false;await loadMqtt();mqttMessage('配置已保存，重启设备后生效。');
  }catch(error){mqttMessage(`保存失败：${error.message}`,true);}
  finally{mqttState.saving=false;$('#save-mqtt').disabled=false;$('#reboot-mqtt').disabled=false;}
};
$('#refresh-mqtt').onclick=()=>{mqttState.loaded=false;loadMqtt();};
$('#reboot-mqtt').onclick=()=>rebootDevice($('#reboot-mqtt'),mqttMessage,
  '现在重启设备并应用已保存的 MQTT 设置？当前连接会暂时断开。');
// End of MQTT page code.
const registerPageSize=32;
const registerState={info:null,area:'X',start:0,values:new Map(),revisions:new Map(),generation:0,
  snapshotEpoch:0,stream:null,retry:null,retryDelay:1000,connected:false,synced:false,snapshotBusy:false,snapshotAgain:false};
const registerVisible=()=>location.pathname==='/registers' && !document.hidden;
function compareRevision(a,b) {
  const left=String(a).replace(/^0+(?=\d)/,''),right=String(b).replace(/^0+(?=\d)/,'');
  return left.length-right.length || (left<right?-1:left>right?1:0);
}
function registerRange() {
  const capacity=registerState.info?.areas?.[registerState.area] || 0;
  return {capacity,count:Math.min(registerPageSize,Math.max(0,capacity-registerState.start))};
}
function setRegisterStatus(text,error=false) { message('#registers-message',text,error); }
function renderRegisters() {
  const {capacity,count}=registerRange(),area=registerState.area,start=registerState.start;
  for(const button of $('#register-areas').querySelectorAll('button'))
    button.setAttribute('aria-pressed',String(button.dataset.area===area));
  $('#register-position').textContent=capacity?`${start}–${start+count-1} / ${capacity}`:'—';
  $('#register-prev').disabled=!capacity || start===0;
  $('#register-next').disabled=!capacity || start+count>=capacity;
  const body=$('#register-values');body.replaceChildren();
  for(let index=start;index<start+count;index++) {
    const address=`${area}${index}`,tr=node('tr');tr.dataset.address=address;
    tr.append(node('th',address));tr.firstChild.scope='row';
    const value=registerState.values.get(address),cell=node('td');
    if(value===undefined)cell.textContent='—';
    else if(area==='D')cell.textContent=String(value);
    else {cell.append(node('span',value?'1':'0'),node('span',value?'开':'关','register-bit-hint'));}
    tr.append(cell);body.append(tr);
  }
}
function mergeRegister(address,value,revision) {
  if(!/^(X|Y|M|D)(0|[1-9]\d*)$/.test(address) || !/^\d+$/.test(String(revision)))return;
  const {count}=registerRange(),index=Number(address.slice(1));
  if(address[0]!==registerState.area || index<registerState.start || index>=registerState.start+count)return;
  if(address[0]==='D' ? !Number.isInteger(value) || value<0 || value>65535 : typeof value!=='boolean')return;
  const old=registerState.revisions.get(address);
  if(old!==undefined && compareRevision(revision,old)<0)return;
  registerState.values.set(address,value);registerState.revisions.set(address,String(revision));
  const row=$('#register-values').querySelector(`[data-address="${address}"]`);
  if(row){const cell=row.lastElementChild;cell.replaceChildren();
    if(address[0]==='D')cell.textContent=String(value);
    else cell.append(node('span',value?'1':'0'),node('span',value?'开':'关','register-bit-hint'));}
}
function closeRegisterStream() {
  if(registerState.stream){registerState.stream.close();registerState.stream=null;}
  if(registerState.retry){clearTimeout(registerState.retry);registerState.retry=null;}
  registerState.connected=false;
}
function stopRegisters() {
  ++registerState.generation;closeRegisterStream();registerState.synced=false;
  ++registerState.snapshotEpoch;registerState.snapshotBusy=false;registerState.snapshotAgain=false;
}
async function refreshRegisterSnapshot(generation=registerState.generation) {
  if(!registerVisible() || generation!==registerState.generation || !registerState.info)return;
  if(registerState.snapshotBusy){registerState.snapshotAgain=true;return;}
  const epoch=registerState.snapshotEpoch;registerState.snapshotBusy=true;
  do {
    registerState.snapshotAgain=false;registerState.synced=false;
    setRegisterStatus('正在同步寄存器…');
    const {count}=registerRange(),area=registerState.area,start=registerState.start;
    try {
      for(let offset=0;offset<count;offset+=registerState.info.maxTransaction) {
        const chunkCount=Math.min(registerState.info.maxTransaction,count-offset);
        const result=await api('/api/v1/registers/read',{method:'POST',headers:{'Content-Type':'application/json'},
          body:JSON.stringify({start:`${area}${start+offset}`,count:chunkCount})});
        if(generation!==registerState.generation || epoch!==registerState.snapshotEpoch || !registerVisible())return;
        if(!Array.isArray(result.values) || result.values.length!==chunkCount || !/^\d+$/.test(String(result.revision)))
          throw new Error('无效的寄存器响应');
        for(let i=0;i<chunkCount;i++) {
          const entry=result.values[i];
          if(entry?.address!==`${area}${start+offset+i}` ||
             (area==='D' ? !Number.isInteger(entry.value) || entry.value<0 || entry.value>65535 : typeof entry.value!=='boolean'))
            throw new Error('无效的寄存器响应');
          mergeRegister(entry.address,entry.value,result.revision);
        }
      }
      if(generation!==registerState.generation || epoch!==registerState.snapshotEpoch || !registerVisible())return;
      registerState.synced=true;
      setRegisterStatus(registerState.connected?'寄存器实时更新':'已读取快照；实时连接不可用，数值可能已过期');
      $('#connection').textContent='已连接';
    } catch(e) {
      if(generation!==registerState.generation || epoch!==registerState.snapshotEpoch || !registerVisible())return;
      setRegisterStatus('寄存器读取失败，显示的数值可能已过期；请刷新重试。',true);
    }
  } while(registerState.snapshotAgain && generation===registerState.generation && epoch===registerState.snapshotEpoch && registerVisible());
  if(epoch===registerState.snapshotEpoch)registerState.snapshotBusy=false;
}
function openRegisterStream(generation=registerState.generation) {
  if(!registerVisible() || generation!==registerState.generation || !registerState.info)return;
  const {count}=registerRange();if(!count)return;
  closeRegisterStream();registerState.synced=false;
  setRegisterStatus('正在连接寄存器订阅…');
  const source=new EventSource(`/api/v1/registers/events?start=${registerState.area}${registerState.start}&count=${count}`);
  registerState.stream=source;
  source.onopen=()=>{
    if(generation!==registerState.generation || registerState.stream!==source)return;
    registerState.connected=true;registerState.retryDelay=1000;
    ++registerState.snapshotEpoch;registerState.snapshotBusy=false;registerState.snapshotAgain=false;
    registerState.values.clear();registerState.revisions.clear();renderRegisters();
    refreshRegisterSnapshot(generation);
  };
  source.addEventListener('change',event=>{
    if(generation!==registerState.generation || registerState.stream!==source)return;
    try {const change=JSON.parse(event.data);mergeRegister(change.a,change.v,change.r);} catch(e) {refreshRegisterSnapshot(generation);}
  });
  source.addEventListener('dropped',()=>{
    if(generation!==registerState.generation || registerState.stream!==source)return;
    registerState.synced=false;refreshRegisterSnapshot(generation);
  });
  source.onerror=()=>{
    if(generation!==registerState.generation || registerState.stream!==source)return;
    const hasValues=registerState.values.size>0;
    closeRegisterStream();registerState.synced=false;
    setRegisterStatus(hasValues?'订阅连接中断，显示的数值可能已过期；正在重连…':'实时连接不可用；正在重连…',true);
    if(!hasValues)refreshRegisterSnapshot(generation);
    const delay=registerState.retryDelay;registerState.retryDelay=Math.min(delay*2,30000);
    registerState.retry=setTimeout(()=>{registerState.retry=null;openRegisterStream(generation);},delay);
  };
}
function showRegisterPage(start=registerState.start) {
  stopRegisters();registerState.start=start;registerState.values.clear();registerState.revisions.clear();
  renderRegisters();$('#register-values').closest('.register-table-wrap').scrollTop=0;
  if(registerVisible())openRegisterStream();
}
async function startRegisters() {
  stopRegisters();const generation=registerState.generation;
  registerState.values.clear();registerState.revisions.clear();renderRegisters();
  setRegisterStatus('正在读取寄存器信息…');
  try {
    const info=await api('/api/v1/registers');
    if(generation!==registerState.generation || location.pathname!=='/registers')return;
    if(!info.areas || !['X','Y','M','D'].every(area=>Number.isInteger(info.areas[area]) && info.areas[area]>0) ||
       !Number.isInteger(info.maxTransaction) || info.maxTransaction<1)throw new Error('无效的寄存器信息');
    registerState.info=info;
    const capacity=info.areas[registerState.area];
    if(registerState.start>=capacity)registerState.start=Math.floor((capacity-1)/registerPageSize)*registerPageSize;
    renderRegisters();
    if(registerVisible())openRegisterStream(generation);
    else setRegisterStatus('页面位于后台，恢复显示后读取寄存器。');
  } catch(e) {
    if(generation!==registerState.generation || location.pathname!=='/registers')return;
    registerState.info=null;renderRegisters();
    setRegisterStatus('寄存器接口不可用；请检查设备连接或配置后刷新。',true);
  }
}
$('#refresh-registers').onclick=()=>registerState.info?refreshRegisterSnapshot():startRegisters();
$('#register-areas').addEventListener('click',event=>{
  const area=event.target.closest('button[data-area]')?.dataset.area;
  if(!area || !registerState.info || area===registerState.area)return;
  registerState.area=area;message('#register-jump-error','');showRegisterPage(0);
});
$('#register-prev').onclick=()=>showRegisterPage(registerState.start-registerPageSize);
$('#register-next').onclick=()=>showRegisterPage(registerState.start+registerPageSize);
$('#register-jump').onsubmit=event=>{
  event.preventDefault();const address=$('#register-address').value.trim();
  const match=/^([XYMD])(0|[1-9]\d*)$/.exec(address);
  if(!match || !registerState.info || Number(match[2])>=registerState.info.areas[match[1]]) {
    message('#register-jump-error','请输入设备容量范围内的地址，例如 D100。',true);return;
  }
  message('#register-jump-error','');registerState.area=match[1];
  showRegisterPage(Math.floor(Number(match[2])/registerPageSize)*registerPageSize);
  $('#register-values').querySelector(`[data-address="${address}"]`)?.scrollIntoView({block:'nearest'});
};
document.addEventListener('visibilitychange',()=>{
  if(location.pathname==='/plugins'){
    if(document.hidden)stopTelemetryValues();else startTelemetryValues(pluginState.detail);
    return;
  }
  if(location.pathname!=='/registers')return;
  if(document.hidden){stopRegisters();setRegisterStatus('页面位于后台，显示的数值可能已过期。');}
  else if(registerState.info)openRegisterStream();else startRegisters();
});
function navigate() {
  const network=location.pathname === '/network',registers=location.pathname === '/registers',plugins=location.pathname === '/plugins',mqtt=location.pathname === '/mqtt';
  $('#overview-page').hidden=network || registers || plugins || mqtt;$('#network-page').hidden=!network;
  $('#registers-page').hidden=!registers;$('#plugins-page').hidden=!plugins;$('#mqtt-page').hidden=!mqtt;
  for(const tab of ['#overview-tab','#network-tab','#registers-tab','#plugins-tab','#mqtt-tab'])$(tab).removeAttribute('aria-current');
  $(mqtt?'#mqtt-tab':plugins?'#plugins-tab':registers?'#registers-tab':network?'#network-tab':'#overview-tab').setAttribute('aria-current','page');
  setSidebarOpen(false);
  if(!plugins)stopTelemetryValues();
  if(registers)startRegisters();
  else {stopRegisters();if(mqtt)loadMqtt();else if(plugins)loadPlugins();else if(network){if(!config)loadConfig();refreshStatus();}else refreshInfo();}
}
const sidebarMedia=window.matchMedia('(max-width:720px)');
function setSidebarOpen(open) {
  const wasOpen=document.body.classList.contains('sidebar-open');
  const expanded=Boolean(open && sidebarMedia.matches);
  document.body.classList.toggle('sidebar-open',expanded);
  $('#menu-toggle').setAttribute('aria-expanded',String(expanded));
  $('#menu-toggle').setAttribute('aria-label',expanded?'关闭导航菜单':'打开导航菜单');
  $('#sidebar-backdrop').hidden=!expanded;
  $('#sidebar').inert=sidebarMedia.matches && !expanded;
  $('main').inert=expanded;
  if(expanded)$('#sidebar').querySelector('a[aria-current]').focus();
  else if(wasOpen && sidebarMedia.matches)$('#menu-toggle').focus();
}
$('#menu-toggle').onclick=()=>setSidebarOpen(!document.body.classList.contains('sidebar-open'));
$('#sidebar-backdrop').onclick=()=>setSidebarOpen(false);
sidebarMedia.addEventListener('change',()=>setSidebarOpen(false));
window.addEventListener('keydown',event=>{
  if(!document.body.classList.contains('sidebar-open'))return;
  if(event.key==='Escape'){event.preventDefault();setSidebarOpen(false);return;}
  if(event.key==='Tab'){
    const links=$('#sidebar').querySelectorAll('a'),first=links[0],last=links[links.length-1];
    if(event.shiftKey && document.activeElement===first){event.preventDefault();last.focus();}
    else if(!event.shiftKey && document.activeElement===last){event.preventDefault();first.focus();}
  }
});
$('#refresh-info').onclick=refreshInfo;
document.querySelectorAll('header .brand, nav a').forEach(link=>{
  link.addEventListener('click',event=>{
    if(event.defaultPrevented || event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey)return;
    const destination=new URL(link.href,location.href);
    if(destination.origin !== location.origin || !['/','/network','/registers','/plugins','/mqtt'].includes(destination.pathname))return;
    event.preventDefault();
    if(location.pathname !== destination.pathname || location.search !== destination.search)
      history.pushState(null,'',destination.pathname+destination.search);
    navigate();
  });
});
window.addEventListener('popstate',navigate);
navigate();
setInterval(()=>{if(document.hidden)return;if(location.pathname === '/network')refreshStatus();else if(location.pathname === '/mqtt')refreshMqttStatus();else if(location.pathname === '/')refreshInfo();},5000);
