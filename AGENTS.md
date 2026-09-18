# lcode development notes

This file is automatically appended to the system prompt when lcode is started in this directory. The conventions below apply to this repository (a command-line coding agent written in C11 that talks to DeepSeek's OpenAI-compatible Chat Completions API).

Update AGENTS.md to match the actual code.

## Project structure

- `src/main.c` — entry point: environment and command-line argument parsing, `--list-*` modes, the directory-trust check, and `AGENTS.md` auto-loading
- `src/agent.c` — system prompt and message construction, the REPL and tool-calling main loop, session commands (`/new`, `/switch`, ...), DeepSeek thinking mode, and token-usage accounting
- `src/http.c` — libcurl-based HTTP POST with a spinner and ESC-interrupt polling, wraps `HttpResponse`
- `src/tools.c` — tool schemas and implementations (`read_file` / `write_file` / `edit_file` / `list_dir` / `run_shell` / `ask_user` / `skill`) plus the interactive option menu
- `src/skills.c` — skill discovery and loading (`.lcode/skills` and the global skills directory, `SKILL.md` frontmatter)
- `src/session.c` — session persistence: JSON store under the sessions directory, list/load/save/delete, most-recent lookup
- `src/trust.c` — directory trust model: canonicalisation, the trusted-directory list and its config file, active root
- `src/interrupt.c` — ESC-based interrupt state and raw-terminal watch used while a request or command is in flight
- `include/*.h` — corresponding module headers
- `test/` — test code (currently only a placeholder `t.c`, not wired into the build)
- `cmake/FindReadline.cmake` — module to locate readline
- `CMakeLists.txt` — build configuration

## Build and verification

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build -j"$(nproc)"`
- Debug build: `-DCMAKE_BUILD_TYPE=Debug`
- Dependencies are found with pkg-config: libcurl and json-c (plus readline and pthreads).
- Always rebuild after changes, and make sure `-Wall -Wextra` reports no new warnings (the build adds `-Wno-unused-parameter`).
- Run: `./build/lcode`; for testing, point `--base-url` at a local mock service to avoid real API calls.

## Runtime model

- Tools are confined to trusted directories. `src/main.c` calls `ensure_trust()` at startup and refuses to run in an untrusted tree unless the user approves (or `--trust` is passed); `path_guard()` in `src/tools.c` re-checks every file tool, and `run_shell` refuses when no trusted root is active. Do not bypass these checks.
- Persistent state lives under `LCODE_HOME` when set, otherwise the XDG paths: sessions in `<data>/lcode/sessions`, skills in `<data>/lcode/skills`, trusted dirs in `<config>/lcode/trusted.json`.
- Tools: `read_file`, `write_file`, `edit_file`, `list_dir`, `run_shell`, `ask_user`, and `skill`. `run_shell` streams output from a reader thread and kills the whole process group on interrupt.

## Code conventions

- Use C11 with `_GNU_SOURCE`; feel free to use GNU/POSIX extensions such as `asprintf`, `strdup`, `strndup`, `getline`, and `open_memstream`.
- Use 4-space indentation and match the style of the existing files; do not add comments unless necessary.
- Prefer reusing existing helpers and patterns (such as `env_or`, `read_whole_file`, `arg_str`, the `sb_*` / `StrBuf` buffer, `xstrdup`).
- Dynamic memory must be freed in pairs; clean up on error paths too, to avoid leaks.
- Do not introduce new third-party dependencies without good reason; dependencies are located via pkg-config.
- Keep public interfaces in the `include/` headers and keep them concise.

## Behavioral conventions

- When adding a command-line option or environment variable, update `usage()` and both `README.md` and `README_CN.md` accordingly.
- When using an LLM to develop lcode, the git commit message must include a `Co-authored-by` line crediting the model used.
- For network requests, file I/O, and error handling, produce clear, readable error messages.
- Keep changes minimal and focused on the current task; do not perform unrelated refactoring.
- Before finishing, briefly state what changed and how it was verified (build/run results).
