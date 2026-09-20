export * from './runtime-surface'

/**
 * The JSX tag set, restated on the *runtime* entrypoint so an app that
 * augments `@geastack/core` reaches it.
 *
 * The package answers `"."` with two files -- `types: ./index.d.ts` for an
 * editor, `default: ./runtime.ts` for anything that actually loads the
 * library, this file. A build pipeline that compiles the framework from
 * source (geatsc, and the vite plugin) resolves the specifier to THIS module,
 * so an app's `declare module '@geastack/core' { interface
 * GeaIntrinsicElements { 'glass-pane': ... } }` augments this module -- while
 * `JSX.IntrinsicElements` was wired, in `index.d.ts`, to *that* module's
 * interface. One package, two module identities, and the app's own tags
 * landing in the one the JSX namespace never reads: every use of a custom tag
 * is then `Property 'glass-pane' does not exist on type
 * 'JSX.IntrinsicElements'`, which is a fact about which file the specifier
 * resolved to and not about the app.
 *
 * Extending the declaration file's own interface rather than re-listing its
 * tags keeps `index.d.ts` the single authority for what the framework itself
 * ships; this adds only the seam an augmentation needs. The two global
 * `JSX.IntrinsicElements` declarations merge, so a program that resolved the
 * package the other way loses nothing.
 */
import type { GeaIntrinsicElements as FrameworkIntrinsicElements } from './index'

export interface GeaIntrinsicElements extends FrameworkIntrinsicElements {}

declare global {
  namespace JSX {
    interface IntrinsicElements extends GeaIntrinsicElements {}
  }
}
