import {test} from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {spawnSync} from 'node:child_process'
import {snapshot,verifySnapshot,pipeline,validateSelection,validateGeneratedOutput} from './native-contract-pipeline.mjs'
test('source provenance refuses changed or missing bytes, independent of timestamp',()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'gea-contract-provenance-'))
 try{
  const file=path.join(directory,'input');fs.writeFileSync(file,'one')
  const expected=snapshot([file]),time=fs.statSync(file).mtime
  verifySnapshot(expected)
  fs.writeFileSync(file,'two');fs.utimesSync(file,time,time)
  assert.throws(()=>verifySnapshot(expected),/provenance changed/)
  fs.rmSync(file);assert.throws(()=>verifySnapshot(expected),/provenance changed/)
 }finally{fs.rmSync(directory,{recursive:true,force:true})}
})
test('runner argument refusal occurs before any compiler or native producer',()=>{
 for(const name of ['native-remove-child','native-dom-structural','native-insert-before']){
  const result=spawnSync('bash',[new URL(`./run-${name}-contract.sh`,import.meta.url).pathname,'--invalid'],{encoding:'utf8'})
  assert.equal(result.status,2);assert.match(result.stderr,/usage:/)
 }
 assert.throws(()=>pipeline('invented','prepare'),/Invalid/)
})
test('normal and prepare-only runners admit their mode but fail closed without compiler authority',()=>{
 const env={...process.env};delete env.GEA_GEATSC_BIN;delete env.GEA_NATIVE_TEST_ENGINE_ROOT;delete env.GEA_NATIVE_TEST_ELEMENTS_ROOT
 for(const name of ['native-remove-child','native-dom-structural','native-insert-before'])for(const args of [[],['--prepare-only']]){
  const result=spawnSync('bash',[new URL(`./run-${name}-contract.sh`,import.meta.url).pathname,...args],{encoding:'utf8',env})
  assert.notEqual(result.status,0)
  assert.match(result.stderr,/GEA_GEATSC_BIN must name/)
  assert.doesNotMatch(result.stderr,/usage:|unbound variable/)
 }
})
test('all emitted runtime siblings and builtins must match the explicit provider',()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'gea-runtime-provenance-'))
 try{
  const runtime=path.join(directory,'provider'),out=path.join(directory,'output');fs.mkdirSync(runtime);fs.mkdirSync(out)
  for(const name of ['gea_runtime.h','gea_dynamic_proxy.h','gea_runtime_builtins.cpp']){
   fs.writeFileSync(path.join(runtime,name),name);fs.writeFileSync(path.join(out,name),name)
  }
  validateGeneratedOutput(runtime,out)
  for(const name of ['gea_dynamic_proxy.h','gea_runtime_builtins.cpp']){
   fs.writeFileSync(path.join(out,name),'changed')
   assert.throws(()=>validateGeneratedOutput(runtime,out),new RegExp(name.replaceAll('.','\\.')))
   fs.writeFileSync(path.join(out,name),name)
  }
  const before=snapshot([path.join(runtime,'gea_runtime_builtins.cpp')]);fs.writeFileSync(path.join(runtime,'gea_runtime_builtins.cpp'),'provider changed')
  assert.throws(()=>verifySnapshot(before),/provenance changed/)
 }finally{fs.rmSync(directory,{recursive:true,force:true})}
})
test('asset-free contracts refuse each extra generated source and engine/elements overrides',()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'gea-assets-provenance-'))
 try{
  for(const name of ['gea_runtime.h','gea_runtime_builtins.cpp'])fs.writeFileSync(path.join(directory,name),name)
  for(const name of ['gea_embedded_font_generated.cpp','gea_embedded_assets_generated.cpp']){
   fs.writeFileSync(path.join(directory,name),'unexpected consumed source')
   assert.throws(()=>validateGeneratedOutput(directory,directory),/refuses optional generated source/)
   fs.rmSync(path.join(directory,name))
  }
  validateSelection({})
  for(const name of ['GEA_NATIVE_TEST_ENGINE_ROOT','GEA_NATIVE_TEST_ELEMENTS_ROOT'])assert.throws(()=>validateSelection({[name]:'/explicit/alternate'}),new RegExp(name))
 }finally{fs.rmSync(directory,{recursive:true,force:true})}
})
