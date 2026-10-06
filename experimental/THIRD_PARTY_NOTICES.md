# Source Provenance and Third-Party Notices

## License of this fork

WiVRn and this fork's modifications are distributed under GNU GPL v3 as
stated in the repository's `COPYING` file. Third-party components keep their
own licenses; this notice does not relicense or claim ownership of them.

## WiVRn

- Original project: https://github.com/WiVRn/WiVRn
- Custom base: https://github.com/stwbzy/WiVRn
- Base commit of this work copy: `7d7e0755`.
- License: GNU GPL v3, preserved in `../COPYING`.

The original Git history, source copyright headers, font license and SGSR
notices are retained. Public repository visibility does not replace these
licenses or transfer ownership of third-party work.

## Mocktail

- Project: https://github.com/komaruworld/mocktail
- Upstream branch: `vr`.
- Pinned revision: `9a1ccbb5cebea7bb578bfb198d2a3246734e6a40`.
- License: Apache-2.0, available in `LICENSE-APACHE-2.0` here and in the upstream
  checkout's `LICENSE`.

Mocktail authors provide the Android host, linker/runtime integration, payload
verification and native Roblox/OpenXR rendering bridge. This project does not
claim authorship of that implementation. Its pristine checkout and submodules
are fetched at build time; their copyright and license notices are preserved.
Their respective third-party licenses remain applicable.

Local modifications are implemented by `launcher/overlay.cmake` and
`launcher/tuning-overlay.cmake`, plus the accompanying launcher/helpers/tests.
The overlays generate modified copies of upstream sources in the ignored build
directory. They add isolated payload preparation, canary gating, home-only
recentering, pointer consistency, render presets, audio routing and diagnostic
counters. Generated/compiled upstream files are not committed here.

## Sober and Roblox

Sober is a separate project: https://github.com/vinegarhq/sober. Its public
repository is an issue tracker, not the source of this launcher. No Sober source
or executable is copied into this repository. There is no affiliation implied.

Roblox client binaries are proprietary and subject to their own terms. APKs,
ELFs, account data and authentication cookies are not redistributed. Build IDs,
hashes and reviewed compatibility metadata identify locally verified payloads;
they are not permission to redistribute or bypass signature checks.

## Native Build Dependencies

`launcher/native-dependencies.tsv` records download locations and integrity
hashes for locally extracted build dependencies. Dependency archives, libraries
and build outputs are ignored, not vendored. Their upstream licenses apply
when building or distributing binaries; this source upload is not a binary
release or a complete redistribution license review.
