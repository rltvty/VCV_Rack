# VCV Rack Fork Agents Guide

This repo is both a source tree and a reference workspace for learning how Rack works and how custom plugins could be built on top of it. Treat it like a codebase and a lab notebook.

## Scope

- This root guide applies repo-wide unless a deeper `AGENTS.md` adds narrower instructions for a subfolder.
- Any subproject guide should be used together with this file, not instead of it.

## Orientation

Before making large assumptions about current project direction, read:

- `README.md`
- `docs/index.md`
- `Core.json`
- `helper.py`

When the task is about plugin development or module behavior, also inspect the most relevant local examples:

- `include/rack.hpp`
- `include/helpers.hpp`
- `include/componentlibrary.hpp`
- `src/core/`

The most useful mental model for this repo is:

- `include/` is the public API and reusable framework surface
- `src/` is Rack internals and built-in module implementations
- `src/core/` is the best local example set for module structure and patterns
- `helper.py` and `plugin.mk` are the fastest path for custom plugin scaffolding and builds

## Exploration First

- Preserve ideas even when implementation direction changes.
- Prefer writing down hypotheses, dead ends, reverse-engineering notes, and design pivots instead of letting them disappear in chat history.
- If you create throwaway experiments, notes, or plugin ideas that may matter later, prefer saving them under `explorations/` with a descriptive name rather than leaving them only in transient discussion.
- If `explorations/` does not exist yet, create it when needed.

## Daily Journal

Maintaining a running daily journal in `explorations/` is required for non-trivial work in this repo.

The journal should read like a scientist's daily notes:

- what was explored
- what was tried
- what was learned
- what seems promising
- what failed or remains unclear

### How To Choose The Journal File

- Use the system date at runtime. Do not infer the date from memory or from the conversation alone.
- Get the current date from a datetime tool before journaling.
- Preferred command:
  `date '+%Y-%m-%d %H:%M %Z'`
- Journal filename format:
  `explorations/YYYY-MM-DD-journal.md`
- If today's file already exists, append to it.
- If it does not exist, create it.

### When To Write

Write a journal update at these points:

- after every few meaningful exploration or implementation iterations
- when changing approach or revising the plan
- when discovering an important constraint, bug, or insight
- at natural conclusion points
- before finishing any non-trivial task that involved investigation, design decisions, or experimentation

Do not wait until the very end of a long session if useful findings are already accumulating.

### What To Record

Each journal update should be concrete and concise. Record things that would still matter weeks later:

- current goal or question
- files, modules, or systems explored
- important findings
- decisions made and why
- rejected ideas or failed attempts
- open questions
- likely next steps

When helpful, include specific file paths, binary names, commands, module names, API entry points, or UI behaviors.

### Suggested Entry Shape

Use a short timestamped section inside the daily file, for example:

- `## 10:15 CEST - Core module walkthrough`
- `## 14:40 CEST - First plugin scaffold`

Within that section, prefer short paragraphs or flat bullets over long narrative dumps.

### Journal Quality Bar

- Append; do not replace prior notes from the same day.
- Favor high-signal observations over raw command transcripts.
- Capture pivots and abandoned ideas, not just successful changes.
- If the work changed direction, say that explicitly.
- If no code changed but understanding improved, that is still journal-worthy.

## Status Docs Vs Journal

- Use the daily journal for in-progress notes, experiments, and evolving thinking.
- Use focused notes under `explorations/` or `docs/` for more stable summaries when the task specifically calls for a reusable writeup, architecture note, or plugin design reference.
