# Changelog

What changed in each release of DigidreamSound, fixes first. Kept as fixes
land: anything not released yet is under **Unreleased**.

## Unreleased

## 0.2.0 -- 2026-10-14

The first public release. Rebuilt on 2026-10-15 (same version) with the
fixes below.

### Fixed

- Building from source found the engine's fixed-point header in the wrong
  place (`../engine/core/include` is where it lives).
- The emsdk hint names the engine's setup script, which installs it beside
  the repositories (`../emsdk`).

### Changed

- The fitted DOOM II bank and its fitting tools left the repository.
- A README for users; the maintainer material is in DEVELOPING.md.
- MIT licence.
