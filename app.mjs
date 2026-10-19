import {discFiles,folderFiles,validateGame,sha256,ZipWriter} from './core.mjs';
const $=id=>document.getElementById(id),status=text=>{$('status').textContent=text;};
$('folder').onchange=()=>{$('iso').value='';};$('iso').onchange=()=>{$('folder').value='';};
function formatChanged(){const iso=$('format-iso').checked;$('iso-choice').hidden=!iso;$('folder-choice').hidden=iso;if(iso)$('folder').value='';else $('iso').value='';}
$('format-iso').onchange=formatChanged;$('format-folder').onchange=formatChanged;
async function json(path){const response=await fetch(path);if(!response.ok)throw Error('Cannot load '+path);return response.json();}
async function getNro(manifest){
 if(!manifest.nroUrl || !manifest.nroSha256)throw Error('No published build is configured yet.');
 status('Downloading and verifying release '+manifest.version+'…');
 let response;try{response=await fetch(manifest.nroUrl,{headers:{Accept:'application/octet-stream'}});}catch{throw Error('Browser blocked the download. Try again in desktop Chrome or Edge.');}
 if(!response.ok)throw Error('Download failed. Please try again.');
 const blob=await response.blob();if(await sha256(blob)!==manifest.nroSha256)throw Error('Release NRO checksum mismatch');return blob;
}
$('create').onclick=async()=>{
 let destination,zip;
 try{
  if(!$('folder').files.length&&!$('iso').files.length)throw Error('Choose your game folder or ISO.');
  if(!window.showSaveFilePicker)throw Error('Use desktop Chrome or Edge for streaming ZIP output, or follow the manual SD instructions in the README.');
  // Open the picker while this click still has browser user activation.
  destination=await showSaveFilePicker({suggestedName:'mkda-nx.zip',types:[{description:'ZIP archive',accept:{'application/zip':['.zip']}}]});
  $('create').disabled=true;status('Checking your game…');
  const [config,manifest]=await Promise.all([json('./game.json'),json('./release.json')]);
  let files=$('iso').files.length?await discFiles($('iso').files[0]):folderFiles($('folder').files);
  files=await validateGame(files,config);const nro=await getNro(manifest);
  const license=await fetch('./LICENSES.txt');if(!license.ok)throw Error('Licence notices unavailable');
  const notices=await license.blob();
  const settings=await fetch('./mkdax_env.txt');if(!settings.ok)throw Error('Default settings unavailable');
  const defaults=await settings.blob();
  const total=files.reduce((n,f)=>n+f.blob.size,0)+nro.size+notices.size+files.length*1024;
  if(total>0xffffffff)throw Error('This disc exceeds the 4 GiB ZIP limit; use manual SD installation.');
  zip=await destination.createWritable();const writer=new ZipWriter(zip),prefix='switch/'+config.sdFolder+'/';
  await writer.add(prefix+'mkdax.nro',nro);await writer.add(prefix+'LICENSES.txt',notices);
  await writer.add(prefix+'mkdax_env.txt',defaults);
  let done=0;for(const f of files){status('Packing '+(++done)+' / '+files.length+': '+f.path);await writer.add(prefix+'game/'+f.path,f.blob);}
  await writer.finish();await zip.close();zip=null;status('Done. Extract mkda-nx.zip into your SD card root.');
 }catch(error){if(zip)await zip.abort().catch(()=>{});status(error.name==='AbortError'?'Cancelled.':error.message);}
 finally{$('create').disabled=false;}
};

