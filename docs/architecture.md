# StreamForge 架构说明（M1 版）

本文记录 M1 里程碑的架构与关键实现决策。M2–M4 交付时随实现更新。

## 1. 系统形态

StreamForge 由两个可执行文件与一个静态库组成（M1 状态）：

| 产物 | 说明 |
| --- | --- |
| `streamforge-server` | 输入目录轮询、两阶段导入流水线、恢复与归档；HTTP 服务在 M3 加入 |
| `streamforgectl` | 配置校验、导入、状态、查询、事件确认、数据库维护 |
| `streamforge-tests` | Catch2 单元与集成测试，支持按标签选择（`streamforge-tests "[csv]"`） |
| `libtlmcodec` | M2 交付：TLM 协议 C11 编解码库（协议草案见 docs/tlm-protocol.md） |

## 2. 模块与依赖方向

```text
core ← config ← storage ← ingest ← server / ctl
         ↑          ↑
         └── date/tz, yaml-cpp, nlohmann/json   SQLite, OpenSSL(crypto), libuuid
```

- **core**：`Result<T>`/`Error`/`ErrorCode`、`TimePointUs`（UTC 微秒）、ISO 8601 解析
  （date/tz + DST 规则）、UUIDv4（libuuid）、spdlog 封装（text/json）、`BoundedQueue`、
  路径规范化与控制字符清洗。
- **config**：YAML 严格加载 + 语义校验（FR-CFG-002 全量 M1 子集）、未知可选字段告警、
  相对路径基于配置文件目录解析、`config_version`（FNV-1a 64 规范化序列哈希）、时区预解析。
- **storage**：SQLite RAII（`Db`/`Stmt`/`Txn`）、带校验和的向前迁移（migrations/*.sql 编译期
  嵌入）、`Store` 门面（单连接互斥 = 单写者模型）。WAL + foreign_keys=ON；BEGIN IMMEDIATE
  指数退避（25ms 起，×2，上限 500ms，共 8 次）。
- **ingest**：文件身份、目录发现、CSV/JSONL 流式解析、基础校验、两阶段导入流水线、
  归档/隔离。

## 3. 关键设计决策

### 3.1 文件级两阶段暂存（错误率语义，用户裁决 ④）

大文件必须分批提交（有界内存），而"错误率超限回滚"要求文件级原子性，因此：

1. **Stage 1（PROCESSING_STAGE1）**：流式解析 + 基础校验，结果按 `batch_size` 分批写入
   `staging_samples` 暂存表；每批提交时同步推进文件检查点（字节偏移 + 行号）并更新计数。
   暂存期不产生 samples/聚合/事件。
2. **EOF 裁决**：`error_rate = business_errors / max(total_records, error_rate_min_records)`
   （小文件用最小分母，避免 1/1=100% 的误放大）。超阈值 → 单事务删除该文件暂存数据并
   标记 QUARANTINED（`finalize_quarantine`），随后物理移入隔离目录并生成
   `.error.json`（≤100 条样例错误，原始数据 ≤256 字节且去除控制字符）。
   未超阈值 → 标记 VALIDATED。
3. **Stage 2（PROCESSING_STAGE2）**：从暂存表按 seq 游标分批读取，接受记录生成 samples
   （M1 直接入库；校准/单位换算/去重在 M2 加入 stage 2 与规则、聚合一致），样本插入、
   暂存游标推进、状态更新同一事务。完成后单事务清空暂存并标记 COMPLETED，再补归档。

崩溃恢复：Stage 1 从字节偏移重读，Stage 2 从暂存 seq 游标继续；两阶段都满足
"检查点只在业务事务提交后推进"。恢复入口：`recover_pending()`（server 启动时调用），
并对 COMPLETED/QUARANTINED 但仍留在输入目录的文件补做归档/隔离移动。

### 3.2 文件身份（FR-IN-003）

`SHA-256(规范化绝对路径 \0 size \0 mtime_us \0 SHA-256(前 64KiB))`，SHA-256 使用
OpenSSL EVP。同一 identity_hash 再次出现且状态终态 → 跳过；同路径不同内容 → 旧任务
标记 SOURCE_CHANGED，新文件作为新版本处理；文件消失 → MISSING。

### 3.3 目录发现（FR-IN-002）

轮询 + 双条件：`.ready` 后缀立即处理（格式取去除 `.ready` 后的扩展名）；否则要求
连续两次扫描 size+mtime 不变且静默期达标。隐藏文件、`.tmp/.part/.partial/.swp/~`、
符号链接、未知扩展名一律跳过并记录 debug 日志。`.tlm` 在 M1 被发现但显式推迟到 M2。

### 3.4 时间处理（FR-VAL-002）

C++17 无 chrono 时区库，采用 Howard Hinnant date/tz + **系统 IANA 数据库**
（/usr/share/zoneinfo，USE_SYSTEM_TZ_DB=ON）。规则：

- 带偏移 → 直接换算；Unix 毫秒整数 → 直接换算；无偏移 → 设备时区解释。
- DST 不存在时间（spring gap）→ 拒绝（ValidationTimeInvalid）；歧义时间按
  `ambiguous_time_policy: earlier|later` 取早/晚偏移。
- 闰秒 `:60` 预扫描改写为 `:00` 并在解析后加 1 分钟，附 `leap_second=true` 标签。
- 校验分层：无法解析/缺字段 = 严重格式错误（不计错误率）；未知设备/指标、单位不可
  转换、时间越窗（未来 >10 分钟、早于 history_range）、quality 非法、标签超限 =
  业务错误（计入错误率）。

### 3.5 数据模型

`config_version`（uint64，FNV-1a 64 规范化序列化）记录在 source_files 与每条 sample 上，
实现"查询结果可追溯到处理时配置版本"。跨进程对象（文件、事件）用 UUIDv4 字符串；
整数主键仅内部使用。样本去重索引（device,metric,sequence 部分唯一索引与
device,metric,event_time_us,normalized_value 部分唯一索引）已在迁移 0001 中就位，
M2 启用写入。

### 3.6 依赖与获取方式

| 依赖 | 方式 | 版本固定 |
| --- | --- | --- |
| ZLIB / OpenSSL / SQLite3 / libuuid | 系统库 `find_package(... REQUIRED)`，缺失即配置失败 | 系统版本 |
| yaml-cpp / nlohmann/json / spdlog / Catch2 / date | FetchContent | 0.8.0 / v3.11.3 / v1.14.1 / v3.7.1 / v3.0.1 |

FetchContent 缓存默认在源树 `.deps/cache`（gitignore），首次拉取后离线可重复构建；
源树位于不支持全部文件操作的挂载盘（如 WSL 下 /mnt/e）时用
`-DFETCHCONTENT_BASE_DIR=<dir>` 迁移缓存，构建目录也应放在 Linux 原生文件系统。

## 4. 错误处理与日志

- 统一 `Result<T>` + `Error{code, message, context}`；错误码见
  `include/streamforge/core/result.hpp`，不以日志替代错误返回。
- 日志：spdlog，组件即 logger 名（server/ingest/storage/...），text 与 json 两种格式；
  json 为自定义 formatter（ts/level/component/thread/msg，消息 JSON 转义）。
  轮转文件 + 控制台，级别与轮转参数来自配置。

## 5. 已知限制（M2）

- HTTP API、热加载、规则/表达式引擎、数据保留清理：M3 交付；`streamforgectl
  report/replay` 未提供（M3/M4）。
- server 目录扫描为顺序处理（单文件内串行，多文件排队于扫描循环）；BoundedQueue
  已实现并有并发测试，多工作线程并行化在 M3 与规则引擎一起接入。
- 隔离/归档物理移动失败时 DB 状态为准，移动由下一次恢复流程重试（日志告警）。
- CSV 解析要求秒字段存在（ISO 8601 子集，`YYYY-MM-DDTHH:MM:SS[.frac][Z|±HH:MM]`），
  与 FR-FMT-001/002 的示例一致。

## 6. M2：二进制接入与时序处理

### 6.1 libtlmcodec（C11）

`c/tlmcodec/` 提供 TLM 二进制协议的 C11 编解码：文件头（CRC-32/ISO-HDLC 校验）、
帧解析（sync/类型/flags/负载上限 4MiB/负载 CRC）、zlib 解压、TLV 字典（必填/重复/
字符集校验，未知 TLV 跳过）、严格文件内序号、损坏帧后最多 1 MiB 的同步字扫描。
所有整数逐字节小端装配，天然对齐安全。协议全文见 `docs/tlm-protocol.md`。

C++ 侧 `ingest/tlm_parser.hpp` 流式封装：文件头校验、检查点偏移（帧边界）、序号
状态（持久化于 checkpoint 的 stage2_cursor 列）、恢复后数据标记 `quality=Suspicious`
并附 `resync=true` 标签（FR-FMT-004）。

### 6.2 时序处理链（stage 2 正式流水线）

stage 2 从暂存表按游标分批读取，每条记录依次经过：

1. **校准**（FR-VAL-003）：分段线性 `[min,max)` 闭开区间，未匹配按配置拒绝或透传。
2. **单位换算**（FR-VAL-004）：内置 C/F/K、Pa/kPa/MPa/bar、mm/s/cm/s/m/s、W/kW/MW、
   L/min/m3/h/m3/s 仿射换算（base = value*scale + offset），检测溢出与物理无效值
   （低于绝对零度、负压力/流速/功率/流量），库表位于 `core/units.hpp`（配置校验与
   运行时共用）。之后应用配置的 valid_min/valid_max。
3. **去重**（FR-ORD-001，⑥ 裁决）：LRU 热点缓存 + SQLite 唯一索引权威。键：有序号
   `(device, metric, sequence)`；无序号 `(device, metric, event_time, normalized_value)`
   （normalized_value 为 IEEE 754 位模式十六进制，-0/0 归一）。迁移 0003 将无序号
   唯一索引收窄至 `sequence IS NULL`，避免误伤有序号记录。`INSERT OR IGNORE` 为
   最终防线。
4. **水位线重排**（FR-ORD-002）：每设备 `wm = max_seen - allowed_lateness`；晚于
   水位线的样本立即发射并标记 `kLate`；缓冲区有记录数/字节数上限，触顶强制推进
   并标记 `kForcedFlush`。**每批提交前强制 drain**——"cursor 前进 ⟹ 样本已持久化"，
   崩溃重放精确无丢失。
5. **缺失检测与插值**（FR-MIS-001/002）：相邻有效样本（排除 quality=2）间隔超过
   `period+jitter` 产生缺失区间（迁移 0002 唯一索引保证重放幂等）；`previous/linear`
   插值在 `max_gap` 内生成 `kSynthetic` 样本（⑦a：计入聚合 synthetic_count，区间
   始终保留；异常规则是否使用由规则配置决定）。
6. **窗口聚合**（FR-AGG-001/003）：固定窗口（配置秒数，UTC 对齐）在水位线越过
   窗口末端时关闭——统计从 samples 表重算（Welford 方差、Kahan 求和、最近秩精确
   分位数），幂等且崩溃安全；`(device,metric,type,start,version)` 版本化，迟到数据
   在 `window_correction: true` 时废弃旧版本并追加新版本，默认不动已关闭窗口。
   **滚动窗口**（FR-AGG-002）由 `processing/rolling.hpp` 的增量 `RollingSeries`
   （计数/时长双模式）支撑，供 M3 规则引擎使用。

### 6.3 stage 2 提交边界

每个暂存批次一个事务：样本批 + 暂存游标推进（`commit_stage2_batch`）；窗口关闭、
迟到修正在提交后执行（读取已提交数据）。崩溃重放：从游标重放 → 去重去重余样本 →
窗口重算覆盖 → 结果收敛一致（重放幂等，验收 §14.2 要求）。
