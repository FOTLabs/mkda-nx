import {test} from 'node:test';
import assert from 'node:assert/strict';
import {safePath,ZipWriter,crc32,discFiles,uniqueFiles,validateGame} from '../core.mjs';
import {writeFile,mkdtemp} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
test('reject paths that could escape or alias SD files',()=>{
 for(const p of ['../a','/a','a/../b','a\\b','a//b','a/','C:a','a.','a '])assert.throws(()=>safePath(p));
 assert.equal(safePath('dvddata/level.mtx'),'dvddata/level.mtx');
 assert.throws(()=>uniqueFiles([{path:'a'},{path:'A'}]));
});
test('CRC32 standard vector',()=>assert.equal((crc32(new TextEncoder().encode('123456789'))^0xffffffff)>>>0,0xcbf43926));
test('ZIP records carry names, descriptors and CRC',async()=>{
 const chunks=[];const zip=new ZipWriter({write:async b=>chunks.push(b.slice())});
 await zip.add('switch/mkdax/game/default.xbe',new Blob(['test']));await zip.add('switch/mkdax/empty',new Blob([]));await zip.finish();
 const data=Buffer.concat(chunks);assert.equal(data.readUInt32LE(0),0x04034b50);assert.equal(data.readUInt32LE(data.length-22),0x06054b50);assert.equal(data.readUInt16LE(data.length-12),2);
 const dir=await mkdtemp(join(tmpdir(),'buffy-zip-'));await writeFile(join(dir,'test.zip'),data);console.log('ZIP_FIXTURE='+join(dir,'test.zip'));
});
test('reject invalid disc and unsupported game',async()=>{
 await assert.rejects(()=>discFiles(new Blob([new Uint8Array(70000)])),/Not a supported/);
 await assert.rejects(()=>validateGame([],{xbeSha256:'0'}),/Unsupported/);
});

function syntheticDisc(){
 const data=new Uint8Array(38*2048),v=new DataView(data.buffer),desc=32*2048,table=34*2048;
 data.set(new TextEncoder().encode('MICROSOFT*XBOX*MEDIA'),desc);v.setUint32(desc+20,34,true);v.setUint32(desc+24,64,true);
 const name=new TextEncoder().encode('default.xbe');v.setUint32(table+4,35,true);v.setUint32(table+8,4,true);data[table+13]=name.length;data.set(name,table+14);data.set([88,66,69,72],35*2048);
 return data;
}
test('reads a minimal Xbox filesystem and bounds corrupted records',async()=>{
 let data=syntheticDisc();const files=await discFiles(new Blob([data]));assert.equal(files.length,1);assert.equal(files[0].path,'default.xbe');assert.equal(await files[0].blob.text(),'XBEH');
 data=syntheticDisc();new DataView(data.buffer).setUint32(34*2048+4,999999,true);await assert.rejects(()=>discFiles(new Blob([data])),/outside image/);
 data=syntheticDisc();data[34*2048+13]=0;await assert.rejects(()=>discFiles(new Blob([data])),/Invalid file name/);
});
