// Isolated DOM-contract checks. No browser, miner, network or external packages.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../../nexus-ui-desktop/web/desktop.js'),'utf8');
const start=source.indexOf('function patchPanel('),end=source.indexOf('function controlsPage(',start);
assert(start>=0&&end>start);
function element(tag,text=''){
 return {tagName:tag,textContent:text,className:'',disabled:false,title:'',value:'',attributes:{},
 getAttribute(name){return this.attributes[name]??null;},setAttribute(name,value){this.attributes[name]=value;},removeAttribute(name){delete this.attributes[name];}};
}
function row(name,control){
 const label=element('SPAN',name),value=element('SPAN','old');
 return {label,value,control,bar:null,
 querySelector(selector){return {'.name':this.label,'.value':this.value,'progress':this.bar,'.meter':this.meter}[selector]||null;},
 querySelectorAll(){return this.control?[this.control]:[];},append(item){this.bar=item;}};
}
function card(rows){return {querySelectorAll(){return rows;}};}
function context(active=null){const sandbox={document:{activeElement:active}};vm.createContext(sandbox);vm.runInContext(source.slice(start,end),sandbox);return sandbox;}
test('an open selector keeps identity, focus and in-progress value',()=>{
 const selected=element('SELECT');selected.value='nexus';const next=element('SELECT');next.value='auto';
 const before=row('Workload',selected),after=row('Workload',next);after.value.textContent='fresh';const ctx=context(selected);
 assert.equal(ctx.patchPanel(card([before]),card([after])),true);
 assert.equal(before.control,selected);assert.equal(ctx.document.activeElement,selected);assert.equal(selected.value,'nexus');assert.equal(before.value.textContent,'fresh');
});
test('unfocused controls synchronize saved state',()=>{
 const selected=element('SELECT'),next=element('SELECT');selected.value='auto';next.value='nexus';
 assert.equal(context().patchPanel(card([row('Workload',selected)]),card([row('Workload',next)])),true);assert.equal(selected.value,'nexus');
});
test('toggle updates retain the original event handler',()=>{
 const toggle=element('BUTTON','OFF'),next=element('BUTTON','ON'),handler=()=>{};toggle.onclick=handler;next.setAttribute('aria-checked','true');
 assert.equal(context().patchPanel(card([row('Device enabled',toggle)]),card([row('Device enabled',next)])),true);
 assert.equal(toggle.onclick,handler);assert.equal(toggle.textContent,'ON');assert.equal(toggle.getAttribute('aria-checked'),'true');
});
test('changed row structure requests a safe rebuild before mutation',()=>{
 const before=row('GPU'),after=row('Different');after.value.textContent='new';
 assert.equal(context().patchPanel(card([before]),card([after])),false);assert.equal(before.value.textContent,'old');
});
test('meter updates do not recreate controls',()=>{
 const before=row('Load'),after=row('Load');before.bar=element('PROGRESS');before.bar.value=1;after.bar=element('PROGRESS');after.bar.value=98;const meter=before.bar;
 assert.equal(context().patchPanel(card([before]),card([after])),true);assert.equal(before.bar,meter);assert.equal(meter.value,98);
});

test('metric rows keep explicit meter and action roles',()=>{
 const begin=source.indexOf('function rowNode('),end=source.indexOf('function panelNode(',begin);
 const make=(tag,text='',classes='')=>({tagName:tag,textContent:text,className:classes,children:[],classList:{add(){}},append(...items){this.children.push(...items)},setAttribute(){}});
 const sandbox={make,role:()=>'',inlineActions:()=>[],tab:0};vm.createContext(sandbox);vm.runInContext(source.slice(begin,end),sandbox);
 const row=sandbox.rowNode(['GPU load','98%','',98],{},{});
 assert.equal(row.children.length,3);assert.equal(row.children[2].className,'meter');assert.equal(row.children[2].children[0].tagName,'progress');assert.equal(row.children[2].children[0].value,98);assert.equal(row.children[2].children[1].textContent,'98%');
});

test('live GPU meters update fill and caption together without replacing the meter',()=>{
 const before=row('GPU load'),after=row('GPU load');
 before.bar=element('PROGRESS');after.bar=element('PROGRESS');
 const oldCaption=element('SPAN','98%'),newCaption=element('SPAN','27%');
 before.bar.value=98;after.bar.value=27;after.bar.setAttribute('aria-valuetext','27%');
 before.meter={title:'GPU load: 98%',querySelector:()=>oldCaption};after.meter={title:'GPU load: 27%',querySelector:()=>newCaption};
 const meter=before.meter;
 assert.equal(context().patchPanel(card([before]),card([after])),true);
 assert.equal(before.meter,meter);assert.equal(before.bar.value,27);assert.equal(oldCaption.textContent,'27%');
 assert.equal(before.bar.getAttribute('aria-valuetext'),'27%');assert.equal(before.meter.title,'GPU load: 27%');
});

test('GPU meter displays real zero, rounds captions and omits unavailable data',()=>{
 const begin=source.indexOf('function rowNode('),end=source.indexOf('function panelNode(',begin);
 const make=(tag,text='',classes='')=>({tagName:tag,textContent:text,className:classes,children:[],classList:{add(){}},append(...items){this.children.push(...items)},setAttribute(){}});
 const sandbox={make,role:()=>'',inlineActions:()=>[],tab:0};vm.createContext(sandbox);vm.runInContext(source.slice(begin,end),sandbox);
 for(const [input,value,label] of [[0,0,'0%'],[37.5,37.5,'38%'],[100,100,'100%'],[125,100,'100%'],[-5,0,'0%']]) {
  const meter=sandbox.rowNode(['GPU load','sample','',input],{},{}).children[2];
  assert.equal(meter.children[0].value,value);assert.equal(meter.children[1].textContent,label);
 }
 for(const value of [null,undefined,NaN]) assert.equal(sandbox.rowNode(['GPU load','N/A','',value],{},{}).children.length,2);
});

test('workload selectors address primary and secondary by identity',()=>{
 const begin=source.indexOf('function inlineActions('),end=source.indexOf('function rowNode(',begin);
 const calls=[];
 const sandbox={tab:1,select:(options,value,onchange)=>({options,value,onchange}),command:(...a)=>calls.push(a)};
 vm.createContext(sandbox);vm.runInContext(source.slice(begin,end),sandbox);
 const devices=[{id:'pci:amd',vendor:'amd',runtime_lane:'rx',options:[['Automatic','auto'],['JIT','nexus'],['BDA','nexus_bda']]},
                {id:'GPU-nv',vendor:'nvidia',runtime_lane:'rtx',options:[['Automatic','auto'],['CUDA','nvidia'],['JIT','nexus'],['BDA','nexus_bda']]}];
 const frame={desktop:{devices,preferences:{workloads:{'pci:amd':'nexus'}}}};
 for(const device of devices){const controls=sandbox.inlineActions({device_id:device.id},'Workload',frame);
   assert.equal(controls.length,1);controls[0].onchange('nexus_bda');assert.deepEqual(calls.at(-1)[1].device,device.id);}
});

test('GPU toggle uses explicit runtime lane rather than UUID/vendor heuristics',()=>{
 const begin=source.indexOf('function inlineActions('),end=source.indexOf('function rowNode(',begin),calls=[];
 const device={id:'pci:future-cuda',vendor:'nvidia',runtime_lane:'rtx',options:[]};
 const frame={desktop:{devices:[device],preferences:{workloads:{}}},gpu_devices:{selection:{nvidia_pom:true,amd_pom:false}}};
 const sandbox={tab:1,snapshot:frame,button:(text,fn)=>({text,fn,setAttribute(){}}),command:(...a)=>calls.push(a)};
 vm.createContext(sandbox);vm.runInContext(source.slice(begin,end),sandbox);
 const [toggle]=sandbox.inlineActions({device_id:device.id},'Device enabled',frame);
 assert.equal(toggle.text,'ON');toggle.fn();assert.equal(calls[0][1].enabled,false);
});
