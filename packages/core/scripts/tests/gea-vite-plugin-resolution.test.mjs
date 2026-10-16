import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { resolveAppGeaVitePlugin } from '../gea-vite-plugin-resolution.mjs'

test('declared app plugin honors its real package/version and symlink; undeclared alone inherits Core', () => {
  const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'gea-plugin-resolution-')))
  try {
    const app = path.join(root,'app'), core = path.join(root,'core')
    for (const p of [app,core]) { fs.mkdirSync(p); fs.writeFileSync(path.join(p,'package.json'),'{}') }
    const install = (parent, version) => {
      const pkg=path.join(parent,'node_modules/@geajs/vite-plugin'); fs.mkdirSync(pkg,{recursive:true})
      fs.writeFileSync(path.join(pkg,'package.json'),JSON.stringify({name:'@geajs/vite-plugin',version,type:'module',exports:{'.':{import:'./index.mjs'}}}))
      fs.writeFileSync(path.join(pkg,'index.mjs'),`export const version=${JSON.stringify(version)}`)
      return pkg
    }
    const legacy=install(core,'1.4.1')
    assert.equal(resolveAppGeaVitePlugin(app,path.join(core,'package.json')),path.join(legacy,'index.mjs'))
    fs.writeFileSync(path.join(app,'package.json'),JSON.stringify({devDependencies:{'@geajs/vite-plugin':'file:../source'}}))
    assert.throws(()=>resolveAppGeaVitePlugin(app,path.join(core,'package.json')),/App declares/)
    const source=install(root,'1.5.0')
    fs.mkdirSync(path.join(app,'node_modules/@geajs'),{recursive:true})
    fs.symlinkSync(source,path.join(app,'node_modules/@geajs/vite-plugin'))
    assert.equal(resolveAppGeaVitePlugin(app,path.join(core,'package.json')),path.join(source,'index.mjs'))
    fs.unlinkSync(path.join(source,'index.mjs'))
    assert.throws(()=>resolveAppGeaVitePlugin(app,path.join(core,'package.json')),/App declares/)
  } finally { fs.rmSync(root,{recursive:true,force:true}) }
})

test('declared app plugin hoisted to its npm workspace root resolves; an unlisted ancestor install does not', () => {
  const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'gea-plugin-workspace-')))
  try {
    const app = path.join(root,'apps/clock'), stray = path.join(root,'tools/stray'), core = path.join(root,'core')
    for (const p of [app,stray,core]) fs.mkdirSync(p,{recursive:true})
    for (const p of [app,stray]) fs.writeFileSync(path.join(p,'package.json'),JSON.stringify({devDependencies:{'@geajs/vite-plugin':'^1.4.2'}}))
    fs.writeFileSync(path.join(core,'package.json'),'{}')
    fs.writeFileSync(path.join(root,'package.json'),JSON.stringify({private:true,workspaces:['apps/*']}))
    const pkg=path.join(root,'node_modules/@geajs/vite-plugin'); fs.mkdirSync(pkg,{recursive:true})
    fs.writeFileSync(path.join(pkg,'package.json'),JSON.stringify({name:'@geajs/vite-plugin',version:'1.4.2',type:'module',exports:{'.':{import:'./index.mjs'}}}))
    fs.writeFileSync(path.join(pkg,'index.mjs'),'export const version="1.4.2"')
    assert.equal(resolveAppGeaVitePlugin(app,path.join(core,'package.json')),path.join(pkg,'index.mjs'))
    assert.throws(()=>resolveAppGeaVitePlugin(stray,path.join(core,'package.json')),/App declares/)
  } finally { fs.rmSync(root,{recursive:true,force:true}) }
})
