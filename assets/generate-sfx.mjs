// Original procedural BlastIRC sounds, dedicated to CC0 1.0 Universal.
// Regenerate: node assets/generate-sfx.mjs /path/to/ffmpeg
// All imported cues are documented in sfx/LICENSE and left untouched.
import {mkdirSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const rate=44100, tau=2*Math.PI;
let seed=0xb1a57;
function noise(){seed^=seed<<13;seed^=seed>>>17;seed^=seed<<5;return (seed>>>0)/2147483648-1;}
function smooth(t){t=Math.max(0,Math.min(1,t));return t*t*(3-2*t);}
const output=path.join(path.dirname(fileURLToPath(import.meta.url)),'sfx');
mkdirSync(output,{recursive:true});
const ffmpeg=process.argv[2]||process.env.FFMPEG_BIN||'ffmpeg';
const definitions={boom:1.45,rumble:2.4,ding:1.3,blip:.25,tick:.12,whoosh:.65,click:.13,pop:.3,hush:.7,power_down:1.4,keytap:.032,keytap_space:.036,keytap_delete:.026,keytap_delete_word:.032};
const importedCues=new Set(['ding','tick','click']);
const typing={
    keytap:             {pitch:320,sweep:240,body:.42,damping:410,contact:.32,reverse:0},
    keytap_space:       {pitch:265,sweep:180,body:.39,damping:365,contact:.22,reverse:0},
    keytap_delete:      {pitch:295,sweep:195,body:.46,damping:560,contact:.30,reverse:.010},
    keytap_delete_word: {pitch:240,sweep:160,body:.45,damping:430,contact:.27,reverse:.013},
};
for(const [name,duration] of Object.entries(definitions)){
    const samples=Math.ceil(duration*rate);
    if(importedCues.has(name)){
        // Preserve the deterministic noise stream for the remaining original cues.
        for(let i=0;i<samples;i++)noise();
        continue;
    }
    const tap=typing[name];
    const mono=new Float64Array(samples);
    let phase=0,filtered=0,slow=0,fast=0;
    for(let i=0;i<samples;i++){
        const t=i/rate,u=t/duration,n=noise();
        filtered+=(n-filtered)*.11;slow+=(n-slow)*.008;
        const edge=smooth(t/(tap ? .00045 : .008))*(1-smooth((u-.82)/.18));
        let value=0;
        switch(name){
        case 'boom':{
            const f=31+92*Math.exp(-t*12);phase+=tau*f/rate;
            value=(Math.sin(phase)*Math.exp(-t*4.7)*.85+slow*Math.exp(-t*2.7)*2.5+filtered*Math.exp(-t*15)*.65)*smooth(t/.012);
            break;}
        case 'rumble':{
            const swell=smooth(t/.22)*(1-smooth((u-.5)/.5));
            value=(slow*2.4+Math.sin(tau*31*t)*.23+Math.sin(tau*43*t+Math.sin(t*3))*.13+Math.sin(tau*(94*t-12*t*t))*.06)*swell;
            break;}
        case 'blip':
            phase+=tau*(650+350*smooth(t/.14))/rate;
            value=(Math.sin(phase)+Math.sin(phase*2)*.12)*.3*Math.pow(Math.sin(Math.PI*u),2);
            break;
        case 'whoosh':{
            phase+=tau*(1000-550*smooth(u))/rate;
            const reverse=Math.pow(Math.sin(Math.PI*u),2);
            value=(filtered*.48+Math.sin(phase)*.07)*reverse;
            break;}
        case 'pop':
            phase+=tau*(130+620*Math.exp(-t*25))/rate;
            value=(Math.sin(phase)*.5+filtered*.12)*Math.exp(-t*19)*smooth(t/.006);
            break;
        case 'hush':
            value=(filtered*.28+slow*.8)*smooth(t/.07)*Math.exp(-t*5);
            break;
        case 'power_down':
            phase+=tau*(32+230*Math.exp(-t*3.1))/rate;
            value=(Math.sin(phase)*.33+Math.sin(phase*2)*.08+slow*.12)*smooth(t/.055)*Math.exp(-t*2.1);
            break;
        case 'keytap':
        case 'keytap_space':
        case 'keytap_delete':
        case 'keytap_delete_word':
            // A damped low knock with a much shorter, band-limited contact transient.
            phase+=tau*(tap.pitch+tap.sweep*Math.exp(-t*820))/rate;
            fast+=(n-fast)*.45;
            value=Math.sin(phase)*tap.body*Math.exp(-t*tap.damping)+(fast-filtered)*tap.contact*Math.exp(-t*1150);
            break;
        }
        mono[i]=value*edge;
    }
    if(tap?.reverse){
        // Reverse just the contact, not the silence that pads the clip.
        const end=Math.ceil(tap.reverse*rate);
        mono.subarray(0,end).reverse();
        mono.fill(0,end);
    }
    // A small stereo room adds space to event cues; typing taps stay dry.
    const stereo=Buffer.alloc(samples*8);
    const taps=tap?[]:[[.057,.16],[.113,.10],[.193,.055],[.317,.025]];
    for(let i=0;i<samples;i++){
        let left=mono[i],right=mono[i]*.96;
        for(const [delay,gain] of taps){
            const l=i-Math.round(delay*rate),r=i-Math.round((delay+.007)*rate);
            if(l>=0)left+=mono[l]*gain;
            if(r>=0)right+=mono[r]*gain;
        }
        const tail=1-smooth((i/samples-.9)/.1);
        stereo.writeFloatLE(Math.tanh(left)*.72*tail,i*8);
        stereo.writeFloatLE(Math.tanh(right)*.72*tail,i*8+4);
    }
    const destination=path.join(output,`${name}.ogg`);
    const result=spawnSync(ffmpeg,['-hide_banner','-loglevel','error','-y','-f','f32le','-ar',String(rate),'-ac','2','-i','pipe:0','-c:a','libvorbis','-q:a','5','-metadata','artist=BlastIRC original synthesis','-metadata','license=CC0-1.0','-metadata',`title=BlastIRC ${name}`,destination],{input:stereo,encoding:'utf8',maxBuffer:1024*1024});
    if(result.error)throw result.error;
    if(result.status!==0)throw new Error(`${name}: ${result.stderr}`);
    console.log(destination);
}
