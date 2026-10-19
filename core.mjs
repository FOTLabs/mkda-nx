// Original browser-local Xbox disc reader and streaming ZIP writer (MIT).
export function safePath(path) {
  if (!path || path.startsWith('/') || path.includes(String.fromCharCode(92)) || path.includes(':') || [...path].some(c => c.charCodeAt(0) < 32) || path.split('/').some(p => !p || p === '.' || p === '..' || /[. ]$/.test(p))) throw Error('Unsafe file path: ' + path);
  return path;
}
const u32=(b,p)=>new DataView(b.buffer,b.byteOffset,b.byteLength).getUint32(p,true);
const u16=(b,p)=>new DataView(b.buffer,b.byteOffset,b.byteLength).getUint16(p,true);
export async function discFiles(blob) {
  const magic='MICROSOFT*XBOX*MEDIA';let base=-1,desc;
  for(const candidate of [0,0xfd90,0x30600,0xfd90000,0x18300000]) {
    const bytes=new Uint8Array(await blob.slice(candidate+65536,candidate+67584).arrayBuffer());
    if(new TextDecoder().decode(bytes.slice(0,20))===magic){base=candidate;desc=bytes;break;}
  }
  if(base<0)throw Error('Not a supported Xbox disc image');
  const files=[],dirs=new Set();
  async function directory(sector,size,prefix,depth) {
    if(depth>64 || size>64*1024*1024 || dirs.has(sector))throw Error('Invalid or cyclic disc directory');
    dirs.add(sector);const start=base+sector*2048;
    if(start+size>blob.size)throw Error('Directory is outside image');
    const table=new Uint8Array(await blob.slice(start,start+size).arrayBuffer());
    const pending=[0],seen=new Set();
    while(pending.length){
      const at=pending.pop();if(seen.has(at))throw Error('Cyclic directory tree');seen.add(at);
      if(at+14>table.length)throw Error('Truncated directory entry');
      const len=table[at+13];if(!len || at+14+len>table.length)throw Error('Invalid file name');
      const left=u16(table,at)*4,right=u16(table,at+2)*4;
      if(left)pending.push(left);if(right)pending.push(right);
      const name=new TextDecoder('windows-1252').decode(table.slice(at+14,at+14+len));
      const path=safePath(prefix+name),fileSector=u32(table,at+4),fileSize=u32(table,at+8);
      if(table[at+12]&16)await directory(fileSector,fileSize,path+'/',depth+1);
      else {
        const offset=base+fileSector*2048;if(offset+fileSize>blob.size)throw Error('File is outside image');
        files.push({path,blob:blob.slice(offset,offset+fileSize)});
        if(files.length>100000)throw Error('Too many disc files');
      }
    }
  }
  await directory(u32(desc,20),u32(desc,24),'',0);return uniqueFiles(files);
}
export function uniqueFiles(files) {
  const seen=new Set();for(const f of files){safePath(f.path);const key=f.path.toLowerCase();if(seen.has(key))throw Error('Duplicate path: '+f.path);seen.add(key);}return files;
}
export function folderFiles(list) {
  const files=Array.from(list).map(blob=>({path:blob.webkitRelativePath||blob.name,blob}));
  const executables=files.filter(f=>/(^|\/)default\.xbe$/i.test(f.path));
  if(executables.length!==1)throw Error('Choose the extracted game folder containing one default.xbe');
  const prefix=executables[0].path.slice(0,-'default.xbe'.length);
  return uniqueFiles(files.filter(f=>f.path.startsWith(prefix)).map(f=>({...f,path:f.path.slice(prefix.length)})));
}
export async function sha256(blob) {
  const hash=await crypto.subtle.digest('SHA-256',await blob.arrayBuffer());
  return Array.from(new Uint8Array(hash),v=>v.toString(16).padStart(2,'0')).join('');
}
export async function validateGame(files,config) {
  const xbe=files.find(f=>f.path.toLowerCase()==='default.xbe');
  if(!xbe || xbe.blob.size>32*1024*1024 || await sha256(xbe.blob)!==config.xbeSha256)throw Error('Unsupported game revision. Use your tested European Xbox copy.');
  return files.map(f=>f===xbe?{...f,path:'default.xbe'}:f);
}
const crcTable=Uint32Array.from({length:256},(_,n)=>{for(let i=0;i<8;i++)n=(n&1)?0xedb88320^(n>>>1):n>>>1;return n>>>0;});
export function crc32(bytes,crc=0xffffffff){for(const b of bytes)crc=crcTable[(crc^b)&255]^(crc>>>8);return crc>>>0;}
function header(size,signature){const b=new Uint8Array(size);new DataView(b.buffer).setUint32(0,signature,true);return b;}
function set16(b,p,v){new DataView(b.buffer).setUint16(p,v,true);}
function set32(b,p,v){new DataView(b.buffer).setUint32(p,v,true);}
export class ZipWriter {
  constructor(sink){this.sink=sink;this.offset=0;this.entries=[];this.names=new Set();}
  async write(bytes){if(this.offset+bytes.length>0xffffffff)throw Error('ZIP exceeds 4 GiB; use manual SD installation');await this.sink.write(bytes);this.offset+=bytes.length;}
  async add(path,blob){
    safePath(path);if(this.names.has(path.toLowerCase()))throw Error('Duplicate ZIP path');this.names.add(path.toLowerCase());
    const name=new TextEncoder().encode(path);if(name.length>65535 || blob.size>0xffffffff || this.entries.length>=65535)throw Error('ZIP32 size limit');
    const offset=this.offset,local=header(30,0x04034b50);set16(local,4,20);set16(local,6,0x808);set16(local,12,33);set16(local,26,name.length);
    await this.write(local);await this.write(name);let crc=0xffffffff;
    for(let at=0;at<blob.size;at+=1024*1024){const bytes=new Uint8Array(await blob.slice(at,at+1024*1024).arrayBuffer());crc=crc32(bytes,crc);await this.write(bytes);}
    crc=(crc^0xffffffff)>>>0;const descriptor=header(16,0x08074b50);set32(descriptor,4,crc);set32(descriptor,8,blob.size);set32(descriptor,12,blob.size);await this.write(descriptor);
    this.entries.push({name,offset,crc,size:blob.size});
  }
  async finish(){
    const start=this.offset;
    for(const e of this.entries){const h=header(46,0x02014b50);set16(h,4,20);set16(h,6,20);set16(h,8,0x808);set16(h,14,33);set32(h,16,e.crc);set32(h,20,e.size);set32(h,24,e.size);set16(h,28,e.name.length);set32(h,42,e.offset);await this.write(h);await this.write(e.name);}
    const end=header(22,0x06054b50);set16(end,8,this.entries.length);set16(end,10,this.entries.length);set32(end,12,this.offset-start);set32(end,16,start);await this.write(end);
  }
}
