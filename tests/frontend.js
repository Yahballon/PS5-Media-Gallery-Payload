
'use strict';
const $ = id => document.getElementById(id);
let loading=false, signature='', loaded=false;
let items = [], apps = new Map(), activeFilter = 'all';
const library = $('library');
const fmt = x => Number(x || 0).toLocaleString();
const fileUrl = m => '/media/' + m.type + '/' + m.path.split('/').map(encodeURIComponent).join('/');
const label = m => m.type === 'photo' ? 'SCREENSHOT' : 'VIDEO';
const dateLabel = s => s ? new Date(s * 1000).toLocaleString() : '';
function make(tag, cls, text) { const el = document.createElement(tag); if(cls)el.className=cls; if(text!==undefined)el.textContent=text; return el; }
function fallbackIcon(app) { const el=make('div','app-icon app-fallback',app.id.slice(0,2).toUpperCase());return el; }
function showMedia(item){
  const view=$('viewer');view.replaceChildren();
  let media;
  if(item.type==='photo'){media=make('img');media.src=fileUrl(item);media.alt=item.name;}
  else{media=make('video');media.src=fileUrl(item);media.controls=true;media.autoplay=true;media.playsInline=true;media.preload='metadata';}
  view.append(media);
  $('viewDelete').onclick=()=>removeCaptures('/api/delete'+fileUrl(item),`Delete "${item.name}" permanently?`,false,true);$('viewName').textContent=item.name;
  $('viewSub').textContent=(apps.get(item.app)?.title||item.app)+' · '+dateLabel(item.date);
  $('overlay').classList.remove('hidden');document.body.style.overflow='hidden';
}
function hideMedia(){const view=$('viewer');view.querySelector('video')?.pause();view.replaceChildren();$('overlay').classList.add('hidden');document.body.style.overflow='';}
const viewDelete=make('button','delete-button','Delete this capture');viewDelete.id='viewDelete';$('overlay').append(viewDelete);
let deleting=false;
async function removeCaptures(url,message,all=false,closeViewer=false){
  if(deleting||!confirm(message))return;
  if(all&&prompt('Type DELETE ALL to permanently delete every photo and video:')!=='DELETE ALL')return;
  deleting=true;
  try{
    const response=await fetch(url,{method:'DELETE',headers:{'X-Gallery-Confirm':'delete'}});
    const result=await response.json();if(!response.ok)throw Error(result.error||'Delete failed');
    if(closeViewer)hideMedia();
    signature='';thumbs.clear();await load();
    if(result.failed)alert(`${result.deleted} deleted; ${result.failed} could not be deleted.`);
  }catch(err){alert(String(err));}finally{deleting=false;}
}
$('deleteAll').onclick=()=>removeCaptures('/api/delete/all','Permanently delete ALL photos and videos from every app? This cannot be undone.',true,true);
$('close').onclick=hideMedia;
$('overlay').addEventListener('click', e=>{if(e.target===$('overlay'))hideMedia();});
document.addEventListener('keydown',e=>{if(e.key==='Escape')hideMedia();});
const thumbs=new Map(), pending=[];
let thumbnailBusy=false;
const observer=new IntersectionObserver(entries=>{
  for(const entry of entries)if(entry.isIntersecting){
    observer.unobserve(entry.target);
    pending.push(entry.target);pumpThumbnail();
  }
},{rootMargin:'150px'});
async function pumpThumbnail(){
  if(thumbnailBusy)return;
  thumbnailBusy=true;
  while(pending.length){
    const tile=pending.shift();if(!tile.isConnected)continue;
    const video=document.createElement('video');
    video.muted=true;video.playsInline=true;video.preload='auto';
    await new Promise(resolve=>{
      let finished=false;
      const finish=()=>{if(finished)return;finished=true;clearTimeout(timer);video.removeAttribute('src');video.load();resolve();};
      const timer=setTimeout(finish,12000);
      video.onerror=finish;
      const draw=()=>{
        if(finished||!video.videoWidth)return;
        try{
          const canvas=document.createElement('canvas');canvas.width=320;canvas.height=180;
          canvas.getContext('2d').drawImage(video,0,0,320,180);
          const url=canvas.toDataURL('image/jpeg',0.75);
          if(thumbs.size>=150)thumbs.delete(thumbs.keys().next().value);
          thumbs.set(tile.dataset.key,url);
          if(tile.isConnected){const img=make('img');img.src=url;img.alt='Video preview';tile.replaceChildren(img);}
        }catch(e){/* Unsupported codecs retain the play icon. */}
        finish();
      };
      video.onloadeddata=()=>{
        if(Number.isFinite(video.duration)&&video.duration>0.2){video.onseeked=draw;video.currentTime=Math.min(1,video.duration/2);}
        else draw();
      };
      video.src=tile.dataset.src;video.load();
    });
  }
  thumbnailBusy=false;
}
function render(){
  observer.disconnect();pending.length=0;
  const query=$('search').value.toLocaleLowerCase().trim();
  const matched=items.filter(m=>(activeFilter==='all'||m.type===activeFilter)&&
    (!query||m.name.toLocaleLowerCase().includes(query)||m.app.toLowerCase().includes(query)||
    (apps.get(m.app)?.title||'').toLocaleLowerCase().includes(query)));
  library.replaceChildren();
  const groups=new Map();
  for(const m of matched){if(!groups.has(m.app))groups.set(m.app,[]);groups.get(m.app).push(m);}
  if(groups.size===0){
    const empty=make('div','empty');empty.append(make('strong','',items.length?'No matching captures':'No captures found'));
    empty.append(make('p','',items.length?'Try another filter or search term.':'Screenshots and video clips in /user/av_contents will appear here.'));
    library.append(empty);return;
  }
  for(const [id,media] of groups){
    const app=apps.get(id)||{id,title:id,icon:false};
    const box=make('section','app');
    const head=make('div','app-top');
    if(app.icon){const icon=make('img','app-icon');icon.src='/icon/'+encodeURIComponent(id);icon.alt='';icon.onerror=()=>icon.replaceWith(fallbackIcon(app));head.append(icon);}
    else head.append(fallbackIcon(app));
    const detail=make('div','app-detail');detail.append(make('h3','',app.title),make('small','',id));head.append(detail);
    const photos=media.filter(m=>m.type==='photo').length,videos=media.length-photos;
    head.append(make('span','app-count',`${fmt(photos)} photos  ·  ${fmt(videos)} videos`));
    const deleteApp=make('button','delete-button','Delete app captures');
    deleteApp.onclick=()=>removeCaptures('/api/delete/app/'+encodeURIComponent(id),`Delete ALL photos and videos for ${app.title}? This includes captures hidden by the current filter. This cannot be undone.`);
    head.append(deleteApp);
    box.append(head);
    const grid=make('div','media-grid');
    for(const m of media){
      const btn=make('button','media');btn.type='button';btn.title=m.name;btn.setAttribute('aria-label','Open '+m.name);
      if(m.type==='photo'){
        const img=make('img');img.loading='lazy';img.decoding='async';img.src=fileUrl(m);img.alt='';btn.append(img);
      } else {
        const tile=make('div','video-tile');const glow=make('div','glow');
        glow.innerHTML='<svg viewBox="0 0 24 24"><path d="M9 6l10 6-10 6z"/></svg>';
        tile.append(glow);btn.append(tile);
        tile.dataset.src=fileUrl(m);tile.dataset.key=fileUrl(m)+':'+m.date+':'+m.size;
        if(thumbs.has(tile.dataset.key)){const img=make('img');img.src=thumbs.get(tile.dataset.key);img.alt='Video preview';tile.replaceChildren(img);}
        else observer.observe(tile);
      }
      btn.append(make('span','type',label(m)));
      const shade=make('span','shade');shade.append(make('span','filename',m.name));btn.append(shade);
      btn.onclick=()=>showMedia(m);
      const card=make('div','media-card');card.append(btn);
      const remove=make('button','delete-button tile-delete','Delete');remove.type='button';
      remove.setAttribute('aria-label','Delete '+m.name);
      remove.onclick=()=>removeCaptures('/api/delete'+fileUrl(m),`Permanently delete "${m.name}"? This cannot be undone.`);
      card.append(remove);grid.append(card);
    }
    box.append(grid);library.append(box);
  }
}
document.querySelectorAll('.filter').forEach(btn=>btn.onclick=()=>{
  activeFilter=btn.dataset.filter;
  document.querySelectorAll('.filter').forEach(b=>b.setAttribute('aria-pressed',String(b===btn)));
  render();
});
$('search').addEventListener('input',render);
async function load(){
  if(loading||document.hidden)return;
  loading=true;
  try{
    const response=await fetch('/api/gallery',{cache:'no-store'});if(!response.ok)throw Error('HTTP '+response.status);
    const json=await response.json();
    const nextSignature=JSON.stringify(json);
    if(loaded&&nextSignature===signature)return;
    signature=nextSignature;loaded=true;
    items=Array.isArray(json.items)?json.items:[];
    apps=new Map((json.apps||[]).map(a=>[a.id,a]));
    $('numApps').textContent=fmt(apps.size);
    $('numPhotos').textContent=fmt(items.filter(m=>m.type==='photo').length);
    $('numVideos').textContent=fmt(items.filter(m=>m.type==='video').length);
    render();
    if(json.truncated||!json.roots?.photo||!json.roots?.video){
      const hint=make('p','sub',json.truncated?'Showing the first 12,000 captures.':'Some media paths are missing or inaccessible on this firmware.');
      library.prepend(hint);
    }
  }catch(err){if(loaded)return;library.replaceChildren();const error=make('div','empty error');error.append(make('strong','','Unable to load gallery'),make('p','',String(err)));library.append(error);}
  finally{loading=false;}
}
load();
setInterval(load,10000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)load();});
