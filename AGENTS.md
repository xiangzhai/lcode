# lcode development notes

This file is automatically appended to the system prompt when lcode is started in this directory. The conventions below apply to this repository (a command-line coding agent written in C11 that talks to DeepSeek's OpenAI-compatible Chat Completions API).

## Project structure

- `src/main.c` — entry point: environment and command-line argument parsing, `AGENTS.md` auto-loading
- `src/agent.c` — system prompt, message construction, REPL and the tool-calling main loop
- `src/http.c` — libcurl-based HTTP POST, wraps `HttpResponse`
- `src/tools.c` — tool schemas and implementations (`read_file` / `write_file` / `list_dir` / `run_shell`)
- `src/skills.c` — skill discovery and loading (`.lcode/skills`, global skills directory, `SKILL.md` frontmatter)
- `include/*.h` — corresponding module headers
- `test/` — test code
- `CMakeLists.txt` — build configuration

## Build and verification

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build -j"$(nproc)"`
- Debug build: `-DCMAKE_BUILD_TYPE=Debug`
- Always rebuild after changes, and make sure `-Wall -Wextra` reports no new warnings.
- Run: `./build/lcode`; for testing, point `--base-url` at a local mock service to avoid real API calls.

## Code conventions

- Use C11 with `_GNU_SOURCE`; feel free to use GNU/POSIX extensions such as `asprintf`, `strdup`, and `getline`.
- Use 4-space indentation and match the style of the existing files; do not add comments unless necessary.
- Prefer reusing existing helpers and patterns (such as `env_or`, `read_whole_file`, `arg_str`, the `sb_*` buffer).
- Dynamic memory must be freed in pairs; clean up on error paths too, to avoid leaks.
- Do not introduce new third-party dependencies without good reason; dependencies are located via pkg-config.
- Keep public interfaces in the `include/` headers and keep them concise.

## Behavioral conventions

- When adding a command-line option or environment variable, update `usage()` and `README.md` accordingly.
- When using an LLM to develop lcode, the git commit message must include a `Co-authored-by` line crediting the model used.
- For network requests, file I/O, and error handling, produce clear, readable error messages.
- Keep changes minimal and focused on the current task; do not perform unrelated refactoring.
- Before finishing, briefly state what changed and how it was verified (build/run results).
