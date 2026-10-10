'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const {test}=require('node:test');
const source=fs.readFileSync(require('node:path').resolve(__dirname,'../../components/app_ui/assets/app.js'),'utf8');
const code=source.slice(source.indexOf('const mqttState='),source.indexOf('// End of MQTT page code.'));
test('MQTT forms retain omitted passwords, support explicit clearing and validate enabled URLs',()=>{
  const elements=new Map(),context={Map,JSON,Number,String,Promise,
    $(id){if(!elements.has(id))elements.set(id,{});return elements.get(id);}};
  vm.createContext(context);vm.runInContext(code,context);
  vm.runInContext(`mqttState.fields={
    enabled:{checked:true},url:{value:'mqtt://broker:1883'},username:{value:'user'},
    password:{value:''},clear:{checked:false}}`,context);
  let config=JSON.parse(JSON.stringify(context.readMqttFields()));
  assert.deepEqual(config,{enabled:true,url:'mqtt://broker:1883',username:'user'});
  assert.equal(Object.hasOwn(config,'password'),false);
  vm.runInContext(`mqttState.fields.password.value='secret'`,context);
  assert.equal(context.readMqttFields().password,'secret');
  vm.runInContext(`mqttState.fields.clear.checked=true`,context);
  assert.equal(context.readMqttFields().password,'');
  vm.runInContext(`mqttState.fields.url.value=''`,context);
  assert.throws(()=>context.readMqttFields(),/必须填写地址/);
  vm.runInContext(`mqttState.fields.url.value='mqtt://user:secret@broker'`,context);
  assert.throws(()=>context.readMqttFields(),/格式无效/);
});
