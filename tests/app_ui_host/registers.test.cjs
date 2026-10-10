'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const {test}=require('node:test');
const script=fs.readFileSync(path.resolve(__dirname,'../../components/app_ui/assets/app.js'),'utf8');
const registers=script.slice(script.indexOf('const registerPageSize='),script.indexOf('function navigate()'));

class Element {
  constructor(tag='div') {this.tag=tag;this.children=[];this.dataset={};this.attributes={};this.textContent='';this.scrollTop=0;}
  append(...children){this.children.push(...children);}
  replaceChildren(...children){this.children=[...children];this.textContent='';}
  get firstChild(){return this.children[0];}
  get lastElementChild(){return this.children.at(-1);}
  setAttribute(key,value){this.attributes[key]=value;}
  addEventListener(name,fn){this[name]=fn;}
  querySelectorAll(selector){return selector==='button'?this.children:[];}
  querySelector(selector){
    const address=/data-address="([^"]+)"/.exec(selector)?.[1];
    return this.children.find(child=>child.dataset.address===address) || null;
  }
  closest(){return this.parent || null;}
  scrollIntoView(){this.scrolled=true;}
}
function deferred(){let resolve,reject;const promise=new Promise((ok,no)=>{resolve=ok;reject=no;});return {promise,resolve,reject};}
const tick=()=>new Promise(resolve=>setImmediate(resolve));
function snapshot(options,revision,value=false){
  const {start,count}=JSON.parse(options.body),area=start[0],index=Number(start.slice(1));
  return {revision,values:Array.from({length:count},(_,i)=>({address:`${area}${index+i}`,value}))};
}
function client(read=()=>({revision:'0',values:[]})) {
  const elements=new Map(),streams=[],requests=[],documentEvents={};
  function element(selector){if(!elements.has(selector))elements.set(selector,new Element());return elements.get(selector);}
  const areas=element('#register-areas');
  for(const area of ['X','Y','M','D']){const button=new Element('button');button.dataset.area=area;button.parent=areas;areas.append(button);}
  element('#register-values').parent=element('.register-table-wrap');
  class Stream {
    constructor(url){this.url=url;this.listeners={};this.closed=false;streams.push(this);}
    addEventListener(name,handler){this.listeners[name]=handler;}
    close(){this.closed=true;}
    emit(name,data){if(name==='open')this.onopen();else if(name==='error')this.onerror();else this.listeners[name]({data});}
  }
  const context=vm.createContext({
    Map,Number,String,JSON,EventSource:Stream,location:{pathname:'/registers'},
    document:{hidden:false,addEventListener(name,handler){documentEvents[name]=handler;}},
    $:element,node(tag,value,className){const item=new Element(tag);if(value!==undefined)item.textContent=value;if(className)item.className=className;return item;},
    message(selector,value){element(selector).textContent=value;},
    async api(url,options){requests.push({url,options});return url==='/api/v1/registers'?{areas:{X:35,Y:3,M:1,D:200},maxTransaction:16}:read(url,options);},
    setTimeout(){return 1;},clearTimeout(){},
  });
  vm.runInContext(registers,context);
  return {context,elements,element,streams,requests,documentEvents,eval(expression){return vm.runInContext(expression,context);}};
}

test('snapshot chunks respect maxTransaction and a newer SSE value wins exactly',async()=>{
  const first=deferred();let reads=0;
  const c=client((url,options)=>{reads++;return reads===1?first.promise:snapshot(options,'9007199254740993');});
  await c.eval('startRegisters()');
  assert.equal(c.streams[0].url,'/api/v1/registers/events?start=X0&count=32');
  c.streams[0].emit('open');
  c.streams[0].emit('change',JSON.stringify({r:'9007199254740994',a:'X0',v:true}));
  first.resolve({revision:'9007199254740993',values:Array.from({length:16},(_,i)=>({address:`X${i}`,value:false}))});
  await tick();await tick();
  assert.equal(c.eval('registerState.values.get("X0")'),true);
  assert.equal(c.requests.filter(request=>request.url.endsWith('/read')).length,2);
  assert.equal(JSON.parse(c.requests[1].options.body).count,16);
  assert.equal(JSON.parse(c.requests[2].options.body).start,'X16');
});

test('switching pages closes the old stream and ignores its pending snapshot',async()=>{
  const old=deferred();let reads=0;
  const c=client((url,options)=>++reads===1?old.promise:snapshot(options,'2',true));
  await c.eval('startRegisters()');c.streams[0].emit('open');
  c.element('#register-next').onclick();
  assert.equal(c.streams[0].closed,true);
  assert.equal(c.streams[1].url,'/api/v1/registers/events?start=X32&count=3');
  c.streams[1].emit('open');
  old.resolve({revision:'99',values:[{address:'X0',value:true}]});
  await tick();await tick();
  assert.equal(c.eval('registerState.values.has("X0")'),false);
  assert.equal(c.eval('registerState.values.get("X32")'),true);
});

test('a dropped event reads a new snapshot and leaving closes the stream',async()=>{
  let revision=0;
  const c=client((url,options)=>snapshot(options,String(++revision),Boolean(revision%2)));
  await c.eval('startRegisters()');c.streams[0].emit('open');await tick();
  c.streams[0].emit('dropped','5');await tick();
  assert.equal(c.requests.filter(request=>request.url.endsWith('/read')).length,4);
  c.eval('stopRegisters()');assert.equal(c.streams[0].closed,true);
});

test('address jump crosses areas and a hidden page releases its stream',async()=>{
  const c=client((url,options)=>snapshot(options,'1',JSON.parse(options.body).start[0]==='D'?42:false));
  await c.eval('startRegisters()');c.streams[0].emit('open');await tick();
  c.element('#register-address').value='D100';
  c.element('#register-jump').onsubmit({preventDefault(){}});
  assert.equal(c.streams[0].closed,true);
  assert.equal(c.streams[1].url,'/api/v1/registers/events?start=D96&count=32');
  assert.equal(c.element('#register-values').querySelector('[data-address="D100"]').scrolled,true);
  c.context.document.hidden=true;c.documentEvents.visibilitychange();
  assert.equal(c.streams[1].closed,true);
  c.context.document.hidden=false;c.documentEvents.visibilitychange();
  assert.equal(c.streams[2].url,'/api/v1/registers/events?start=D96&count=32');
});
