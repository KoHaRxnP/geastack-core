// Installed JSDOM oracle for argument conversion order; no native/UI execution.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import path from 'node:path';
if (!process.env.GEA_JSDOM_PACKAGE) throw new Error('GEA_JSDOM_PACKAGE must name the actual installed package');
const require = createRequire(path.join(path.resolve(process.env.GEA_JSDOM_PACKAGE), 'package.json'));
const { JSDOM } = require('./lib/api.js');
for (const [args, expected] of [
  [['b','f'], 'B,A'], [['a','f','a'], 'B,A'],
  [['f','a','f'], 'B,A'], [['f','x','f'], 'A,B,I'],
]) {
  const d = new JSDOM().window.document;
  const p=d.createElement('div'), f=d.createDocumentFragment();
  const a=d.createElement('a'), b=d.createElement('b'), x=d.createElement('i');
  f.append(a,b);
  const nodes={a,b,x,f};
  p.replaceChildren(...args.map(key => nodes[key]));
  const actual=[...p.childNodes].map(node => node.tagName).join(',');
  assert.equal(actual,expected);
  console.log(args.join(',')+' -> '+actual);
}
