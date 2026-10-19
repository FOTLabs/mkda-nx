// Optional integration check using a user-owned disc; no retail fixture in Git.
import {open,stat,readFile} from 'node:fs/promises';
import {discFiles,validateGame} from '../core.mjs';
const path=process.argv[2];if(!path)throw Error('Pass your local ISO path');
const file=await open(path,'r');
// Node 24 openAsBlob on this Windows host truncates >4 GiB file sizes.
// Use explicit 64-bit filesystem offsets to exercise the browser parser.
function range(start,end){return {size:end-start,slice(a,b){return range(start+a,Math.min(start+b,end));},async arrayBuffer(){const data=Buffer.alloc(end-start);let done=0;while(done<data.length){const {bytesRead}=await file.read(data,done,data.length-done,start+done);if(!bytesRead)throw Error('Unexpected EOF');done+=bytesRead;}return data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength);}};}
try{
 const blob=range(0,(await stat(path)).size),files=await discFiles(blob);
 const config=JSON.parse(await readFile(new URL('../game.json',import.meta.url),'utf8'));
 await validateGame(files,config);console.log('PASS real disc:',files.length,'files,',files.reduce((n,f)=>n+f.blob.size,0),'bytes; executable SHA-256 matches');
}finally{await file.close();}
