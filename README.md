# lcode

[English](README.md) | [简体中文](README_CN.md)

A command-line coding agent written in C. It talks to DeepSeek's OpenAI-compatible Chat Completions API and supports multi-turn conversations and function calling (tool use).

## Features

- Interactive REPL with multi-turn context
- Interruptible at any time: press `ESC` while a model request or a tool execution is in flight to abort the current turn, then type a new instruction to steer the model in a different direction
- Session management: sessions are persisted to disk as JSON and can be named, resumed, listed, and switched
- DeepSeek thinking mode: enabled by default, with a configurable `--reasoning-effort` level; while the model reasons, a `thinking...` status line refreshes in place (pass `--show-reasoning` to print the reasoning text instead)
- Streaming output: responses are streamed over Server-Sent Events, so text appears token by token as the model generates it (disable with `--no-stream`)
- Token usage stats: after each request, prompt / completion / total and the cache hit rate are printed to stderr; use `/usage` for the running total
- Context management: the conversation is compacted when the estimated prompt size exceeds a budget, so a long session never overruns the model's context window; the oldest messages are summarized by the model (or dropped) while the system prompt and the most recent exchanges are kept
- Function calling loop: the model can call multiple tools in a row
- Built-in tools:
  - `read_file` — read a text file
  - `write_file` — write (overwrite) a text file
  - `edit_file` — replace an exact string in a text file; by default `old_string`
    must match exactly once (pass `replace_all` to change every occurrence). Prefer this
    over `write_file` when changing part of an existing file
  - `list_dir` — list directory entries (type and size)
  - `run_shell` — run a shell command and return its output and exit code; an optional
    `timeout` argument (seconds) kills the command's process group if it runs too long,
    otherwise the default from `--shell-timeout` / `LCODE_SHELL_TIMEOUT` applies
  - `ask_user` — show an interactive menu when the user must choose among several options (arrow/number keys to select, `Enter` to confirm, `q`/`Esc` to cancel; falls back to numbered input when not attached to a TTY)
    - With `--non-interactive` (or `-y`) no menu is shown; the decision is handed back to the model, which picks for itself and completes the task silently
- Skills: automatically discover skills (`SKILL.md`) from convention directories, list them in the system prompt, and let the model load a skill's full instructions on demand through the `skill` tool
- Directory trust: before touching a new directory lcode asks whether you trust it and remembers the answer; a tool that reaches outside the trusted directories asks the same way, and directories you have not trusted are never accessed
- Shell command policy: on top of directory trust, `run_shell` classifies each command and confirms destructive ones (recursive deletes, piping a download into a shell, writes outside the trusted directories, ...) before running, refuses the most dangerous ones outright, and remembers approvals per command or per kind of command

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
| `DEEPSEEK_THINKING` | Thinking mode; set to `disabled` / `off` / `false` / `0` to disable, any other value keeps it enabled | enabled |
| `DEEPSEEK_REASONING_EFFORT` | Thinking effort: `minimal` / `low` / `medium` / `high` / `xhigh` / `max` / `ultra` | `high` |
| `LCODE_SHELL_TIMEOUT` | Default `run_shell` timeout in seconds; `0` disables it | `120` |
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
--trust                            trust the current directory without asking and save it
--list-trusted                     list trusted directories and exit
--list-permissions                 list remembered shell command permissions and exit
--reset-permissions                forget all remembered shell command permissions
--allow-dangerous                  run shell commands without the dangerous-command
                                   confirmation policy (this run only)
--non-interactive, -y              non-interactive mode: let the model decide, never ask the user;
                                   if --prompt/--prompt-file is also given, exit once it finishes
--stream                           stream the response over SSE (default)
--no-stream                        buffer the whole response before printing it
--thinking                         enable thinking mode (default)
--no-thinking                      disable thinking mode
--reasoning-effort <level>         thinking effort: minimal|low|medium|high|xhigh|max|ultra
                                   (default: high)
--show-reasoning                   print the model's reasoning content instead of the
                                   live 'thinking...' status line
--max-iterations <n>               print a notice every n tool-call rounds; a turn keeps
                                   running until the model stops calling tools (0 = unlimited,
                                   the default)
--shell-timeout <seconds>          kill a run_shell command that runs longer than this
                                   (0 = no timeout, default: 120)
--context-tokens <n>               compact the history once the estimated prompt size exceeds
                                   n tokens (0 = disabled, default: 60000)
--context-keep <n>                 keep at least the last n messages when compacting
                                   (default: 8)
--context-strategy <summarize|truncate>
                                   how to shrink the history: ask the model to summarize the
                                   dropped messages, or simply drop them (default: summarize)
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
./build/lcode --no-thinking                  # disable thinking mode
./build/lcode --reasoning-effort max         # use the highest thinking effort
```

> The preloaded initial prompt is sent automatically before entering the REPL; you can keep typing
> afterwards. When both `--prompt` and `--prompt-file` are given, the file contents win (options are
> applied in order, so whichever appears later overrides the earlier one).

At startup, lcode reads `AGENTS.md` from the **current directory** and appends its contents to the
system prompt (equivalent to `--append-system-prompt-file AGENTS.md`). If `--append-system-prompt` or
`--append-system-prompt-file` is given explicitly, that takes precedence and `AGENTS.md` is not loaded.
Use it to pin down a project's development and debugging conventions, as this repository's own
`AGENTS.md` does.

## Directory trust

lcode only reads, writes and runs commands inside directories you have explicitly trusted.
On the first run in a directory, lcode asks:

```
lcode: '/path/to/project' is not a trusted directory.
lcode will only read, write and run commands inside a directory you trust.
Trust this directory and its contents? [y/N]
```

Answering yes records the directory in the trust config file and lets lcode run; answering no
aborts the run so the directory is never touched. A directory is also considered trusted when one
of its parents is trusted, so trusting a project root covers its subdirectories.

The file tools (`read_file`, `write_file`, `edit_file`, `list_dir`) refuse any path that resolves (through `..`
or symlinks) outside every trusted directory, and `run_shell` refuses to run when no trusted
directory is active. A shell command can still reach elsewhere on its own, so trust a
directory only when you are comfortable with lcode running commands in it; the
[shell command policy](#shell-command-policy) adds a second layer that confirms destructive
commands and keeps writes outside the trusted directories in check.

A tool that reaches outside the trusted directories is offered to you before it is refused, using
the same menu as the `ask_user` tool:

```
? '$HOME/notes.txt' is outside the trusted directories. Trust '$HOME' and continue?
  1) Yes, trust '$HOME'
  2) No, keep refusing access
```

Choosing yes records the directory in the trust config file and lets the tool call proceed; choosing
no returns the refusal to the model. The directory containing the target is trusted: the target
itself when it is a directory, otherwise its parent. Every recorded directory stays accessible for
the rest of the run, and only a terminal can answer, so in non-interactive mode (or with `stdin`
redirected from a file or pipe) the prompt is skipped and access stays refused.

Trusted directories are stored in `$LCODE_HOME/trusted.json` when `LCODE_HOME` is set, otherwise
`$XDG_CONFIG_HOME/lcode/trusted.json`, otherwise `~/.config/lcode/trusted.json`. Use
`--list-trusted` to inspect them. In non-interactive mode (`--non-interactive`) lcode never
prompts; pass `--trust` to trust the current directory up front.

## Shell command policy

Directory trust decides *where* lcode may work, but a trusted directory does not make every command
safe, so `run_shell` runs each command through a policy before executing it. Commands are
classified three ways:

- **safe** — ordinary commands (`ls`, `make`, `git status`, ...) run without asking;
- **risky** — commands that can lose data or escape the trusted tree are confirmed first, for
  example recursive deletes (`rm -rf build`), piping a download into a shell (`curl ... | sh`),
  `sudo`, force pushes (`git push --force`), `git reset --hard`, `git clean -fd`, recursive
  `chmod`/`chown`, `eval`, `dd`/`shred`, `find -delete`, and redirects or `dd` writes to a path
  outside the trusted directories;
- **forbidden** — a small set of commands that are refused outright, whatever you answer: erasing
  the filesystem root or a critical system path, formatting a disk, writing to a raw block device,
  overwriting a system file, shutting the machine down, and fork bombs.

A risky command is confirmed with a menu:

```
? Run this shell command?
  1) Yes, run it once
  2) Yes, and always allow this exact command
  3) Yes, and always allow this kind of command
  4) No, do not run it
```

Choosing "once" runs just this command; the "always allow" options remember a rule so lcode stops
asking — either for the exact command or for its whole kind (for example, all recursive deletes)
— which refines the coarse directory-level trust down to the level of individual commands. Choosing
"No" returns a refusal to the model, which is told not to retry the command unchanged.

Remembered rules are stored in `permissions.json` next to the trust config (under `$LCODE_HOME`, or
`$XDG_CONFIG_HOME/lcode`, or `~/.config/lcode`). Inspect them with `--list-permissions` and forget
them all with `--reset-permissions`.

In non-interactive mode (`--non-interactive`) there is no one to confirm, so risky commands are
refused; pass `--allow-dangerous` to run commands without the confirmation policy for that run.
Use it only when you fully trust the model and the task.

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

## Context management

A long session accumulates messages without bound, and eventually the prompt would exceed the
model's context window. To prevent that, lcode estimates the size of the message history before
every request and compacts it when it grows past a budget:

- the system prompt is always kept;
- at least the last `--context-keep <n>` messages (default `8`) are always kept;
- older messages are removed, and the kept tail never starts in the middle of a tool call (a
  `tool` result is never separated from the assistant call it answers);
- with the default `--context-strategy summarize`, the messages about to be dropped are first sent
  to the model in a small extra request and replaced by a single system message holding the
  summary, so goals, decisions, touched files and open work are preserved. If that request fails
  or you pass `--context-strategy truncate`, the messages are simply dropped instead.

The budget is set with `--context-tokens <n>` (or `LCODE_CONTEXT_TOKENS`); `0` disables compaction.
The estimate is a heuristic — ASCII text at roughly four characters per token, non-ASCII characters
at roughly one token each — so leave some headroom below the real window size. The same value can
be set with `LCODE_CONTEXT_TOKENS` / `LCODE_CONTEXT_KEEP` / `LCODE_CONTEXT_STRATEGY`.

Compaction is silent: it prints nothing, and only shows up in the token usage.

As with `run_shell`, the summarization request counts toward token usage and briefly invalidates the
prompt cache prefix (the history it was built on has changed).

## Thinking mode

lcode enables DeepSeek's thinking mode by default. When it is on, each request sends a `thinking`
field (`{"type": "enabled"}`) and an optional `reasoning_effort` level, and the model's
`reasoning_content` is kept out of the way: instead of scrolling the whole chain of thought past you,
lcode keeps a single `thinking...` status line that refreshes in place until the answer (or a tool
call) starts, then clears it. Pass `--show-reasoning` to print the reasoning in dim text prefixed
with `[thinking]` instead (temperature is left at the API default). When thinking is disabled, lcode
instead sends `{"type": "disabled"}` and a fixed `temperature` of `0.2`.

Toggle it with `--thinking` / `--no-thinking` or the `DEEPSEEK_THINKING` environment variable, and
choose the effort with `--reasoning-effort <level>` (or `DEEPSEEK_REASONING_EFFORT`). Valid levels are
`minimal`, `low`, `medium`, `high`, `xhigh`, `max`, and `ultra`; the default is `high`. An invalid
level is rejected at startup.

## Streaming output

By default lcode requests the response as a Server-Sent Events stream and prints the
model's text as soon as the tokens arrive, instead of waiting for the whole
reply. While the model is thinking, a `thinking...` status line refreshes in place instead of
streaming the chain of thought (see [Thinking mode](#thinking-mode)). The token-usage stats at the
end of a turn still come from the final stream chunk.
Pass `--no-stream` to fall back to buffering the complete response before printing it,
which is useful for endpoints or proxies that do not support SSE.

## Shell command timeout

A `run_shell` command that hangs (a stuck build, an interactive program waiting for input, a
runaway loop) would otherwise block the whole turn. Pass `--shell-timeout <seconds>` (or set
`LCODE_SHELL_TIMEOUT`) to put a limit on every command: once it elapses, lcode kills the command's
whole process group (reaching pipelines, `make`, and other grandchildren) and reports the output
collected so far as timed out. The default is `120` seconds; pass `0` to disable the timeout.

The model can also pass a per-call `timeout` argument (in seconds) to the `run_shell` tool to
override the configured default for a single command; `0` disables the timeout for that call. Unlike
pressing `ESC`, a timeout does not interrupt the turn — the model simply receives the timed-out
output and can continue.

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
