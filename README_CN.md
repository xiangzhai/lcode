# lcode

[English](README.md) | 简体中文

用 C 语言编写的命令行编程智能体。它对接 DeepSeek 兼容 OpenAI 的 Chat Completions API，支持多轮对话与函数调用（工具使用）。

## 特性

- 带多轮上下文的交互式 REPL
- 随时可中断：在模型请求或工具执行过程中按 `ESC` 即可中止当前轮次，然后输入新的指示让模型转向其他方向
- 会话管理：会话以 JSON 形式持久化到磁盘，可命名、恢复、列出与切换
- DeepSeek 思考模式：默认开启，可通过 `--reasoning-effort` 配置强度；模型的思考内容会以暗淡文本前缀 `[thinking]` 打印
- Token 用量统计：每次请求后会把 prompt / completion / total 以及缓存命中率打印到 stderr；使用 `/usage` 查看累计值
- 函数调用循环：模型可以连续调用多个工具
- 内置工具：
  - `read_file` — 读取文本文件
  - `write_file` — 写入（覆盖）文本文件
  - `list_dir` — 列出目录条目（类型与大小）
  - `run_shell` — 运行 shell 命令并返回输出与退出码
  - `ask_user` — 当用户需要在多个选项中做出选择时显示交互式菜单（用方向键/数字键选择，`Enter` 确认，`q`/`Esc` 取消；在非 TTY 环境下回退为数字输入）
    - 使用 `--non-interactive`（或 `-y`）时不显示菜单；决策交还给模型，由模型自行选择并静默完成任务
- 技能（Skills）：自动从约定目录中发现技能（`SKILL.md`），在系统提示中列出，并让模型按需通过 `skill` 工具加载技能的完整指令

## 依赖

构建需要一个 C11 编译器、CMake，以及下列开发库：

| 依赖 | 用途 | 建议最低版本 |
| --- | --- | --- |
| CMake | 构建系统 | 3.13 |
| GCC / Clang | C 编译器（须支持 C11） | GCC 8 / Clang 8 |
| pkg-config | 定位依赖库 | 0.29 |
| libcurl（开发包） | HTTP 请求 | 7.58 |
| json-c（开发包） | JSON 解析与序列化 | 0.12 |
| GNU Readline（开发包） | 交互式输入与历史 | 7.0 |

### 各发行版安装命令

Debian / Ubuntu：

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcurl4-openssl-dev libjson-c-dev libreadline-dev
```

Fedora / RHEL / CentOS：

```sh
sudo dnf install -y gcc cmake pkgconf-pkg-config libcurl-devel json-c-devel readline-devel
```

Arch Linux：

```sh
sudo pacman -S --needed base-devel cmake pkgconf curl json-c readline
```

macOS（Homebrew）：

```sh
brew install cmake pkg-config curl json-c readline
```

> 在 macOS 上，系统自带的 `curl`/`readline` 可能无法被 pkg-config 找到。通过 Homebrew 安装后，
> 通常需要设置
> `export PKG_CONFIG_PATH="$(brew --prefix)/opt/curl/lib/pkgconfig:$(brew --prefix)/opt/readline/lib/pkgconfig:$PKG_CONFIG_PATH"`。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

生成的二进制文件为 `build/lcode`。

调试构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

## 配置

通过环境变量提供密钥：

```sh
export DEEPSEEK_API_KEY=sk-xxxxxxxx
```

可选环境变量：

| 变量 | 说明 | 默认值 |
| --- | --- | --- |
| `DEEPSEEK_API_KEY` | API 密钥（必需） | 无 |
| `DEEPSEEK_BASE_URL` | API 基础地址 | `https://api.deepseek.com` |
| `DEEPSEEK_MODEL` | 模型名称 | `deepseek-flash` |
| `DEEPSEEK_THINKING` | 思考模式；设为 `disabled` / `off` / `false` / `0` 可关闭，其他任何值保持开启 | 开启 |
| `DEEPSEEK_REASONING_EFFORT` | 思考强度：`minimal` / `low` / `medium` / `high` / `xhigh` / `max` / `ultra` | `high` |
| `LCODE_HOME` | 会话存储的根目录（会话位于其下的 `sessions/`） | `$XDG_DATA_HOME/lcode`，否则为 `~/.local/share/lcode` |

## 运行

```sh
./build/lcode
```

命令行选项（会覆盖对应的环境变量）：

```
--model <name>                     模型名称，默认为 deepseek-flash
--base-url <url>                   API 基础地址
--system-prompt <text>             替换内置系统提示
--system-prompt-file <path>        从文件读取系统提示（替换内置提示）
--append-system-prompt <text>      在内置系统提示之后追加指令
--append-system-prompt-file <path> 将文件内容追加到系统提示
--prompt <text>                    预载一个初始用户提示，并在启动时发送
--prompt-file <path>               从文件读取初始用户提示
--session <name>                   打开或新建一个命名会话
--resume                           恢复最近更新的会话
--list-sessions                    列出已保存的会话并退出
--list-skills                      列出已发现的技能并退出
--non-interactive, -y              非交互模式：让模型自行决策、绝不询问用户；
                                   若同时给出 --prompt/--prompt-file，完成后即退出
--thinking                         开启思考模式（默认）
--no-thinking                      关闭思考模式
--reasoning-effort <level>         思考强度：minimal|low|medium|high|xhigh|max|ultra
                                   （默认：high）
--max-iterations <n>               每 n 轮工具调用打印一次提示；一个轮次会持续
                                   运行，直到模型不再调用工具（0 = 不限，默认值）
-h, --help                         显示帮助
-V, --version                      显示版本并退出
```

示例：

```sh
./build/lcode --model deepseek-flash
./build/lcode --append-system-prompt "always answer in Chinese"
./build/lcode --prompt "list the current directory first, then summarize the project structure"
./build/lcode --system-prompt-file prompts/reviewer.md --prompt-file prompts/task.md
./build/lcode --session refactor-auth        # 打开/新建命名会话
./build/lcode --resume                       # 恢复最近的会话
./build/lcode --list-sessions                # 列出所有会话
./build/lcode --no-thinking                  # 关闭思考模式
./build/lcode --reasoning-effort max         # 使用最高思考强度
```

> 预载的初始提示会在进入 REPL 之前自动发送；之后你可以继续输入。当同时给出 `--prompt` 和
> `--prompt-file` 时，以文件内容为准（选项按顺序生效，因此后出现的会覆盖先出现的）。

启动时，lcode 会从**当前目录**读取 `AGENTS.md` 并把其内容追加到系统提示
（等价于 `--append-system-prompt-file AGENTS.md`）。如果显式给出了 `--append-system-prompt` 或
`--append-system-prompt-file`，则以其为准，不再加载 `AGENTS.md`。
可用它来固化项目的开发与调试约定，正如本仓库自带的 `AGENTS.md` 那样。

## 会话管理

每个会话把完整消息历史（system / user / assistant / tool）保存在单个 JSON 文件中，
默认位于 `~/.local/share/lcode/sessions/<name>.json`（可用 `LCODE_HOME` 覆盖根目录）。
每轮对话后会自动保存；启动时若未给出 `--session`，会新建一个带时间戳的会话。

命令行：`--session <name>` 打开或新建，`--resume` 恢复最近的一个，`--list-sessions`
列出并退出。

REPL 命令：

```
/new [name]      新建会话（会先保存当前会话）
/list            列出已保存的会话
/switch <name>   保存当前会话并切换到另一个
/delete <name>   删除已保存的会话（不能删除当前会话）
/session         显示当前会话名
/save            立即保存当前会话
/usage           显示本次会话的 token 用量与缓存命中率
/skills          列出已发现的技能
/exit            保存并退出（等同于 Ctrl-D）
/help            显示命令帮助
```

> 恢复会话会保留之前的对话，但第一条 system 消息会被替换为根据当前启动配置生成的系统提示
> （因此对 `AGENTS.md`、`--append-system-prompt` 等的改动会立即生效）。

## 技能

技能用于按需注入某个特定任务的详细指令，而不是一开始就占用系统提示。

技能按以下顺序被发现（名称冲突时，先找到者胜出）：

1. 当前目录下的 `.lcode/skills/`（项目级）
2. 全局目录：`$LCODE_HOME/skills`，否则 `$XDG_DATA_HOME/lcode/skills`，否则
   `~/.local/share/lcode/skills`

每个技能要么是一个包含 `SKILL.md` 的子目录，要么是直接位于某个技能目录下的独立 `*.md` 文件。
文件顶部可选的 YAML frontmatter 提供 `name` 与 `description`：

```md
---
name: pdf-forms
description: Fill in and validate PDF forms
---

# Steps
1. ...
```

省略 `name` 时使用目录名（或文件名）；省略 `description` 时使用正文的第一行。

启动时 lcode 会把每个技能的名称与描述追加到系统提示；当某个任务匹配时，模型会调用 `skill`
工具（参数 `name`）加载该文件的完整正文。使用 `--list-skills` 或 REPL 的 `/skills` 查看发现列表。

## 用量与缓存

每次 API 请求后，lcode 会读取响应中的 `usage` 字段，并把统计数据打印到 **stderr**（以暗淡的一行显示），
因此不会污染对话内容、不会写入会话历史，也不会影响前缀缓存：

```
[usage] prompt 1200 (cache hit 1100 / miss 100, 91.7%), completion 12, total 1212
```

`/usage` 显示当前会话的累计值；命中率为
`cache_hit / (cache_hit + cache_miss)`。统计数据同时识别 DeepSeek 的
`prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` 以及 OpenAI 风格的
`prompt_tokens_details.cached_tokens`。累计值仅存在于进程内，重启后从零开始。

进入 REPL 后，直接输入你的请求即可，例如：

```
> list the current directory, then read CMakeLists.txt and explain the build configuration
```

按 `Ctrl-D` 退出。

## 思考模式

lcode 默认开启 DeepSeek 的思考模式。开启时，每次请求会发送 `thinking` 字段
（`{"type": "enabled"}`）以及可选的 `reasoning_effort` 强度，模型返回的 `reasoning_content`
会以暗淡文本、前缀 `[thinking]` 的形式打印（temperature 保持 API 默认值）。关闭思考模式时，
lcode 改为发送 `{"type": "disabled"}` 并使用固定的 `temperature` `0.2`。

可用 `--thinking` / `--no-thinking` 或环境变量 `DEEPSEEK_THINKING` 开关，用
`--reasoning-effort <level>`（或 `DEEPSEEK_REASONING_EFFORT`）选择强度。有效取值为
`minimal`、`low`、`medium`、`high`、`xhigh`、`max`、`ultra`，默认为 `high`。
启动时会对非法的强度取值报错。

## 中断与重定向

当模型正在调用 API 或运行工具，而你发现它耗时过久或偏离方向时，按一次 `ESC` 即可中断：

- 正在进行的 HTTP 请求会被中止，正在运行的 `run_shell` 子进程会被杀掉，任何未完成的工具调用会
  用 "interrupted by user" 占位符补齐，从而保持会话的消息序列有效；
- 随后会出现 `redirect>` 提示，你可以在其中输入新的指示（例如 "don't use sed, use patch"）让模型转向；
  直接回车则跳过；
- 新指示会作为 user 消息追加到当前会话，模型在既有上下文基础上按新方向继续；
- 在 `--non-interactive` 模式下不显示提示，中断后该轮次立即结束。

被中断的轮次会完整保存到会话中，因此恢复后可以接着之前的位置继续。
