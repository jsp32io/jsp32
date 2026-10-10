'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const assets = path.resolve(__dirname, '../../components/app_ui/assets');
const script = fs.readFileSync(path.join(assets, 'app.js'), 'utf8');
const html = fs.readFileSync(path.join(assets, 'index.html'), 'utf8');
// Execute the actual routing section with browser history/DOM adapters.
const routing = script.slice(script.indexOf('function navigate()'));
function browser(startPath, smallScreen=false) {
  const calls = {info:0, status:0, config:0, registers:0, stopRegisters:0, plugins:0, mqtt:0, mqttStatus:0};
  const elements = new Map();
  function element(selector) {
    if (!elements.has(selector)) elements.set(selector, {
      hidden:false, attributes:{},
      removeAttribute(name) {delete this.attributes[name];},
      setAttribute(name,value) {this.attributes[name]=value;},
      focus() {context.document.activeElement=this;},
      querySelectorAll() {return links.slice(1);},
      querySelector() {return links.slice(1).find(link=>link.attributes['aria-current']);}
    });
    return elements.get(selector);
  }
  const links = [];
  for (const match of html.matchAll(/<a\b([^>]+)>/g)) {
    const attrs=match[1];
    if (!/class="brand"|id="(?:overview|network|registers|plugins|mqtt)-tab"/.test(attrs)) continue;
    const href=attrs.match(/href="([^"]+)"/)[1];
    const id=attrs.match(/id="([^"]+)"/);
    const link=element(id?'#'+id[1]:'.brand');
    link.href=new URL(href,'http://device'+startPath).href;
    link.addEventListener=function(name,handler){this[name]=handler;};
    links.push(link);
  }
  const classes=new Set();
  const media={matches:smallScreen, listeners:{}, addEventListener(name,handler){this.listeners[name]=handler;}};
  const context = {
    URL, config:null, document:{hidden:false, activeElement:null, querySelectorAll(){return links;},
      body:{classList:{contains(name){return classes.has(name);},toggle(name,on){if(on)classes.add(name);else classes.delete(name);return on;}}}},
    location:new URL('http://device'+startPath),
    $:element,
    loadConfig(){calls.config++;context.config={};},
    refreshInfo(){calls.info++;},
    refreshStatus(){calls.status++;},
    startRegisters(){calls.registers++;},
    stopRegisters(){calls.stopRegisters++;},
    loadPlugins(){calls.plugins++;},
    stopTelemetryValues(){},
    loadMqtt(){calls.mqtt++;},
    refreshMqttStatus(){calls.mqttStatus++;},
    setInterval(handler){context.poll=handler;}
  };
  const events={};
  context.window={matchMedia(){return media;},addEventListener(name,handler){events[name]=handler;}};
  const entries=[context.location.href];let position=0;
  context.history={pushState(state,title,url){
    entries.splice(position+1);entries.push(new URL(url,context.location).href);
    position++;context.location=new URL(entries[position]);
  }};
  vm.runInNewContext(routing,context);
  function click(index,extra={}) {
    const event={button:0,defaultPrevented:false,preventDefault(){this.defaultPrevented=true;},...extra};
    links[index].click(event);return event;
  }
  function move(delta){position+=delta;context.location=new URL(entries[position]);events.popstate();}
  return {context,calls,element,click,move,entries,events,media};
}

test('direct /network entry and refresh select network page using pathname',()=>{
  for(let i=0;i<2;i++){
    const b=browser('/network?source=refresh');
    assert.equal(b.element('#network-page').hidden,false);
    assert.equal(b.element('#overview-page').hidden,true);
    assert.equal(b.element('#network-tab').attributes['aria-current'],'page');
    assert.equal(b.calls.info,0);assert.equal(b.calls.status,1);assert.equal(b.calls.config,1);
    b.context.poll();assert.equal(b.calls.status,2);
  }
});
test('click navigation and back/forward keep the same document and draft',()=>{
  const b=browser('/');assert.equal(b.calls.info,1);
  assert.equal(b.click(2).defaultPrevented,true);
  assert.equal(b.context.location.pathname,'/network');
  const draft={ssid:'unsaved'};b.context.config=draft;
  b.click(1);assert.equal(b.context.location.pathname,'/');
  b.move(-1);assert.equal(b.context.location.pathname,'/network');
  assert.equal(b.element('#network-page').hidden,false);
  assert.equal(b.context.config,draft);assert.equal(b.calls.config,1);
  b.move(1);assert.equal(b.context.location.pathname,'/');
  assert.equal(b.element('#overview-tab').attributes['aria-current'],'page');
  const count=b.entries.length;b.click(1);assert.equal(b.entries.length,count);
});
test('modified and middle clicks retain native browser behavior',()=>{
  const b=browser('/');
  for(const extra of [{ctrlKey:true},{metaKey:true},{shiftKey:true},{altKey:true},{button:1}])
    assert.equal(b.click(2,extra).defaultPrevented,false);
  assert.equal(b.context.location.pathname,'/');assert.equal(b.entries.length,1);
});
test('periodic refresh follows the current path and pauses while hidden',()=>{
  const b=browser('/');b.context.poll();assert.equal(b.calls.info,2);
  b.click(2);b.context.poll();assert.equal(b.calls.status,2);
  b.context.document.hidden=true;b.context.poll();assert.equal(b.calls.status,2);
});
test('register page supports direct entry, navigation, and releases its subscription on exit',()=>{
  const direct=browser('/registers');
  assert.equal(direct.element('#registers-page').hidden,false);
  assert.equal(direct.element('#registers-tab').attributes['aria-current'],'page');
  assert.equal(direct.calls.registers,1);
  direct.context.poll();assert.equal(direct.calls.info,0);
  const b=browser('/');b.click(3);
  assert.equal(b.context.location.pathname,'/registers');
  b.move(-1);assert.equal(b.context.location.pathname,'/');
  assert.ok(b.calls.stopRegisters>=2);
  b.move(1);assert.equal(b.calls.registers,2);
});
test('plugin page supports direct entry and sidebar navigation',()=>{
  const direct=browser('/plugins');
  assert.equal(direct.element('#plugins-page').hidden,false);
  assert.equal(direct.element('#plugins-tab').attributes['aria-current'],'page');
  assert.equal(direct.calls.plugins,1);
  direct.context.poll();assert.equal(direct.calls.info,0);
  const b=browser('/');b.click(4);
  assert.equal(b.context.location.pathname,'/plugins');
  assert.equal(b.calls.plugins,1);
  b.move(-1);assert.equal(b.context.location.pathname,'/');
  b.move(1);assert.equal(b.calls.plugins,2);
});


test('small-screen sidebar opens, confines keyboard focus and closes on Escape',()=>{
  const b=browser('/network',true),menu=b.element('#menu-toggle'),sidebar=b.element('#sidebar');
  assert.equal(sidebar.inert,true);assert.equal(b.element('#sidebar-backdrop').hidden,true);
  menu.onclick();assert.equal(menu.attributes['aria-expanded'],'true');
  assert.equal(sidebar.inert,false);assert.equal(b.element('main').inert,true);
  assert.equal(b.context.document.activeElement,b.element('#network-tab'));
  b.element('#mqtt-tab').focus();
  let prevented=false;
  b.events.keydown({key:'Tab',shiftKey:false,preventDefault(){prevented=true;}});
  assert.equal(prevented,true);assert.equal(b.context.document.activeElement,b.element('#overview-tab'));
  b.events.keydown({key:'Tab',shiftKey:true,preventDefault(){}});
  assert.equal(b.context.document.activeElement,b.element('#mqtt-tab'));
  b.events.keydown({key:'Escape',preventDefault(){}});
  assert.equal(sidebar.inert,true);assert.equal(menu.attributes['aria-expanded'],'false');
  assert.equal(b.element('main').inert,false);assert.equal(b.context.document.activeElement,menu);
});

test('MQTT page supports direct entry, sidebar navigation and status polling',()=>{
  const direct=browser('/mqtt');
  assert.equal(direct.element('#mqtt-page').hidden,false);
  assert.equal(direct.element('#overview-page').hidden,true);
  assert.equal(direct.element('#mqtt-tab').attributes['aria-current'],'page');
  assert.equal(direct.calls.mqtt,1);direct.context.poll();assert.equal(direct.calls.mqttStatus,1);
  const b=browser('/');b.click(5);assert.equal(b.context.location.pathname,'/mqtt');
  b.move(-1);assert.equal(b.element('#mqtt-page').hidden,true);
});
test('mobile navigation and backdrop close the sidebar without reloading',()=>{
  const b=browser('/',true),menu=b.element('#menu-toggle');
  menu.onclick();b.click(2);
  assert.equal(b.context.location.pathname,'/network');assert.equal(menu.attributes['aria-expanded'],'false');
  assert.equal(b.element('#sidebar').inert,true);
  menu.onclick();b.element('#sidebar-backdrop').onclick();
  assert.equal(b.element('#sidebar-backdrop').hidden,true);assert.equal(b.element('main').inert,false);
});
test('desktop resize restores a visible, keyboard-accessible sidebar',()=>{
  const b=browser('/',true);b.element('#menu-toggle').onclick();
  b.media.matches=false;b.media.listeners.change();
  assert.equal(b.element('#sidebar').inert,false);assert.equal(b.element('main').inert,false);
  assert.equal(b.context.document.body.classList.contains('sidebar-open'),false);
});
