# Releasing

A release is a tag. Pushing `v<major>.<minor>.<patch>` runs
[`release.yml`](../../.github/workflows/release.yml):

1. **CI** — the whole of `ci.yml` (x64 Debug and Release tests, native ARM64 tests).
2. **Pack** — fails unless the tag matches `EFFECTGRAPH_VERSION_STRING` in
   `Engine/Api/EffectGraphVersion.h`. Builds the packages with the **v143**
   toolset (`Pack.ps1 -PlatformToolset v143`): a static library links only
   with the toolset that built it or a newer one, so `EffectGraph.Engine` built
   with v143 works under VS 2022 and VS 2026, and one built with v145 would not
   link under VS 2022. Writes an SPDX SBOM and a signed build-provenance
   attestation for each `.nupkg` (`gh attestation verify <file> -R Spruill-1/EffectGraph`).
3. **Publish** — waits in the `release` environment for a reviewer's approval,
   then pushes `EffectGraph`, `EffectGraph.Native` and `EffectGraph.Engine` to
   nuget.org and creates the GitHub release with the packages and the SBOM.

A nuget.org version cannot be deleted, only unlisted, so a mistake means a new
patch version.

## Cutting a release

1. Bump `Engine/Api/EffectGraphVersion.h` and add the `CHANGELOG.md` section.
2. Merge to `main` with CI green.
3. `git tag v1.2.3 && git push origin v1.2.3`.
4. Approve the `publish` job when the pack job's artifacts look right.

## One-time setup

- **Environment:** Settings → Environments → `release`, with yourself as a
  required reviewer and deployments limited to tags `v*.*.*`.
- **nuget.org trusted publishing:** on nuget.org, *Trusted Publishing* → add a
  policy for owner `Spruill-1`, repository `EffectGraph`, workflow file
  `release.yml`, environment `release`. No API key is stored anywhere:
  `NuGet/login` trades the job's GitHub OIDC token for a key that lives for
  the job.
- **Variable:** Settings → Variables → `NUGET_USER` = the nuget.org account
  name that owns the policy.
