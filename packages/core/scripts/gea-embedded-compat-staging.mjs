export const COMPAT_STAGING_IGNORED_DIRECTORIES = Object.freeze([
  'node_modules',
  'dist',
  'build',
  '.build',
  '.build-test',
  '.vite',
  '.scratch',
  '.test-tmp',
  'generated-output',
])

const ignoredDirectorySet = new Set(COMPAT_STAGING_IGNORED_DIRECTORIES)

export function shouldIgnoreCompatStagingDirectory(name) {
  return ignoredDirectorySet.has(name)
}
