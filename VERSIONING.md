# Versioning

- `VERSION` is the current project version for local builds and releases.
- Record user-visible changes under `CHANGELOG.md` before changing `VERSION`.
- Use Semantic Versioning: MAJOR for incompatible changes, MINOR for compatible features, PATCH for fixes.
- A `baseline/vX.Y.Z` Git tag marks a verified local source snapshot; it is not a public release unless a release artifact and acceptance record exist.
- Keep secrets, runtime state, generated output, caches, backups, and private source data outside Git.
- Roll back by checking out a known commit or `baseline/vX.Y.Z`; do not delete runtime data as part of a source rollback.
