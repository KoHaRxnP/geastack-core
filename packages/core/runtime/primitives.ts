import { Component } from './compiler'

/**
 * Whatever the `document` in scope hands back for an id, minus the miss.
 *
 * Not a named element type. This file is compiled under two different worlds:
 * a project that includes lib.dom (`document` is lib.dom's, answering
 * `HTMLElement`) and one that does not (`document` is the framework's,
 * answering the framework's `Element`). Naming either one is wrong under the
 * other, and naming `HTMLElement` in particular made the DOM library a build
 * requirement of every pipeline that compiles this file.
 */
type MountRoot = NonNullable<ReturnType<typeof document.getElementById>>
// Typed as a component constructor — no `any`. geatsc lowers a `new () => T`
// parameter to an abbreviated-template forwarding reference, so this function is
// instantiated per call site and `new component()` constructs the concrete app
// class natively (no boxing, no dynamic construct path).
//
// The parameter is named `component`, NOT `App`: geatsc emits user classes into
// flat global C++ scope, so a parameter named after a common root class would
// shadow it at this construction site.
export function mount<RootComponent extends Component>(component: new () => RootComponent): void {
  const root = document.getElementById('app')
  if (!root) throw new Error('gea-embedded mount root #app was not found')
  const instance = new component()
  // `render` is synthesised onto compiled components and is not on the base
  // `Component` type (see compiler.ts for why it must not be), so reach it
  // through a local structural cast rather than widening the parameter to `any`.
  //
  // The cast goes through `unknown` because the two types genuinely do not
  // overlap: the base declares no `render` at all, so a direct assertion is one
  // TypeScript rightly rejects. It was only ever accepted while this file went
  // unchecked -- a pipeline resolving `@geastack/core` to `index.d.ts` never
  // compiles it.
  //
  // The parameter is spelled off `root` itself rather than by name. It used to
  // read `HTMLElement` -- a lib.dom global this framework never declares -- and
  // that one word made the DOM library a build requirement of every pipeline
  // that compiles this file, which then merged lib.dom's `Window`/`globalThis`
  // into apps that had declared their own. Naming the framework's `Element`
  // instead is no better in the other direction: under a project that DOES
  // include lib.dom, `document` is lib.dom's and its `getElementById` answers
  // `HTMLElement`, which is not assignable to it. `typeof root` is neither: it
  // is whatever `document` in scope actually answers, which is the only correct
  // answer in both. Read off the METHOD rather than off `root`: `typeof root`
  // inside this assertion really is circular, because narrowing makes `root`'s
  // type here depend on the assertion being resolved first.
  ;(instance as unknown as { render(root: MountRoot, depth: number): void }).render(root, 1)
}
