# Changelog

All notable changes to Domdly are documented in this file. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Versioning

Domdly uses [Semantic Versioning](https://semver.org/) in the form `MAJOR.MINOR.PATCH`:

- **PATCH** (`1.0.0` to `1.0.1`): bug fixes only, no new behaviour.
- **MINOR** (`1.0.1` to `1.1.0`): new features that keep existing files, settings and shortcuts working.
- **MAJOR** (`1.4.2` to `2.0.0`): changes that break something users rely on.

Every change that reaches `main` is listed under *Unreleased* first. A release moves those entries under a new version heading, bumps the number in `src/version.h` (and the manifest), and is tagged `vMAJOR.MINOR.PATCH`; the CI then publishes `Domdly.exe` for that tag. Version `1.0.0` is the first public release.

## [Unreleased]

## [1.0.0] - 2026-11-26

First public version.

### Added

- Markdown parser written from scratch: CommonMark style blocks and inlines plus tables, task lists, strikethrough and bare URL autolinks.
- Single-page Windows interface with an **Edit** view (Markdown source) and a **Read** view (formatted document). `Ctrl+E` or the status bar switch toggles them and keeps the current block at the same place on screen.
- Editing in the Read view like a word processor: headings, paragraphs, lists, quotes, tables and code blocks are edited in their formatted form, with no Markdown marks on screen. Formatting shortcuts, Enter, Backspace, Tab between table cells, check boxes, paste (read as Markdown) and undo work there; every change is written back as Markdown and untouched blocks stay byte for byte as they were. Blocks with pictures or HTML open as Markdown source when clicked. Links open with Ctrl+click.
- Tabs: any number of documents, each with its own undo history; `Ctrl+T`, `Ctrl+W`, `Ctrl+Tab`, middle click to close. A second start (for example opening a file from Explorer) adds a tab to the running window, and open tabs are restored at the next start.
- Markdown writer in the core (`md_write.c`) that turns styled text back into Markdown, with round-trip tests.
- Modern window chrome drawn by Domdly: header with menu button, file name and version, status bar with position, word count and reading time. Hovering over a button shows its shortcut. The Windows 11 title bar is tinted to match and popup menus follow the theme.
- Three themes on one button in the bottom-right corner (`Ctrl+Shift+L`): Light (sun), Dark navy (half sun, half moon) and Night (moon, pure black); the menu can also follow the Windows setting.
- *Save as* button in the header (`Ctrl+Shift+S`): save the document anywhere as Markdown, or a copy as PDF, HTML or plain text. `Ctrl+Shift+P` exports a PDF directly, and `Domdly.exe file.md --pdf out.pdf` converts from the command line. PDF files are written by the Microsoft Print to PDF printer that ships with Windows.
- The version number in the header is a small link to the project on GitHub, and *Check for Updates on GitHub* in the menu asks GitHub for the latest release when the user picks it (never automatically, nothing is downloaded); a newer version offers to open its release page.
- Files sidebar on Windows and Linux (`Ctrl+Shift+B` or the button next to the menu): a tree of the folder that holds the open document, or of a pinned folder. Closed by default; whether it is open, its width and the pinned folder are remembered. Folders load when expanded, hidden files are left out, Markdown files open in a tab with one click and other files open with their usual program on a double click. The active document is highlighted and the tree refreshes when the window is activated or a file is saved.
- "Drop to open" overlay while a file is dragged over the window.
- Inline display of local PNG, JPEG, GIF and BMP images.
- Clickable links, including links between Markdown files and jumps to headings.
- Formatting commands, automatic list continuation, find and replace, recent files, zoom (also `Ctrl` + mouse wheel) and word wrap.
- Immediate mouse wheel scrolling in both views.
- Export to a standalone HTML page and copy as HTML.
- Self-installer: install to Program Files, Start menu shortcut, Installed apps entry, Open with registration and uninstall.
- `md2html` command line converter and a portable test suite for Windows, Linux and macOS.
- Linux version for Debian and derivatives, built on GTK 3: tabs, Edit and Read views (`Ctrl+E`), light, dark navy and night themes, Save as Markdown, PDF or HTML (`domdly file.md --pdf out.pdf` too), drag and drop, one window for every opened file, restored tabs and the same shortcuts. `./build.sh deb` makes a Debian package with a menu entry and the icon.
- HTML inside Markdown is shown on every system: alignment, pictures with `width`, emphasis, `<kbd>`, line breaks, links, headings, lists, tables and `<details>`. Scripts, styles, frames, embedded objects and forms are dropped with their content; HTML exports keep the HTML as written behind a Content Security Policy that blocks scripts.
- `Ctrl+Shift+T` reopens closed tabs (up to 25, never saved documents included); files moved or deleted in the meantime are skipped with a note in the status bar.
- Separate build outputs: `build/windows/` (`build.bat`) and `build/linux/` (`build.sh`).
- Repository cover image (`docs/images/cover.png`, 1280 x 640) drawn by `tools/mkcover.c`.

[Unreleased]: https://github.com/dogukansahil/Domdly/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/dogukansahil/Domdly/releases/tag/v1.0.0
