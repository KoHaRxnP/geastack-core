import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import {fileURLToPath} from 'node:url'
import {spawnSync} from 'node:child_process'

const testDir=path.dirname(fileURLToPath(import.meta.url))
const root=path.resolve(testDir,'../../..')
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
const files=(directory,predicate)=>fs.readdirSync(directory,{withFileTypes:true}).flatMap(entry=>{
 const file=path.join(directory,entry.name)
 if(['node_modules','.build','dist'].includes(entry.name))return []
 if(entry.isSymbolicLink())throw Error(`Refuse symbolic input: ${file}`)
 return entry.isDirectory() ? files(file,predicate) : entry.isFile()&&predicate(file)?[file]:[]
})
export function snapshot(paths){return Object.fromEntries([...new Set(paths)].sort().map(file=>[file,hash(file)]))}
export function verifySnapshot(expected){
 for(const [file,digest] of Object.entries(expected))if(!fs.existsSync(file)||hash(file)!==digest)throw Error(`Native contract provenance changed: ${file}`)
}
export function validateSelection(env){
 for(const name of ['GEA_NATIVE_TEST_ENGINE_ROOT','GEA_NATIVE_TEST_ELEMENTS_ROOT'])if(env[name])throw Error(`Native DOM contracts refuse ${name} overrides`)
}
export function validateGeneratedOutput(runtime,out){
 // Normal cli-emit copies every top-level runtime .h and this .cpp. A matching
 // primary header alone cannot authenticate sibling implementations.
 const names=fs.readdirSync(runtime).filter(name=>name.endsWith('.h'))
 if(!names.includes('gea_runtime.h'))throw Error('Explicit runtime provider has no gea_runtime.h')
 names.push('gea_runtime_builtins.cpp')
 for(const name of names)if(!fs.existsSync(path.join(out,name))||hash(path.join(out,name))!==hash(path.join(runtime,name)))throw Error(`Emitted runtime differs from explicit provider: ${name}`)
 for(const name of ['gea_embedded_font_generated.cpp','gea_embedded_assets_generated.cpp'])if(fs.existsSync(path.join(out,name)))throw Error(`Asset-free native DOM contract refuses optional generated source: ${name}`)
}
function run(command,args){
 const result=spawnSync(command,args,{stdio:'inherit'})
 if(result.error)throw result.error
 if(result.status!==0)throw Error(`${command} failed (${result.status??result.signal})`)
}
export function pipeline(name,mode){
 if(!['native-remove-child','native-dom-structural','native-insert-before'].includes(name)||!['prepare','check'].includes(mode))throw Error('Invalid native contract pipeline arguments')
 const stem=`${name}-contract`,out=path.join(root,'dist',stem)
 const stamp=path.join(out,'contract-provenance.json')
 validateSelection(process.env)
 const compiler=process.env.GEA_GEATSC_BIN
 if(!compiler||!path.isAbsolute(compiler)||!fs.statSync(compiler).isFile())throw Error('GEA_GEATSC_BIN must name the actual absolute installed/shared compiler CLI file')
 const entry=path.join(testDir,`${stem}.ts`),project=path.join(testDir,`tsconfig-${name}.json`)
 const plugin=path.join(root,'packages/geatsc-plugin-gea/dist/index.js')
 const oracle=path.join(testDir,`${name}-oracle.mjs`)
 const runtime=process.env.GEA_COMPILER_RUNTIME
 if(!runtime||!path.isAbsolute(runtime)||!fs.statSync(path.join(runtime,'gea_runtime.h')).isFile())throw Error('GEA_COMPILER_RUNTIME must name the actual absolute compiler runtime header directory')
 const inputs=()=>snapshot([compiler,entry,project,oracle,fileURLToPath(import.meta.url),
  path.join(testDir,`run-${stem}.sh`),path.join(testDir,`test_${stem.replaceAll('-','_')}.cpp`),path.join(testDir,'native-test-common.sh'),
  ...files(runtime,file=>/\.(?:h|hpp|cpp)$/.test(file)),
  ...files(path.dirname(compiler),file=>/\.(?:js|json|h)$/.test(file)),
  ...files(path.dirname(plugin),file=>/\.(?:js|json)$/.test(file)),
  ...['native_host_backends.cpp','native_test_host.cpp','native_camera_bridge.cpp'].map(name=>path.join(testDir,name)),
  ...['core','engine','host','elements'].flatMap(name=>files(path.join(root,'packages',name),file=>/\.(?:ts|json|h|hpp|cpp|c|inc)$/.test(file)&&!file.startsWith(testDir+path.sep)))])
 const outputs=()=>{
  const manifest=path.join(out,'geatsc-sources.txt')
  if(!fs.existsSync(manifest)||!fs.readFileSync(manifest,'utf8').trim())throw Error('Actual normal CLI source manifest required')
  const sources=fs.readFileSync(manifest,'utf8').trim().split(/\r?\n/)
  for(const file of sources)if(!path.isAbsolute(file)||!file.startsWith(out+path.sep)||!fs.statSync(file).isFile())throw Error(`Invalid normal CLI source: ${file}`)
  if(fs.existsSync(path.join(out,'program.cpp')))throw Error('Refuse fallback program.cpp in typed contract output')
  validateGeneratedOutput(runtime,out)
  return snapshot([manifest,...sources,...files(out,file=>/\.(?:h|hpp|cpp|c|inc)$/.test(file))])
 }
 if(mode==='prepare'){
  if(!process.env.GEA_JSDOM_PACKAGE)throw Error('GEA_JSDOM_PACKAGE must name the actual installed JSDOM package')
  fs.mkdirSync(out,{recursive:true});fs.rmSync(stamp,{force:true})
  const before=inputs()
  run(process.execPath,[compiler,'compile',entry,'--project',project,'--out-dir',out,'--plugin',plugin])
  run(process.execPath,[oracle])
  verifySnapshot(before)
  fs.writeFileSync(stamp,JSON.stringify({version:1,inputs:before,outputs:outputs()},null,2)+'\n')
 }else{
  const saved=JSON.parse(fs.readFileSync(stamp,'utf8'))
  if(saved.version!==1||JSON.stringify(saved.inputs)!==JSON.stringify(inputs())||JSON.stringify(saved.outputs)!==JSON.stringify(outputs()))throw Error('Native contract provenance differs; run the normal noargs pipeline to regenerate')
  verifySnapshot(saved.inputs);verifySnapshot(saved.outputs)
 }
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url))pipeline(process.argv[2],process.argv[3])
