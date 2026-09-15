# StreamForge

工业设备遥测数据处理系统：从 CSV / JSON Lines / TLM 二进制文件接入传感器数据，
完成校验、校准、单位换算、去重、乱序重排、窗口聚合、派生指标与异常检测，
结果持久化到 SQLite，并通过 HTTP API 与命令行工具提供查询、报表和运维能力。

当前进度：**M1（工程基础与数据接入）已完成**——CMake/Clang 20 工具链约束、核心类型、
配置加载与校验、日志、SQLite 迁移、文件发现/身份、CSV/JSONL 流式解析、基础校验、
两阶段导入流水线（暂存 + 错误率隔离 + 检查点恢复）、归档/隔离、CLI。
M2（TLM/时序处理）、M3（规则与 API）、M4（报表/性能）按里程碑推进。

## 1. 环境要求

- Linux x86_64，**Clang 20**（`clang-20` / `clang++-20`，CMake 配置阶段强制校验，
  非此工具链直接报错终止）。
- CMake ≥ 3.20，Ninja（推荐）。
- 系统库：zlib、OpenSSL、SQLite3、libuuid（`find_package(... REQUIRED)`，缺失即失败）。
- 时区数据：系统 IANA 数据库（Ubuntu：`tzdata`，即 `/usr/share/zoneinfo`）。
- 推荐在 WSL2 Ubuntu 24.04 中开发运行；源码目录放在 Linux 原生文件系统时体验最佳
  （位于 Windows 挂载盘时，构建目录与依赖缓存请移到原生文件系统，见 §2.3）。

一键安装工具链（Ubuntu 24.04，root 或免密 sudo）：

```bash
bash scripts/setup-wsl.sh
```

## 2. 构建与测试

### 2.1 标准构建

```bash
CC=clang-20 CXX=clang++-20 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

或直接：

```bash
bash scripts/build.sh
```

构建产物（`build/` 下）：`streamforge-server`、`streamforgectl`、`streamforge-tests`。

### 2.2 测试

```bash
ctest --test-dir build --output-on-failure        # 全量
build/streamforge-tests "[csv]"                   # 按标签选择（Catch2 语法）
build/streamforge-tests "[pipeline][storage]" -s  # 多标签、展示成功用例
```

### 2.3 源码在 Windows 挂载盘（/mnt/e）时

CMake 的部分文件操作在 DrvFS 上会失败，请把构建目录与依赖缓存放在 WSL 原生目录：

```bash
CC=clang-20 CXX=clang++-20 cmake -S /mnt/e/Workspace/StreamForge -B ~/sf-build \
    -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_BASE_DIR=$HOME/sf-deps
cmake --build ~/sf-build --parallel
```

依赖缓存默认位于源树 `.deps/cache`（gitignore），首次联网拉取后即可离线重复构建；
用 `-DFETCHCONTENT_BASE_DIR=` 可将其迁移到任意本地目录。所有 FetchContent 依赖均
固定 tag/commit（见 `cmake/Dependencies.cmake`），不允许浮动分支。

### 2.4 Sanitizer 与覆盖率

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSTREAMFORGE_SANITIZE=asan,ubsan
cmake --build build-asan --parallel && ctest --test-dir build-asan --output-on-failure

# LLVM 源代码覆盖率（llvm-cov）
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DSTREAMFORGE_COVERAGE=ON
cmake --build build-cov --parallel
cd build-cov && LLVM_PROFILE_FILE=sf.profraw ctest -R streamforge-all
llvm-profdata-20 merge -sparse sf.profraw -o sf.profdata
llvm-cov-20 report ./streamforge-tests -instr-profile=sf.profdata
```

## 3. 快速上手

```bash
# 1) 用示例配置启动服务（监听输入目录 data/input）
mkdir -p data/input && build/streamforge-server --config config/example.yaml &

# 2) 或直接用 CLI 导入示例数据
build/streamforgectl import examples/data/pumps.csv      --config config/example.yaml
build/streamforgectl import examples/data/sensors.jsonl  --config config/example.yaml

# 3) 查询
build/streamforgectl status  --config config/example.yaml
build/streamforgectl devices list --config config/example.yaml --tag line=A
build/streamforgectl samples query --config config/example.yaml \
    --device pump-07 --metric bearing_temp \
    --from 2026-08-01T00:00:00Z --to 2026-08-02T00:00:00Z --output json

# 4) 数据库维护
build/streamforgectl db migrate --config config/example.yaml
build/streamforgectl db check   --config config/example.yaml
```

成功处理的文件归档到 `data/archive/YYYY/MM/DD/`；错误率超阈值（默认 10%）或无法解析的
文件进入 `data/quarantine/`，旁边生成同名 `.error.json` 诊断报告（≤100 条样例错误）。

## 4. CLI 速查

| 命令 | 说明 |
| --- | --- |
| `validate-config --config <f>` | 配置结构与语义校验，不落盘 |
| `import <path> [--recursive] [--wait]` | 导入文件/目录（目录按路径字典序），失败不中断批次 |
| `status --config <f>` | 数据库概况（schema/样本数/文件状态分布） |
| `devices list [--tag k=v]` | 设备清单（来自配置快照） |
| `samples query --device --metric --from --to` | 样本查询（`--output json` 可选） |
| `incidents list [--state open] [--severity high]` | 事件查询（M1 表为空属正常） |
| `incidents ack <id> --by <name> [--comment]` | 确认事件并写入历史与审计 |
| `db migrate` / `db check` | 迁移到最新版本 / 完整性与外键检查 |

退出码：`0` 成功，`1` 业务失败，`2` 参数错误。`--output json` 时进度文本走 stderr，
stdout 只有 JSON。

## 5. 配置要点

完整注释示例见 `config/example.yaml`，配置参考见 `docs/architecture.md` §3。要点：

- 相对路径基于配置文件所在目录解析。
- `pipeline.max_error_rate`：文件级业务错误率阈值（默认 0.10），
  `error_rate_min_records`：小文件误差分母（默认 100）。
- 时间字面量格式：`500ms` / `10s` / `5m` / `2h` / `7d`。
- 未知可选字段告警不阻断；`schema_version` 主版本不识别直接拒绝。
- 校准为分段线性 `slope*raw+intercept`，区间 `[min, max)` 闭开、不得重叠。

## 6. 故障排查

| 现象 | 处理 |
| --- | --- |
| CMake 报 "requires the Clang 20 toolchain" | 用 `CC=clang-20 CXX=clang++-20` 重新配置；`apt.llvm.org` 提供安装 |
| CMake 报 "Could NOT find ZLIB/OpenSSL/SQLite3/libuuid" | 安装系统开发包：`zlib1g-dev libssl-dev libsqlite3-dev uuid-dev` |
| configure_file "Operation not permitted" | 源码在 /mnt/e：按 §2.3 把 build 目录与 `FETCHCONTENT_BASE_DIR` 移到 Linux 原生 fs |
| 首次配置很慢 | FetchContent 正在拉取固定版本的第三方库；完成后缓存于 `.deps/cache`，离线可重建 |
| 数据库 "database is locked" | server 与 CLI 同时写库；CLI 的写命令会重试（指数退避），长查询不会永久阻塞 |
| 文件既不导入也不隔离 | 检查是否 `.hidden`/`.tmp`、扩展名是否受支持，或静默期未到（`quiet_period`） |
| 重启后重复导入被跳过 | 这是按文件身份（路径+大小+mtime+前 64KiB SHA-256）去重的预期行为；内容变化视为新版本 |

## 7. 目录结构

```text
├── CMakeLists.txt / cmake/       构建系统与工具链约束
├── include/streamforge/…         公共头（core/config/storage/ingest）
├── src/…                          实现；server_main.cpp / ctl_main.cpp
├── migrations/0001_initial.sql   SQLite 迁移（编译期嵌入 + 校验和）
├── c/tlmcodec/                   M2：C11 TLM 编解码库
├── tests/{unit,integration,…}    Catch2 测试
├── config/example.yaml           示例配置
├── examples/data/                示例输入（CSV/JSONL）
├── docs/                         架构说明、TLM 协议草案
└── scripts/                      setup-wsl.sh / build.sh / test.sh
```

## 8. 文档

- [docs/architecture.md](docs/architecture.md)：模块职责、两阶段暂存设计、关键决策、已知限制。
- [docs/tlm-protocol.md](docs/tlm-protocol.md)：TLM 二进制协议（草案，含 CRC32 参数与
  可人工核对的十六进制示例），M2 实现前供审查。
- 需求来源：`StreamForge_需求规格说明书.md`（仓库根目录）。
