# Contributing to Domdly

Thanks for your interest in Domdly! Bug reports, ideas and pull requests are all welcome.

## Before you start

- For anything larger than a small fix, please open an issue first so we can agree on the approach.
- Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The most important rules are:
  - **No third-party code or libraries.** Only what the operating system provides and the C standard library. Do not copy code from other projects, even small snippets, and do not add assets (icons, fonts, images) you did not create yourself.
  - **`src/core/` stays portable C99** with no platform headers and no I/O.

## Building and testing

On Windows run `build.bat`; on Linux or macOS run `./build.sh`. Both run the parser tests first. Every change to the parser or the renderers should come with a test case in `tests/test_markdown.c`.

## Code style

- C99 in `src/core/`, C11 is fine in platform code.
- 4 spaces, no tabs, braces on the same line except for function bodies.
- Keep functions small and names descriptive; comment *why*, not *what*.
- New source files start with the same license header as the existing ones (`SPDX-License-Identifier: GPL-3.0-only`).
- The build must stay warning-free (`/W4` on MSVC, `-Wall -Wextra -Wpedantic` on GCC and Clang).

## Pull requests

- One topic per pull request, with a clear description of what changes for the user.
- Include screenshots for visual changes, in both the light and the dark theme.
- Update `CHANGELOG.md` under *Unreleased*.

## Versions and releases

Domdly follows Semantic Versioning; the rules are at the top of [CHANGELOG.md](CHANGELOG.md). In short: fixes bump the patch number, new features the minor number, breaking changes the major number. To release:

1. Move the *Unreleased* entries in `CHANGELOG.md` under the new version and date.
2. Update the numbers in `src/version.h` and the `version` attribute in `src/platform/win32/Domdly.manifest`.
3. Commit, then tag `vX.Y.Z` and push the tag. The CI builds `Domdly.exe` and attaches it to the GitHub release.

The repository cover (`docs/images/cover.png`) is drawn by `tools/mkcover.c`; run `build.bat cover` after changing it. On GitHub, upload the same file under *Settings > General > Social preview*.

## License of contributions

By submitting a contribution you agree that it is your own original work and that it is licensed under the [GNU GPL version 3](LICENSE) (`GPL-3.0-only`), the same license as the rest of the project.
