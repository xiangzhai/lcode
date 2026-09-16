# lcode

A command-line coding agent written in C. It talks to DeepSeek's OpenAI-compatible Chat Completions API and supports multi-turn conversations and function calling (tool use).

## Features

- Interactive REPL with multi-turn context
- Interruptible at any time: press `ESC` while a model request or a tool execution is in flight to abort the current turn, then type a new instruction to steer the model in a different direction
- Session management: sessions are persisted to disk as JSON and can be named, resumed, listed, and switched
- Token usage stats: after each request, prompt / completion / total and the cache hit rate are printed to stderr; use `/usage` for the running total
- Function calling loop: the model can call multiple tools in a row
- Built-in tools:
  - `read_file` — read a text file
  - `write_file` — write (overwrite) a text file
  - `list_dir` — list directory entries (type and size)
  - `run_shell` — run a shell command and return its output and exit code
  - `ask_user` — show an interactive menu when the user must choose among several options (arrow/number keys to select, `Enter` to confirm, `q`/`Esc` to cancel; falls back to numbered input when not attached to a TTY)
    - With `--non-interactive` (or `-y`) no menu is shown; the decision is handed back to the model, which picks for itself and completes the task silently
- Skills: automatically discover skills (`SKILL.md`) from convention directories, list them in the system prompt, and let the model load a skill's full instructions on demand through the `skill` tool

## Dependencies

Building requires a C11 compiler, CMake, and the following development libraries:

| Dependency | Purpose | Minimum recommended version |
| --- | --- | --- |
| CMake | Build system | 3.13 |
| GCC / Clang | C compiler (must support C11) | GCC 8 / Clang 8 |
| pkg-config | Locate dependency libraries | 0.29 |
| libcurl (dev package) | HTTP requests | 7.58 |
| json-c (dev package) | JSON parsing and serialization | 0.12 |
| GNU Readline (dev package) | Interactive input and history | 7.0 |

### Install commands per distribution

Debian / Ubuntu:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcurl4-openssl-dev libjson-c-dev libreadline-dev
```

Fedora / RHEL / CentOS:

```sh
sudo dnf install -y gcc cmake pkgconf-pkg-config libcurl-devel json-c-devel readline-devel
```

Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake pkgconf curl json-c readline
```

macOS (Homebrew):

```sh
brew install cmake pkg-config curl json-c readline
```

> On macOS the system-provided `curl`/`readline` may not be found by pkg-config. After installing via
> Homebrew you usually need to set
> `export PKG_CONFIG_PATH="$(brew --prefix)/opt/curl/lib/pkgconfig:$(brew --prefix)/opt/readline/lib/pkgconfig:$PKG_CONFIG_PATH"`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The resulting binary is `build/lcode`.

For a debug build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

## Configuration

Provide the key through an environment variable:

```sh
export DEEPSEEK_API_KEY=sk-xxxxxxxx
```

Optional environment variables:

| Variable | Description | Default |
| --- | --- | --- |
| `DEEPSEEK_API_KEY` | API key (required) | none |
| `DEEPSEEK_BASE_URL` | API base URL | `https://api.deepseek.com` |
| `DEEPSEEK_MODEL` | Model name | `deepseek-flash` |
| `LCODE_HOME` | Root directory for session storage (sessions live under its `sessions/`) | `$XDG_DATA_HOME/lcode`, otherwise `~/.local/share/lcode` |

## Running

```sh
./build/lcode
```

Command-line options (these override the corresponding environment variables):

```
--model <name>                     model name, defaults to deepseek-flash
--base-url <url>                   API base URL
--system-prompt <text>             replace the built-in system prompt
--system-prompt-file <path>        read the system prompt from a file (replaces the built-in one)
--append-system-prompt <text>      append instructions after the built-in system prompt
--append-system-prompt-file <path> append file contents to the system prompt
--prompt <text>                    preload an initial user prompt and send it at startup
--prompt-file <path>               read the initial user prompt from a file
--session <name>                   open or create a named session
--resume                           resume the most recently updated session
--list-sessions                    list saved sessions and exit
--list-skills                      list discovered skills and exit
--non-interactive, -y              non-interactive mode: let the model decide, never ask the user;
                                   if --prompt/--prompt-file is also given, exit once it finishes
--max-iterations <n>               print a notice every n tool-call rounds; a turn keeps
                                   running until the model stops calling tools (0 = unlimited,
                                   the default)
-h, --help                         show help
-V, --version                      show version and exit
```

Examples:

```sh
./build/lcode --model deepseek-flash
./build/lcode --append-system-prompt "always answer in Chinese"
./build/lcode --prompt "list the current directory first, then summarize the project structure"
./build/lcode --system-prompt-file prompts/reviewer.md --prompt-file prompts/task.md
./build/lcode --session refactor-auth        # open/create a named session
./build/lcode --resume                       # resume the most recent session
./build/lcode --list-sessions                # list all sessions
```

> The preloaded initial prompt is sent automatically before entering the REPL; you can keep typing
> afterwards. When both `--prompt` and `--prompt-file` are given, the file contents win (options are
> applied in order, so whichever appears later overrides the earlier one).

At startup, lcode reads `AGENTS.md` from the **current directory** and appends its contents to the
system prompt (equivalent to `--append-system-prompt-file AGENTS.md`). If `--append-system-prompt` or
`--append-system-prompt-file` is given explicitly, that takes precedence and `AGENTS.md` is not loaded.
Use it to pin down a project's development and debugging conventions, as this repository's own
`AGENTS.md` does.

## Session management

Each session stores the full message history (system / user / assistant / tool) in a single JSON file,
by default at `~/.local/share/lcode/sessions/<name>.json` (override the root with `LCODE_HOME`).
It is saved automatically after each turn; at startup, if no `--session` is given, a new timestamped
session is created.

CLI: `--session <name>` to open or create, `--resume` to resume the most recent one, `--list-sessions`
to list and exit.

REPL commands:

```
/new [name]      start a new session (saves the current one)
/list            list saved sessions
/switch <name>   save the current session and switch to another
/delete <name>   delete a saved session (cannot delete the current one)
/session         show the current session name
/save            save the current session now
/usage           show token usage and cache hit rate for this session
/skills          list discovered skills
/exit            save and exit (same as Ctrl-D)
/help            show command help
```

> Resuming a session keeps the previous conversation, but the first system message is replaced with
> the system prompt generated from the current startup configuration (so changes to `AGENTS.md`,
> `--append-system-prompt`, etc. take effect immediately).

## Skills

A skill injects detailed instructions for a specific task on demand, instead of occupying the system
prompt up front.

Skills are discovered in the following order (when names collide, the first one found wins):

1. `.lcode/skills/` in the current directory (project-level)
2. The global directory: `$LCODE_HOME/skills`, otherwise `$XDG_DATA_HOME/lcode/skills`, otherwise
   `~/.local/share/lcode/skills`

Each skill is either a subdirectory containing `SKILL.md` or a standalone `*.md` file directly under a
skills directory. An optional YAML frontmatter block at the top of the file provides `name` and
`description`:

```md
---
name: pdf-forms
description: Fill in and validate PDF forms
---

# Steps
1. ...
```

When `name` is omitted, the directory name (or file name) is used; when `description` is omitted, the
first line of the body is used.

At startup lcode appends the name and description of every skill to the system prompt; when a task
matches, the model calls the `skill` tool (argument `name`) to load the file's full body. Use
`--list-skills` or the REPL's `/skills` to see the discovered list.

## Usage and caching

After each API request, lcode reads the `usage` field of the response and prints the stats to
**stderr** (in a dim line), so it does not pollute the conversation, get written to session history,
or affect prefix caching:

```
[usage] prompt 1200 (cache hit 1100 / miss 100, 91.7%), completion 12, total 1212
```

`/usage` shows the running total for the current session; the hit rate is
`cache_hit / (cache_hit + cache_miss)`. The stats understand both DeepSeek's
`prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` and the OpenAI-style
`prompt_tokens_details.cached_tokens`. The running total lives only in the process and starts from
zero after a restart.

Once in the REPL, just type your request, for example:

```
> list the current directory, then read CMakeLists.txt and explain the build configuration
```

Press `Ctrl-D` to exit.

## Interrupting and redirecting

When the model is calling the API or running a tool and you notice it is taking too long or going off
track, press `ESC` once to interrupt:

- the in-flight HTTP request is aborted, a running `run_shell` child process is killed, and any
  unfinished tool calls are filled in with an "interrupted by user" placeholder so the session's
  message sequence stays valid;
- you are then prompted with `redirect>`, where you can type a new instruction (for example, "don't use
  sed, use patch") to steer the model; press Enter to skip;
- the new instruction is appended to the current session as a user message, and the model continues in
  the new direction with its existing context;
- in `--non-interactive` mode no prompt appears and the turn ends immediately after the interrupt.

The interrupted turn is saved in full to the session, so you can pick up where you left off after
resuming.
