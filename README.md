# lcode

一个用 C 编写的命令行编码 agent，对接 DeepSeek 的 OpenAI 兼容 Chat Completions API，支持多轮对话与函数调用（工具调用）。

## 功能

- 交互式 REPL，多轮上下文
- 可随时打断：模型请求或工具执行途中按 `ESC` 立即中止当前回合，随后可输入一条新指示让模型换思路继续
- Session 管理：会话以 JSON 持久化到磁盘，可通过命名、恢复、列出与切换
- Token 用量统计：每次请求后在 stderr 输出 prompt / completion / total 与缓存命中率，`/usage` 查看累计
- Function Calling 工具循环，模型可连续调用多个工具
- 内置工具：
  - `read_file` —— 读取文本文件
  - `write_file` —— 写入（覆盖）文本文件
  - `list_dir` —— 列出目录内容（类型与大小）
  - `run_shell` —— 执行 shell 命令并返回输出与退出码
  - `ask_user` —— 当需要用户在多个选项中做出选择时，弹出交互式菜单（方向键/数字键选择，`Enter` 确认，`q`/`Esc` 取消；非 TTY 环境退化为编号输入）
    - 配合 `--non-interactive`（或 `-y`）使用时不弹菜单，改为把决策权交回模型，由其自行选择并静默完成任务

## 依赖

构建需要 C11 编译器、CMake 以及以下开发库：

| 依赖 | 用途 | 最低建议版本 |
| --- | --- | --- |
| CMake | 构建系统 | 3.13 |
| GCC / Clang | C 编译器（需支持 C11） | GCC 8 / Clang 8 |
| pkg-config | 查找依赖库 | 0.29 |
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

> macOS 上系统自带的 `curl`/`readline` 可能无法被 pkg-config 找到，Homebrew 安装后通常需要设置
> `export PKG_CONFIG_PATH="$(brew --prefix)/opt/curl/lib/pkgconfig:$(brew --prefix)/opt/readline/lib/pkgconfig:$PKG_CONFIG_PATH"`。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

生成的二进制文件为 `build/lcode`。

如需调试构建：

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
| `DEEPSEEK_API_KEY` | API 密钥（必填） | 无 |
| `DEEPSEEK_BASE_URL` | API 基础地址 | `https://api.deepseek.com` |
| `DEEPSEEK_MODEL` | 模型名称 | `deepseek-v4-flash` |
| `LCODE_HOME` | session 存储根目录（会话位于其下 `sessions/`） | `$XDG_DATA_HOME/lcode`，否则 `~/.local/share/lcode` |

## 运行

```sh
./build/lcode
```

命令行参数（会覆盖对应环境变量）：

```
--model <name>                     指定模型，默认 deepseek-v4-flash
--base-url <url>                   指定 API 基础地址
--system-prompt <text>             替换内置 system prompt
--system-prompt-file <path>        从文件读取 system prompt（替换内置）
--append-system-prompt <text>      在内置 system prompt 后追加指令
--append-system-prompt-file <path> 追加文件内容到 system prompt
--prompt <text>                    启动时预加载首条用户提示词并自动发送
--prompt-file <path>               从文件读取首条用户提示词
--session <name>                   打开或创建命名 session
--resume                           恢复最近更新的 session
--list-sessions                    列出已保存的 session 并退出
--non-interactive, -y              非交互模式：由模型自行决策，不询问用户；
                                   若同时给出 --prompt/--prompt-file，跑完即退出
--max-iterations <n>               单轮最多工具调用轮数，默认 64
-h, --help                         显示帮助
-V, --version                      显示版本并退出
```

示例：

```sh
./build/lcode --model deepseek-v4-flash
./build/lcode --append-system-prompt "始终使用中文回答"
./build/lcode --prompt "先列出当前目录，然后总结项目结构"
./build/lcode --system-prompt-file prompts/reviewer.md --prompt-file prompts/task.md
./build/lcode --session refactor-auth        # 打开/新建命名会话
./build/lcode --resume                       # 恢复最近一次会话
./build/lcode --list-sessions                # 列出全部会话
```

> 预加载的首条提示词会在进入 REPL 前自动发送，之后仍可继续交互输入；`--prompt` 与
> `--prompt-file` 同时给出时，以文件内容为准（按参数出现顺序，后出现者覆盖前者）。

启动时会自动读取**当前目录**下的 `LCODE.md`，将其内容追加到 system prompt（等价于
`--append-system-prompt-file LCODE.md`）。若显式指定了 `--append-system-prompt` 或
`--append-system-prompt-file`，则以显式参数为准，不再加载 `LCODE.md`。可用它固化项目的
开发与调试约定，例如仓库自带的 `LCODE.md`。

## Session 管理

每个会话把完整的消息历史（含 system / user / assistant / tool）保存为一个 JSON 文件，
默认位于 `~/.local/share/lcode/sessions/<name>.json`（可用 `LCODE_HOME` 覆盖根目录）。
每轮对话结束后自动保存；启动时若无 `--session`，会新建一个带时间戳的会话。

CLI：`--session <name>` 打开或创建、`--resume` 恢复最近一个、`--list-sessions` 列出并退出。

REPL 内命令：

```
/new [name]      新建会话（自动保存当前会话）
/list            列出已保存的会话
/switch <name>   保存当前会话并切换到另一个
/delete <name>   删除某个会话（不能删除当前会话）
/session         显示当前会话名
/save            立即保存当前会话
/usage           显示当前会话的 token 用量与缓存命中率
/exit            保存并退出（等价于 Ctrl-D）
/help            显示命令帮助
```

> 恢复会话时会保留历史对话，但第一个 system 消息会被替换为当前启动配置生成的 system
> prompt（因此 `LCODE.md`、`--append-system-prompt` 等改动会即时生效）。

## 用量与缓存

每次 API 请求后，lcode 会从响应的 `usage` 中统计用量并输出到 **stderr**（暗色行），
因此不会污染对话内容，也不会被写入会话历史、影响前缀缓存：

```
[usage] prompt 1200 (cache hit 1100 / miss 100, 91.7%), completion 12, total 1212
```

`/usage` 显示当前会话的累计值；命中率 = `cache_hit / (cache_hit + cache_miss)`。统计同时
兼容 DeepSeek 的 `prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` 与 OpenAI 风格的
`prompt_tokens_details.cached_tokens`。累计值仅保存在进程内，重启后从零开始。

进入 REPL 后直接输入需求，例如：

```
> 列出当前目录，然后读取 CMakeLists.txt 并解释构建配置
```

按 `Ctrl-D` 退出。

## 打断与纠偏

当模型正在请求 API 或执行工具、而你发现它耗时跑偏时，按一次 `ESC` 即可打断：

- 正在进行的 HTTP 请求会被中止，正在运行的 `run_shell` 子进程会被结束，未完成的工具调用会以“interrupted by user”占位补全，保证会话消息序列完整；
- 随后会提示 `redirect>`，输入一条新的指示（例如“别用 sed，改用 patch”）让模型换思路，回车即跳过；
- 新的指示会作为一条 user 消息追加进当前会话，模型带着原有上下文按新方向继续；
- 在 `--non-interactive` 模式下不弹提示，打断后直接结束本轮。

系统会把被中断的回合完整保存进 session，恢复会话后可继续接着处理。
