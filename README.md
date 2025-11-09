# Minimal Terminal Editor

This repository contains a single C program: a tiny terminal text editor written to be **read and studied**, not just compiled.

The goal is to show, in one file, the moving parts of a text editor:
- a terminal put in raw mode,
- a text buffer kept as an array of lines,
- a render step that rewrites the screen,
- a loop that reacts to keys.

Everything is commented line-by-line so you can follow the control flow without guessing.

---

## Why the code is so verbose

This editor is intentionally over-explained.

- Each section (terminal, buffer, rendering, input) is visible in the same file.
- Each line says *what it touches* (terminal state, buffer state, view state).
- It’s meant to be read top-to-bottom to understand how a key travels through the program.
- It favors clarity over compactness.

So: the comments are not noise, they are the point.

