# Gea plugin architecture ratchets

`ratchet-gea-rendered-cpp-source.mjs` prevents growth of the plugin's
post-rendered C++ consumers while those paths are being removed.

The scanner builds the package TypeScript program, resolves local call targets
through compiler symbols, and follows two separate reachability sets:

- functions reachable from `transformGeneratedSources`;
- exported rendered-text roots that are dormant from that hook.

Rows use repository-relative files, qualified owner symbols, category,
reachability, and normalized AST fingerprints. Source offsets, line numbers,
comments, whitespace, and local identifier spelling are not identity. String
and regular-expression contents remain part of the fingerprint so changing a
rewrite pattern is a removal plus a forbidden addition.

Run the normal check with:

```sh
npm run architecture
```

The complete canonical report is written to
`scripts/architecture/.artifacts/gea-rendered-cpp-ratchet-current.json`; the
console prints only counts, size, and SHA-256. The artifact directory is
ignored by Git.

Current rows must be a subset of
`gea-rendered-cpp-ratchet-baseline.json`. Deletions pass without baseline
updates. To compact the baseline after removals:

```sh
node scripts/architecture/ratchet-gea-rendered-cpp-source.mjs --prune-baseline
```

Pruning refuses additions and ceiling violations. Do not reinitialize or grow
the baseline to accommodate new post-render consumers. When the terminal hook
and all rendered-text consumers are removed, retain the scanner as a zero-ban
guard and set every ceiling to zero.
