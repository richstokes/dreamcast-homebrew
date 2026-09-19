/* Pixel data only crosses the network; imported source images stay in-browser. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const form = $('studio-form');
  if (!form || !window.PointerEvent) return;
  const editor = $('pixel-editor'), ctx = editor.getContext('2d');
  let colours = ['ffff','f000','ff60','ffb0','ffdf','fa42','fa13','ff68',
    'fc4b','f738','f348','f16c','f3bd','f5b8','f373','faaa'];
  let pixels = Array(1024).fill(0), mono = Array(1024).fill(0);
  let mode = 'colour', selected = 2, ink = 1, cursor = [15,15], drawing = false, last = null;
  const history = [];
  const status = message => { $('studio-status').textContent = message; };
  function rgb(hex) { return [1,2,3].map(i => parseInt(hex[i],16)*17); }
  function colour(hex) { return `rgb(${rgb(hex).join(',')})`; }
  function remember() {
    history.push({pixels: [...pixels], mono: [...mono], colours: [...colours]});
    if (history.length > 30) history.shift();
  }
  // A small pixel diamond gives the preview a starting point that is easy to edit.
  for (let y=6;y<26;y++) for (let x=6;x<26;x++) {
    const d = Math.abs(x-15.5)+Math.abs(y-15.5);
    if (d<11 && d>6) { pixels[y*32+x]=2; mono[y*32+x]=1; }
  }
  function serialize() {
    $('pixel-data').value = pixels.map(n=>n.toString(16)).join('');
    $('mono-data').value = mono.join('');
    $('palette-data').value = colours.join('');
  }
  function preview(canvas, values, palette) {
    const c=canvas.getContext('2d');
    values.forEach((v,i)=> { c.fillStyle=palette[v]; c.fillRect(i%32, Math.floor(i/32),1,1); });
  }
  function render() {
    const values=mode==='colour'?pixels:mono, palette=mode==='colour'?colours.map(colour):['#d1dbbb','#263229'];
    values.forEach((v,i)=> { ctx.fillStyle=palette[v]; ctx.fillRect(i%32*16,Math.floor(i/32)*16,16,16); });
    ctx.strokeStyle='rgba(70,80,90,.2)'; ctx.lineWidth=1;
    for(let i=0;i<=32;i++) {ctx.beginPath();ctx.moveTo(i*16,0);ctx.lineTo(i*16,512);ctx.moveTo(0,i*16);ctx.lineTo(512,i*16);ctx.stroke();}
    if(document.activeElement===editor) {
      ctx.strokeStyle='#1565e0';ctx.lineWidth=2;ctx.strokeRect(cursor[0]*16+1,cursor[1]*16+1,14,14);
    }
    preview($('colour-preview'),pixels,colours.map(colour));
    preview($('mono-preview'),mono,['#d1dbbb','#263229']);
    $('undo').disabled=!history.length;
    serialize();
  }
  function palette() {
    $('palette').replaceChildren();
    const list=mode==='colour'?colours:['fdcb','f232'];
    list.forEach((hex,index)=> {
      const button=document.createElement('button'); button.type='button';
      button.setAttribute('aria-label',mode==='colour'?`Colour ${index+1}: #${rgb(hex).map(n=>n.toString(16).padStart(2,'0')).join('')}`:index?'Black':'White');
      button.setAttribute('aria-pressed',String(index===(mode==='colour'?selected:ink)));
      const swatch=document.createElement('canvas');swatch.width=24;swatch.height=24;swatch.setAttribute('aria-hidden','true');
      swatch.getContext('2d').fillStyle=colour(hex);swatch.getContext('2d').fillRect(0,0,24,24);
      button.append(swatch);button.addEventListener('click',()=>{if(mode==='colour')selected=index;else ink=index;palette();});
      $('palette').append(button);
    });
    $('custom-colour').value='#'+rgb(colours[selected]).map(n=>n.toString(16).padStart(2,'0')).join('');
    $('colour-control').hidden=mode!=='colour';
    $('edit-colour').setAttribute('aria-pressed',String(mode==='colour'));
    $('edit-mono').setAttribute('aria-pressed',String(mode==='mono'));
  }
  function drawPoint(x,y,erase=false) { (mode==='colour'?pixels:mono)[y*32+x]=erase?0:mode==='colour'?selected:ink; }
  function point(e) {
    const r=editor.getBoundingClientRect();
    return [Math.max(0,Math.min(31,Math.floor((e.clientX-r.left)*32/r.width))),Math.max(0,Math.min(31,Math.floor((e.clientY-r.top)*32/r.height)))];
  }
  function paint(e) {
    const p=point(e), from=last||p, steps=Math.max(Math.abs(p[0]-from[0]),Math.abs(p[1]-from[1]),1);
    for(let i=0;i<=steps;i++)drawPoint(Math.round(from[0]+(p[0]-from[0])*i/steps),Math.round(from[1]+(p[1]-from[1])*i/steps));
    cursor=p;last=p;render();
  }
  editor.addEventListener('pointerdown',e=>{if(e.button!==0)return;e.preventDefault();editor.focus();remember();drawing=true;last=null;editor.setPointerCapture(e.pointerId);paint(e);});
  editor.addEventListener('pointermove',e=>{if(drawing)paint(e);});
  for(const event of ['pointerup','pointercancel','lostpointercapture'])editor.addEventListener(event,()=>{drawing=false;last=null;});
  editor.addEventListener('keydown',e=>{
    const directions={ArrowLeft:[-1,0],ArrowRight:[1,0],ArrowUp:[0,-1],ArrowDown:[0,1]};
    if(directions[e.key]) {e.preventDefault();cursor=cursor.map((v,i)=>Math.max(0,Math.min(31,v+directions[e.key][i])));render();}
    else if([' ','Enter','Delete','Backspace'].includes(e.key)){e.preventDefault();remember();drawPoint(...cursor,['Delete','Backspace'].includes(e.key));render();}
  });
  editor.addEventListener('focus',render);editor.addEventListener('blur',render);
  $('edit-colour').onclick=()=>{mode='colour';palette();render();};
  $('edit-mono').onclick=()=>{mode='mono';palette();render();};
  $('custom-colour').addEventListener('change',e=>{
    remember();colours[selected]='f'+[1,3,5].map(i=>Math.round(parseInt(e.target.value.slice(i,i+2),16)/17).toString(16)).join('');palette();render();
  });
  $('undo').onclick=()=>{const previous=history.pop();if(previous){({pixels,mono,colours}=previous);palette();render();status('Last edit undone.');}};
  $('clear-icon').onclick=()=>{remember();(mode==='colour'?pixels:mono).fill(0);render();status('Icon cleared. Undo restores it.');};
  function monochrome() {mono=pixels.map(v=>{const [r,g,b]=rgb(colours[v]);return r*.299+g*.587+b*.114<160?1:0;});}
  $('derive-mono').onclick=()=>{remember();monochrome();render();status('VMU icon updated from the colour artwork.');};
  $('image-input').addEventListener('change',async e=>{
    const file=e.target.files[0];if(!file)return;
    if(file.size>8*1024*1024 || !['image/png','image/jpeg','image/webp','image/gif'].includes(file.type)){status('Choose a PNG, JPEG, WebP or GIF up to 8 MB.');return;}
    $('save-icons').disabled=true;status('Converting image…');
    try {
      const image=await createImageBitmap(file);
      if(image.width*image.height>16000000){image.close();throw new Error('Use an image smaller than 16 megapixels.');}
      const small=document.createElement('canvas');small.width=32;small.height=32;
      const c=small.getContext('2d');c.fillStyle='white';c.fillRect(0,0,32,32);
      const scale=Math.min(32/image.width,32/image.height),w=image.width*scale,h=image.height*scale;
      c.drawImage(image,(32-w)/2,(32-h)/2,w,h);image.close();
      const bytes=c.getImageData(0,0,32,32).data;
      remember();
      if(mode==='mono')mono=Array.from({length:1024},(_,i)=>bytes[i*4]*.299+bytes[i*4+1]*.587+bytes[i*4+2]*.114<160?1:0);
      else {
        const sample=Array.from({length:1024},(_,i)=>'f'+[0,1,2].map(j=>Math.round(bytes[i*4+j]/17).toString(16)).join(''));
        const counts=new Map();sample.forEach(v=>counts.set(v,(counts.get(v)||0)+1));
        colours=['ffff',...Array.from(counts).sort((a,b)=>b[1]-a[1]).map(([v])=>v).filter(v=>v!=='ffff').slice(0,15)];
        while(colours.length<16)colours.push('f000');
        pixels=sample.map(v=>{const channels=rgb(v);let best=0,distance=Infinity;colours.forEach((candidate,i)=>{const d=rgb(candidate).reduce((sum,c,j)=>sum+(c-channels[j])**2,0);if(d<distance){best=i;distance=d;}});return best;});
        monochrome();
      }
      palette();render();status('Image converted. You can refine the pixels before saving.');
    } catch(error) { status(error.message==='Use an image smaller than 16 megapixels.'?error.message:'Could not read that image. Try a PNG or JPEG.'); }
    finally { $('save-icons').disabled=false;e.target.value=''; }
  });
  form.addEventListener('submit',()=>{serialize();$('save-icons').disabled=true;});
  palette();render();$('save-icons').disabled=false;
})();
