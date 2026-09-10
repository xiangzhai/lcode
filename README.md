# lcode

一个用 C 编写的命令行编码 agent，对接 DeepSeek 的 OpenAI 兼容 Chat Completions API，支持多轮对话与函数调用（工具调用）。

## 功能

- 交互式 REPL，多轮上下文
- Function Calling 工具循环，模型可连续调用多个工具
- 内置工具：
  - `read_file` —— 读取文本文件
  - `write_file` —— 写入（覆盖）文本文件
  - `list_dir` —— 列出目录内容（类型与大小）
  - `run_shell` —— 执行 shell 命令并返回输出与退出码

## 依赖

构建需要 C11 编译器、CMake 以及以下开发库：

| 依赖 | 用途 | 最低建议版本 |
| --- | --- | --- |
| CMake | 构建系统 | 3.16 |
| GCC / Clang | C 编译器（需支持 C11） | GCC 9 / Clang 10 |
| pkg-config | 查找依赖库 | 0.29 |
| libcurl（开发包） | HTTP 请求 | 7.58 |
| json-c（开发包） | JSON 解析与序列化 | 0.13 |
| GNU Readline（开发包） | 交互式输入与历史 | 8.0 |

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

## 运行

```sh
./build/lcode
```

命令行参数（会覆盖对应环境变量）：

```
--model <name>         指定模型，默认 deepseek-v4-flash
--base-url <url>       指定 API 基础地址
--max-iterations <n>   单轮最多工具调用轮数，默认 16
-h, --help             显示帮助
```

示例：

```sh
./build/lcode --model deepseek-v4-flash
```

进入 REPL 后直接输入需求，例如：

```
> 列出当前目录，然后读取 CMakeLists.txt 并解释构建配置
```

按 `Ctrl-D` 退出。
