---
title: AI-readable documentation
description: Use the Kirn documentation as a compact, structured context source.
---

# AI-readable documentation

Kirn Docs publishes machine-readable context alongside the human documentation:

- [`llms.txt`](/kirn/llms.txt) — a compact index of the documentation and ecosystem links.
- [`llms-full.txt`](/kirn/llms-full.txt) — a single Markdown bundle containing the full page text.
- [`ai/docs.json`](/kirn/ai/docs.json) — structured page metadata, canonical URLs, and stable source paths.

These files are generated from the same Markdown pages used by VitePress. They are useful for coding assistants, search indexes, offline readers, and retrieval pipelines.

## Recommended retrieval flow

1. Fetch `llms.txt` to discover available pages.
2. Fetch individual page URLs when focused context is needed.
3. Use `ai/docs.json` for metadata or a complete page inventory.
4. Use `llms-full.txt` when one context bundle is more useful than multiple requests.

The source repository is [github.com/rkriad585/kirn](https://github.com/rkriad585/kirn). Documentation examples should be checked against the current compiler before being treated as implementation guarantees.
