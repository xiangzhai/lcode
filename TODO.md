# lcode TODO

对当前代码库（`src/`）的功能缺口梳理，按对日常使用的影响从大到小排列。
每条尽量附上代码依据，方便判断优先级和落点。

## 一、工具能力（影响最大）

- [x] 增加精确编辑工具 `edit_file`（old_string/new_string 替换）或 apply-patch。
      目前只有 `write_file` 整文件覆盖（`src/tools.c:tool_write_file`），
      改一个函数要重写整个文件，token 浪费大、出错率高。这是当前最值钱的一块。
- [ ] 内置 `search` / `glob` 工具。现在内容搜索和文件匹配全靠模型拼 `run_shell`
      里的 `grep`/`find`，跨平台不可靠、输出也难控。
- [ ] `read_file` 支持分页：`MAX_READ_SIZE (512KB)` 截断且没有 `offset`/`limit`
      参数（`src/tools.c`），大文件读不全。
- [ ] 并行执行工具调用。`run_turn` 里是串行 `for (t = 0; t < n; t++)`
      （`src/agent.c`），模型一次返回多个工具会一个个跑。
- [ ] 给 `run_shell` 加超时。目前只有 curl 的 600s，命令卡死只能靠 ESC/SIGINT
      （`src/tools.c:tool_run_shell`）。

## 二、网络/API 层

- [x] 流式输出（SSE）。`src/http.c:write_cb` 用 `CURLOPT_WRITEFUNCTION`
      收完整 body 才解析，没有 SSE，只能看 spinner 等整段返回。体验上差距最明显。
      已实现：`http_post_json_stream()` 按行解析 `data:` 事件，`run_turn` 边收边
      打印思考/正文并重组流式工具调用；默认开启，`--no-stream` 可回退。
- [ ] 重试 + 退避。429/5xx/网络抖动目前直接报错（全仓库无 retry 相关代码）。
- [ ] 上下文管理：截断或摘要压缩。`messages` 只增不减，长会话最终会撞模型上下文上限。
- [ ] 补全请求参数：`max_tokens`、`top_p` 等可配置项。

## 三、可靠性与可脚本化

- [ ] 非交互模式退出码。`agent_run` 在 `--prompt` 后无论
      `run_turn_with_redirect` 成败都 `return 0`（`src/agent.c:650-655`），
      CI 里无法感知失败。
- [ ] readline 历史落盘。只有 `add_history`，没有 `read_history`/`write_history`，
      每次启动输入历史清空。

## 四、会话与配置

- [ ] 配置文件支持（如 `~/.config/lcode/config`），减少对 env + 命令行的依赖。
- [ ] 会话压缩 / 导出（摘要、导出 markdown）。

## 五、可扩展性

- [ ] 接入 MCP（Model Context Protocol）等外部工具服务器。
      工具集硬编码在 `tools_schema()` 的固定 6 个（`src/tools.c`），无法扩展。
- [ ] 自定义工具 / hooks（pre/post tool）。

## 六、安全

- [ ] 信任策略：目录被信任后 `run_shell` 无任何策略，`rm -rf`、`curl | sh`
      都不经确认；目录级信任粒度偏粗。
- [ ] 加 `--untrust` 撤销命令（目前只有 `--list-trusted`）。
- [ ] 统一提示风格：启动信任用 `[y/N]` 文本（`src/trust.c:trust_ask`），
      工具越界用方向键菜单（`src/tools.c:trust_prompt`）；
      面向用户的中文提示只在 `src/agent.c:run_turn_with_redirect` 出现一句，其余全英文。

## 七、工程

- [ ] 测试接入构建：`test/t.c` 是占位符，没进 `CMakeLists.txt`，也没有 CI。
- [ ] 加 ASan/UBSan 构建目标。

## 建议的优先级

先做三件：**① `edit_file` 精确编辑 → ② 流式输出(SSE) → ③ 非交互退出码 + 重试**。
前者直接提升改代码的准确率，后两者提升可用性和可自动化程度。
