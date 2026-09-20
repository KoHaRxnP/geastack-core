import type { GeaElement, GeaJsxElement } from '../index'

/**
 * `RootElement` is constrained and defaulted exactly as this package's own
 * `index.d.ts` declares it. The two used to disagree -- the declaration said
 * `extends GeaElement = GeaElement`, this implementation said `= unknown` --
 * and since the embedded/geatsc pipeline aliases `@geastack/core` to
 * `runtime.ts` rather than the declaration file, it was this weaker default
 * that every compiled app actually saw. `unknown` absorbs the union in
 * `RootElement | null`, so `el` typed as plain `unknown`: a value the compiler
 * must box, for no reason other than the two spellings having drifted apart.
 * `runtime-surface.ts` re-exports these element types from `./index` for the
 * same reason and says so in the same terms.
 */
export class Component<RootElement extends GeaElement = GeaElement, Props = void> {
  readonly el: RootElement | null = null
  // Kept in lockstep with `index.d.ts`, which carries the reasoning. This is
  // the copy that matters for a compiled app -- the pipeline aliases
  // `@geastack/core` to `runtime.ts`, so this signature, not the declaration
  // file's, is the one geatsc derives carriers from. `unknown[]`/`unknown` here
  // meant a boxed argument array and a boxed result on every component.
  template(_props?: Props): GeaJsxElement | null {
    return null
  }
  // NB: deliberately NO `render` here. `render` is synthesised onto compiled
  // components by the embedded compiler. Declaring it on the base looks tidier
  // (it would let mount() drop its cast) but it BREAKS embedded codegen: once
  // geatsc sees `render` on the statically-typed `Component`, mount()'s
  // `new component()` constructs the base `Component` by value instead of
  // dynamically constructing the real runtime class, so the app never mounts.
  // Verified on esp32-s3-m5stack-papers3. Keep it off the base.
}

export class Store {}

// EXPERIMENTAL (component-as-store): the vite-plugin rewrites
// `extends ReactiveComponent` to a lean typed class before codegen (matched by
// source identifier). This marker only needs to exist so imports resolve during
// bundling / DOM execution.
export class ReactiveComponent<
  RootElement extends GeaElement = GeaElement,
  Props = void
> extends Component<RootElement, Props> {}
