// Original procedural BlastIRC sound design, dedicated to CC0 1.0 Universal.
// Regenerate: node assets/generate-sfx.mjs /path/to/ffmpeg
// No downloaded samples, dependencies, temporary audio files, or musical sources.
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
const definitions={boom:1.45,rumble:2.4,ding:1.3,blip:.25,tick:.12,whoosh:.65,click:.13,pop:.3,hush:.7,power_down:1.4};
for(const [name,duration] of Object.entries(definitions)){
    const samples=Math.ceil(duration*rate),mono=new Float64Array(samples);
    let phase=0,filtered=0,slow=0;
    for(let i=0;i<samples;i++){
        const t=i/rate,u=t/duration,n=noise();
        filtered+=(n-filtered)*.11;slow+=(n-slow)*.008;
        const edge=smooth(t/.008)*(1-smooth((u-.82)/.18));
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
        case 'ding':
            value=(Math.sin(tau*880*t)*.37*Math.exp(-t*4)+Math.sin(tau*1320*t)*.11*Math.exp(-t*5)+Math.sin(tau*2217*t)*.045*Math.exp(-t*8))*smooth(t/.025);
            break;
        case 'blip':
            phase+=tau*(650+350*smooth(t/.14))/rate;
            value=(Math.sin(phase)+Math.sin(phase*2)*.12)*.3*Math.pow(Math.sin(Math.PI*u),2);
            break;
        case 'tick':
            value=(Math.sin(tau*1700*t)*.22+filtered*.18)*Math.exp(-t*65)*smooth(t/.003);
            break;
        case 'whoosh':{
            phase+=tau*(1000-550*smooth(u))/rate;
            const reverse=Math.pow(Math.sin(Math.PI*u),2);
            value=(filtered*.48+Math.sin(phase)*.07)*reverse;
            break;}
        case 'click':
            value=(Math.sin(tau*610*t)*.32+Math.sin(tau*917*t)*.11+n*.06)*Math.exp(-t*52)*smooth(t/.003);
            break;
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
        }
        mono[i]=value*edge;
    }
    // A small stereo room adds cinematic space without clipping or harsh transients.
    const stereo=Buffer.alloc(samples*8);
    const taps=name==='tick'||name==='click'?[[.021,.08]]:[[.057,.16],[.113,.10],[.193,.055],[.317,.025]];
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
