# Domdly architecture

Domdly is split into a portable **core** and one **front end** per operating system. The core knows nothing about windows, files or fonts; the front ends know nothing about Markdown syntax.

```text
          Markdown text (UTF-8)
                   |
                   v
   +-------------------------------+
   | core/markdown.c   md_parse()  |   block pass, then inline pass
   +-------------------------------+
                   |  md_node tree
         +---------+----------+
         v                    v
 core/render_html.c    core/render_rtf.c
 (export, md2html,     (Windows preview,
  tests)                macOS later)
                              |
                              v
               platform/<os>/ front end
```

## Ground rules

1. **No third-party code.** Every line in this repository is original. A front end may only use what its operating system provides (Win32, AppKit, the desktop's native toolkit) plus the C standard library.
2. **The core is portable C99.** Files in `src/core/` must not include platform headers, must not do I/O and must build warning-free with `-std=c99 -Wall -Wextra -Wpedantic` on GCC and Clang and with `/W4` on MSVC.
3. **Front ends are thin.** Anything that could be shared (parsing, rendering, slugs, URL safety checks) belongs in the core.

## The core

| File | Purpose |
| --- | --- |
| `mdbuf.c/.h` | Growable byte buffer and allocation helpers (abort on out-of-memory). |
| `markdown.c/.h` | The parser. `md_parse()` returns a tree of `md_node`. |
| `render_html.c/.h` | HTML body or complete page (`md_render_html_page`) with a built-in style sheet. |
| `render_rtf.c/.h` | RTF for rich text controls, with color themes and image hooks. |
| `md_html.c/.h` | Turns HTML inside a document into ordinary nodes (`md_expand_html()`), dropping scripts, styles, frames and forms, so viewers show it without a browser engine. |
| `md_write.c/.h` | Markdown writer: turns styled runs (bold, italic, code, links, line breaks) back into Markdown, escaping plain text so it stays plain. |

### Parsing

The parser runs in two passes.

1. **Blocks.** Tabs are expanded and line endings normalised. Lines are classified (heading, fence, quote, list item, table, HTML, paragraph...). Container blocks such as block quotes and list items *gather* their lines, strip their prefix and parse the result recursively. Line numbers are kept one-to-one, so every block node records the source line where it starts (`md_node.line`).
2. **Inlines.** Paragraphs, headings and table cells are parsed with the CommonMark delimiter-stack algorithm for emphasis, links and images. Link reference definitions are collected during the block pass, so references work wherever they are defined.

Nesting depth is limited (`MD_MAX_DEPTH`) so hostile input cannot exhaust the stack.

### Rendering

Both renderers walk the same tree. The RTF renderer works around the limits of rich text controls: code blocks, block quotes and horizontal rules are drawn as single-cell tables because cell borders and shading are widely supported while paragraph borders are not.

With `block_marker` set, the RTF renderer also writes one hidden private-use character at the start of every block and reports the block's source line. The host finds those characters in the rendered text to map positions in both directions; this is how Domdly keeps the same block on screen when switching between Edit and Read, and how a click in the Read view finds the block to edit.

### Editing in the Read view

The Read view is edited like a word processor. Every block marker comes with its node (`block_node`), and the parser records where each block ends (`md_node.end_line`), so the front end knows for every block its kind, its source lines and its line prefix (list marker, `>` of a quote, `#` of a heading). Typing happens in the formatted text itself; when the caret leaves a changed block (or after a short pause), the front end reads that block back run by run through the Text Object Model, turns the runs into Markdown with `md_write_inline()` and replaces only that block's lines in the editor. Blocks that were not touched keep their exact source. Structural edits (Enter, joining blocks, pasting, list and heading commands) are made in the source, and the view is rendered again with the caret put back at the same visible offset of the same block.

Blocks that cannot be written back faithfully (pictures, inline HTML) open as Markdown source instead: with `raw_line` set, the RTF renderer replaces the top-level blocks on source lines `raw_line` to `raw_end - 1` with their Markdown source (`raw_text`), framed by two tiny paragraphs that hold the hidden characters `MD_RTF_RAW_START` and `MD_RTF_RAW_END`. `md_block_span()` widens a line range to whole top-level blocks, so the region always covers complete blocks. The Windows front end lets the user type only between the two frame paragraphs, copies every change straight into the editor (the editor's text stays the only copy of the document) and re-renders when the caret leaves the block or a finished block can be formatted.

Images are handled by the host. With `image_placeholder` set, the RTF renderer asks the host about each image and writes a private-use placeholder character; the host later swaps those characters for real pictures. This keeps file access and image decoding out of the core.

## The Windows front end (`src/platform/win32/`)

| File | Purpose |
| --- | --- |
| `main.c` | Main window, Edit / Read views and switching, window chrome (header, status bar, icons drawn as anti-aliased signed distance shapes), menu, commands, find and replace, links, drag and drop. |
| `install.c` | Self-installer: Program Files copy, Start menu shortcut, *Installed apps* entry, *Open with* registration, uninstall. |
| `settings.c` | Per-user settings in `HKEY_CURRENT_USER\Software\Domdly`. |
| `olecb.c` | Minimal `IRichEditOleCallback` so the preview can host pictures. |
| `util.c` | UTF-8/UTF-16 conversion, atomic file writes, text decoding. |
| `Domdly.rc`, `resource.h`, `Domdly.manifest` | Menus, shortcuts, version information, manifest. |

The Edit and Read views are two RichEdit controls (`msftedit.dll`, part of Windows) that occupy the same place; only one is visible. The Edit view runs in plain text mode and source lines are addressed through the Text Object Model, because RichEdit's own "lines" change with word wrap. The Read view is rendered when it is shown (or after a pause in typing while it is visible): the front end parses the text, renders RTF and streams it into the control; key presses are checked against the block structure before RichEdit sees them, so table cells, markers and spacing paragraphs cannot be damaged. Pictures are inserted through the Text Object Model (`ITextRange2::InsertImage`), which lets Windows decode PNG, JPEG, GIF and BMP.

Each tab owns its own editor control (so undo history and selection stay with the document); the Read view is shared and rendered for the active tab. A second Domdly process hands its files to the running window with `WM_COPYDATA`.

### Saving as PDF

Save as PDF reuses the RTF renderer with the light colors and the page width, loads the result into a hidden RichEdit control and prints it page by page (`EM_FORMATRANGE`) to the *Microsoft Print to PDF* printer, with the output file name set in `DOCINFO`. No dialog appears and no PDF code is needed in Domdly. A future Linux or macOS front end can use the platform's own PDF printing in the same way.

## The Linux front end (`src/platform/gtk/`)

`main.c` is a GTK 3 application (`GtkApplication`, so a second start hands its files to the running window over D-Bus). Each tab is a `GtkStack` with two `GtkTextView`s: the Edit view holds the source (with a small undo stack, which GTK 3 text views lack, and list continuation on Enter), the Read view is filled by walking the parse tree and applying text tags; tables are `GtkGrid`s of labels anchored in the text, pictures are pixbufs. Block start marks map positions between the two views the same way the hidden markers do on Windows. PDF export lays the tree out with Pango on a cairo PDF surface. The icon is drawn at run time from `tools/icon_art.h`, and `build.sh deb` turns the build into a Debian package with `packaging/linux/domdly.desktop`.

Editing in the Read view (the word processor mode of Windows) is not ported yet: it needs a GTK version of the run reader that feeds `md_write_inline()`.

## Adding a front end for another system

A new front end lives in `src/platform/<name>/` and needs to:

1. Provide an editor widget and a way to show rendered output.
2. Call `md_parse()` on the editor text and render the tree:
   - **macOS:** `NSAttributedString` can read RTF directly, so `md_render_rtf()` can be reused as is, with `image_placeholder` mapped to `NSTextAttachment`.
   - **Linux:** done with GTK 3 text tags, see above.
3. Implement file open/save, settings and installation the way the platform expects (for example a `.desktop` file and an install prefix on Linux, an `.app` bundle on macOS).
4. Add the build to `build.sh` and to the CI workflow.

Keep the user-visible behaviour consistent with Windows: the same menus and shortcuts (with the platform's modifier key), the same themes and the same privacy rules (no network access, no remote images).
