# Security policy

Domdly opens documents that may come from anyone, so parser crashes and unsafe link handling are treated as security issues.

## What Domdly does to stay safe

- It never downloads remote images and does not go online on its own; the only network access is the manual *Check for Updates on GitHub* command, one HTTPS request to `api.github.com` that downloads and runs nothing.
- Links in a document never run programs: web and e-mail links open in your browser or mail app, other local files are only shown in File Explorer.
- `javascript:`, `vbscript:`, `file:` and non-image `data:` links are dropped from HTML exports.
- HTML inside a document is shown without a browser engine: only formatting, links, local pictures, headings, lists and tables are drawn. `<script>`, `<style>`, `<iframe>`, `<object>`, `<embed>` and forms are dropped with their content, so nothing in a document is ever run.
- HTML exports keep the document's HTML as written, with a Content Security Policy that blocks scripts, plug-ins and frames when the page is opened in a browser.
- Nesting depth and image sizes are limited.

## Reporting a vulnerability

Please do not open a public issue. Use GitHub's private vulnerability reporting on this repository (*Security > Report a vulnerability*) and include a sample document that shows the problem. You can expect a first answer within a week.
