'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const {test}=require('node:test');
const source=fs.readFileSync(path.resolve(__dirname,'../../components/app_ui/assets/app.js'),'utf8');
const pluginCode=source.slice(source.indexOf('const pluginState='),source.indexOf('// End of plugin page code.'));

function browser(responses){
  const requests=[],elements=new Map();
  const context={
    Map,JSON,Number,String,Promise,AbortController,encodeURIComponent,TextEncoder,
    setTimeout,clearTimeout,location:{pathname:'/plugins'},
    document:{},window:{confirm:()=>false},Date,
    $(selector){if(!elements.has(selector))elements.set(selector,{});return elements.get(selector);},
    fetch:async(url,options)=>{requests.push({url,options});return responses.shift();},
    api:async()=>({ok:true,value:[]}),
    message(){},node(){return {};}
  };
  vm.createContext(context);vm.runInContext(pluginCode,context);
  return {context,requests,elements};
}

test('plugin eval errors identify the operation, plugin, expression and remote stack',async()=>{
  const b=browser([]);
  b.context.api=async()=>({ok:false,error:{name:'TypeError',message:'not a function',stack:'at <eval>:1'}});
  const source='(await import("plugins")).plugins.getTelemetryConfig("io")';
  await assert.rejects(b.context.pluginEval(source,'插件 io：读取遥测设置（plugins.getTelemetryConfig）'),error=>{
    assert.match(error.message,/插件 io/);assert.match(error.message,/getTelemetryConfig/);
    assert.match(error.message,/TypeError: not a function/);assert.match(error.message,/at <eval>:1/);
    assert.ok(error.message.includes(source));return true;
  });
  b.context.api=async()=>{throw new Error('HTTP 503');};
  await assert.rejects(b.context.pluginEval(source,'插件 io：读取上报设置'),error=>{
    assert.match(error.message,/插件 io/);assert.match(error.message,/POST \/api\/v1\/quickjs\/eval/);
    assert.match(error.message,/HTTP 503/);return true;
  });
  b.context.api=async()=>({ok:true,truncated:true});
  await assert.rejects(b.context.pluginEval(source,'插件 io：读取上报设置'),/返回结果超过大小限制/);
});

test('plugin file errors include the decoded file path and HTTP status',async()=>{
  const b=browser([{ok:false,status:500}]);
  await assert.rejects(b.context.readPluginFile('/api/v1/fs/file?path=%2Fplugins%2Fio%2Ftelemetry.json'),error=>{
    assert.match(error.message,/\/plugins\/io\/telemetry.json/);
    assert.match(error.message,/HTTP 500/);return true;
  });
});

test('plugin heading saves enable changes immediately and rolls back rejected writes',async()=>{
  const b=browser([]),writes=[],messages=[];
  function element(tag,text,className){return {tag,textContent:text,className,children:[],
    append(...children){this.children.push(...children);},replaceChildren(){this.children=[];}};}
  const target=element('section');b.elements.set('#plugin-detail',target);b.context.node=element;
  const detail={item:{id:'io',status:'running'},config:{schema:{},values:{}},file:{value:{}},
    meta:{value:{io:{disabled:false}}}};
  b.context.activeDetail=detail;vm.runInContext('pluginState.detail=activeDetail',b.context);
  b.context.pluginMessage=(text,error)=>messages.push({text,error});
  let stored={io:{disabled:false,custom:'preserved'},other:{disabled:true}},reject=false;
  b.context.readPluginFile=async()=>({value:stored,etag:'current-etag'});
  b.context.writePluginFile=async(url,value,etag)=>{
    writes.push({url,value,etag});if(reject)throw new Error('write rejected');stored=value;
  };
  b.context.renderPluginDetail();
  const heading=target.children[0];
  assert.deepEqual(heading.children.map(child=>child.tag),['h2','p','label']);
  assert.equal(heading.children[1].textContent,'状态：running');
  const toggle=heading.children[2].children[0];assert.equal(toggle.checked,true);
  toggle.checked=false;const pending=toggle.onchange();assert.equal(toggle.disabled,true);await pending;
  assert.equal(writes[0].etag,'current-etag');assert.equal(stored.io.disabled,true);
  assert.equal(stored.io.custom,'preserved');assert.equal(stored.other.disabled,true);
  assert.equal(toggle.disabled,false);assert.match(messages.at(-1).text,/重启设备后生效/);
  reject=true;toggle.checked=true;await toggle.onchange();
  assert.equal(toggle.checked,false);assert.equal(toggle.disabled,false);
  assert.equal(messages.at(-1).error,true);assert.match(messages.at(-1).text,/write rejected/);
});

test('telemetry live values decode register types, word order, scale and offset',()=>{
  const {context}=browser([]),info={areas:{X:4,Y:4,M:4,D:2048},maxTransaction:256};
  const row=(address,type,scale='1',offset='0',wordOrder='highFirst')=>({address:{value:address},type:{value:type},
    scale:{value:scale},offset:{value:offset},wordOrder:{value:wordOrder}});
  const decode=(r,values)=>context.decodeTelemetryValue(context.telemetryValuePlan(r,info),new Map(values));
  assert.equal(decode(row('X0','boolean'),[['X0',true]]),true);
  assert.equal(decode(row('D0','int16'),[['D0',65535]]),-1);
  assert.equal(decode(row('D0','uint32'),[['D0',65535],['D1',65535]]),4294967295);
  assert.equal(decode(row('D0','int32','0.01'),[['D0',65535],['D1',65436]]),-1);
  assert.equal(decode(row('D0','int32','1','0','lowFirst'),[['D0',65436],['D1',65535]]),-100);
  assert.equal(decode(row('D0','float32','2','1'),[['D0',0x3fc0],['D1',0]]),4);
  assert.equal(decode(row('D0','float32'),[['D0',0x7f80],['D1',0]]),null);
  assert.throws(()=>context.telemetryValuePlan(row('D2047','uint32'),info),/容量/);
  assert.throws(()=>decode(row('D0','uint32'),[['D0',1]]),/响应/);
});

test('telemetry polling batches pairs, monitors unselected rows and preserves input edits',async()=>{
  const b=browser([]),requests=[];
  const row=(address,type)=>({enabled:{checked:false},address:{value:address},type:{value:type},
    scale:{value:'1'},offset:{value:'0'},wordOrder:{value:'highFirst'},live:{textContent:'old'}});
  const rows=[row('D0','int32'),row('X0','boolean'),row('D100','uint16')];
  const detail={},view={detail,rows,status:{},info:{areas:{X:4,Y:4,M:4,D:16},maxTransaction:2},busy:false};
  b.context.activeDetail=detail;b.context.activeView=view;
  vm.runInContext('pluginState.detail=activeDetail;pluginState.telemetryView=activeView',b.context);
  b.context.api=async(path,options)=>{
    requests.push({path,body:JSON.parse(options.body)});
    return {ok:true,values:requests.at(-1).body.addresses.map(address=>({address,value:address==='X0'?true:65535}))};
  };
  await b.context.refreshTelemetryValues(view);
  assert.deepEqual(requests[0].body.addresses,['D0','D1']);
  assert.deepEqual(requests[1].body.addresses,['X0']);
  assert.equal(rows[0].live.textContent,'-1');assert.equal(rows[1].live.textContent,'true');
  assert.equal(rows[2].live.textContent,'配置无效');assert.match(rows[2].live.title,/容量/);
  assert.equal(rows[0].address.value,'D0');assert.equal(rows[0].enabled.checked,false);
  b.context.stopTelemetryValues();await b.context.refreshTelemetryValues(view);assert.equal(requests.length,2);
});

test('telemetry polling ignores stale replies after switching plugin and clears failed values',async()=>{
  const b=browser([]),detail={},row={address:{value:'D0'},type:{value:'uint16'},scale:{value:'1'},offset:{value:'0'},
    wordOrder:{value:'highFirst'},live:{textContent:'old'}};
  const view={detail,rows:[row],status:{},info:{areas:{X:4,Y:4,M:4,D:16},maxTransaction:256},busy:false};
  b.context.activeDetail=detail;b.context.activeView=view;
  vm.runInContext('pluginState.detail=activeDetail;pluginState.telemetryView=activeView',b.context);
  let resolve;b.context.api=()=>new Promise(done=>{resolve=done;});
  const pending=b.context.refreshTelemetryValues(view);b.context.stopTelemetryValues();
  resolve({ok:true,values:[{address:'D0',value:123}]});await pending;
  assert.equal(row.live.textContent,'old');
  vm.runInContext('pluginState.telemetryView=activeView',b.context);
  b.context.api=async()=>{throw new Error('HTTP 503');};
  await b.context.refreshTelemetryValues(view);assert.equal(row.live.textContent,'—');assert.match(view.status.textContent,/HTTP 503/);
});

test('plugin numeric settings reject empty, fractional, and out-of-range values',()=>{
  const {context}=browser([]);
  const schema={count:{type:'integer',name:'脉冲数',min:1,max:100},ratio:{type:'number',min:0.1,max:10},
    reverse:{type:'boolean'},mode:{type:'string',choices:['up','down']}};
  const fields=new Map([['count',{value:'4'}],['ratio',{value:'1.5'}],
    ['reverse',{checked:true}],['mode',{value:'1'}]]);
  assert.deepEqual(JSON.parse(JSON.stringify(context.readPluginFields(schema,fields))),
    {count:4,ratio:1.5,reverse:true,mode:'down'});
  fields.get('count').value='';assert.throws(()=>context.readPluginFields(schema,fields),/不能为空/);
  fields.get('count').value='1.5';assert.throws(()=>context.readPluginFields(schema,fields),/格式无效/);
  fields.get('count').value='101';assert.throws(()=>context.readPluginFields(schema,fields),/范围/);
});

test('telemetry editor saves every remaining row and validates names, types and timings',()=>{
  const {context}=browser([]);
  const controls={enabled:{checked:true},topic:{value:''},mode:{value:'interval'},qos:{value:'0'},retain:{checked:false},
    intervalMs:{value:'1000'},minIntervalMs:{value:'200'},heartbeatMs:{value:'10000'}};
  const row=(key,address,type)=>({trigger:{checked:true},key:{value:key},name:{value:''},address:{value:address},
    type:{value:type},wordOrder:{value:'highFirst'},scale:{value:'0.01'},offset:{value:'0'}});
  const rows=[row('level','X0','boolean'),row('length','D156','int32')];
  let result=JSON.parse(JSON.stringify(context.readTelemetryFields(controls,rows)));
  assert.deepEqual(result.fields.level,{address:'X0',type:'boolean',trigger:true});
  assert.equal(Object.keys(result.fields).length,2);
  result=context.readTelemetryFields(controls,[]);assert.equal(Object.keys(result.fields).length,0);
  result=JSON.parse(JSON.stringify(context.readTelemetryFields(controls,rows)));
  assert.deepEqual(result.fields.length,{address:'D156',type:'int32',scale:0.01,offset:0,wordOrder:'highFirst',trigger:true});
  rows[1].trigger.checked=false;result=context.readTelemetryFields(controls,rows);
  assert.equal(result.fields.length.trigger,false);
  rows[1].name.value='编码器位移';result=context.readTelemetryFields(controls,rows);
  assert.equal(result.fields.length.name,'编码器位移');
  assert.equal(Object.hasOwn(result.fields,'编码器位移'),false);
  rows[1].key.value='level';assert.throws(()=>context.readTelemetryFields(controls,rows),/重复/);
  rows[1].key.value='length';rows[1].address.value='X1';
  assert.throws(()=>context.readTelemetryFields(controls,rows),/位类型/);rows[1].address.value='D156';
  controls.mode.value='change';controls.heartbeatMs.value='100';
  assert.throws(()=>context.readTelemetryFields(controls,rows),/不能小于/);
  controls.heartbeatMs.value='1000';result=context.readTelemetryFields(controls,rows);
  assert.equal(Object.hasOwn(result,'intervalMs'),false);
  controls.topic.value='/bad/+';assert.throws(()=>context.readTelemetryFields(controls,rows),/Topic/);
});

test('telemetry UI deletes fields, saves the remaining list, and restores defaults',async()=>{
  const b=browser([]),writes=[],deletes=[],validations=[];let savedFile;
  function element(tag,text){return {tag,textContent:text,children:[],attributes:{},value:'',checked:false,
    append(...children){this.children.push(...children);},setAttribute(key,value){this.attributes[key]=value;},
    remove(){},focus(){}};}
  b.context.node=element;
  b.context.pluginEval=async source=>{validations.push(source);return true;};
  b.context.writePluginFile=async(url,value,etag)=>{writes.push({url,value,etag});savedFile=value;};
  b.context.deletePluginFile=async(url,etag)=>{deletes.push({url,etag});savedFile=undefined;};
  b.context.readPluginFile=async()=>savedFile===undefined?{value:{},etag:null}:{value:savedFile,etag:'new-etag'};
  b.context.renderPluginDetail=()=>{};
  const defaults={enabled:true,mode:'interval',intervalMs:1000,fields:{level:{address:'X0',type:'boolean'},count:{address:'D100',type:'uint32'}}};
  const fileValue={...defaults,fields:{level:defaults.fields.level}};
  const detail={item:{id:'io'},telemetry:{defaults,values:defaults},telemetryFile:{value:fileValue,etag:'old-etag'}};
  const target=element('section');b.context.renderTelemetryForm(detail,target);
  const form=target.children[0],all=[];
  function collect(element){all.push(element);for(const child of element.children || [])collect(child);}
  collect(form);
  assert.equal(detail.telemetryRows.length,1);
  assert.equal(all.some(element=>element.tag==='th' && element.textContent==='上报'),false);
  all.find(element=>element.textContent==='删除').onclick();assert.equal(detail.telemetryRows.length,0);
  await form.onsubmit({preventDefault(){}});
  assert.equal(writes.length,1);assert.equal(writes[0].etag,'old-etag');
  assert.deepEqual(JSON.parse(JSON.stringify(writes[0].value.fields)),{});
  assert.match(validations[0],/validateTelemetry/);
  await all.find(element=>element.textContent==='恢复插件默认').onclick();
  assert.equal(writes.length,1);assert.equal(deletes.length,1);
  assert.equal(deletes[0].etag,'new-etag');assert.equal(detail.telemetryFile.etag,null);
  assert.match(validations[1],/"level"/);
  b.context.renderTelemetryForm(detail,element('section'));assert.equal(detail.telemetryRows.length,2);
  b.context.pluginEval=async()=>{throw new Error('invalid register range');};
  await form.onsubmit({preventDefault(){}});assert.equal(writes.length,1);
});

test('restoring telemetry defaults deletes with ETag and rejects concurrent edits',async()=>{
  const b=browser([{ok:false,status:412}]);
  await assert.rejects(b.context.deletePluginFile('/telemetry','"old"'),/其他客户端修改/);
  assert.equal(b.requests[0].options.method,'DELETE');
  assert.equal(b.requests[0].options.headers['If-Match'],'"old"');
  await b.context.deletePluginFile('/telemetry',null);assert.equal(b.requests.length,1);
});

test('plugin reboot requires confirmation and waits for a restarted device',async()=>{
  const b=browser([]),calls=[];
  let reloaded=0,approved=false;
  b.context.window.confirm=()=>approved;
  b.context.location.reload=()=>{reloaded++;};
  b.context.api=async(path,options)=>{
    calls.push({path,method:options?.method});
    if(path==='/api/v1/ota/reboot')return {ok:true,state:'rebooting'};
    return {uptimeMs:calls.length===1?100000:100};
  };
  await b.elements.get('#reboot-plugins').onclick();
  assert.equal(calls.length,0);
  approved=true;
  await b.elements.get('#reboot-plugins').onclick();
  assert.equal(calls[1].path,'/api/v1/ota/reboot');
  assert.equal(calls[1].method,'POST');
  assert.equal(reloaded,1);
});

test('plugin config writes use ETag and detect concurrent changes',async()=>{
  const response=(status,value,etag)=>({status,ok:status>=200&&status<300,
    text:async()=>JSON.stringify(value),headers:{get:()=>etag}});
  const b=browser([response(200,{count:2},'"sha256-old"'),response(412,{},null)]);
  const file=await b.context.readPluginFile('/config');
  assert.equal(file.value.count,2);assert.equal(file.etag,'"sha256-old"');
  await assert.rejects(b.context.writePluginFile('/config',{count:3},file.etag),/其他客户端修改/);
  assert.equal(b.requests[1].options.headers['If-Match'],'"sha256-old"');
  assert.equal(JSON.parse(b.requests[1].options.body).count,3);
  const missing=browser([response(404,{},null),response(200,{},null)]);
  const absent=await missing.context.readPluginFile('/config');
  await missing.context.writePluginFile('/config',{count:1},absent.etag);
  assert.equal(missing.requests[1].options.headers['If-None-Match'],'*');
});
