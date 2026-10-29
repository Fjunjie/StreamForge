# M4 验收记录

日期：2026-09-20 · 分支：`M4` · HEAD：见 git log

## 1. 构建矩阵（Clang 20.1.8，Ubuntu 24.04）

| 配置 | 命令 | 结果 |
| --- | --- | --- |
| Debug（默认） | `cmake -S . -B build -DSTREAMFORGE_BUILD_TESTS=ON && cmake --build build` | ✅ 通过 |
| Release | `cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release ...` | ✅ 通过 |
| AddressSanitizer | `STREAMFORGE_SANITIZE=asan` | ✅ 通过 |
| ASan+UBSan | `STREAMFORGE_SANITIZE=asan,ubsan` | ✅ 通过 |
| clang-tidy-20 | `cmake --build build --target tidy` | ✅ clean |

测试：`build/streamforge-tests` — 177 用例 / 1544 断言全部通过（含 ASan 运行）。

## 2. 功能验收对照

| 需求 | 状态 |
| --- | --- |
| 三种格式接入、两阶段流水线、断点恢复（M1/M2） | ✅（含 M2 验收） |
| 校准/单位换算/去重/水位线/缺失检测/插值/固定+滚动窗口（M2） | ✅ |
| 表达式语言（解析/求值/类型/Null 语义/惰性 IF/深度限制） | ✅ |
| 派生指标（拓扑排序、环检测、缺失策略、年龄上限、事件驱动） | ✅ |
| 规则状态机（五态、事件时间驱动、冷却、合并重开、重放幂等） | ✅ |
| 事件生命周期（开启/关闭/重开/确认/审计/RULE_REMOVED） | ✅ |
| HTTP API（FR-API-001..004：查询/确认/管理/认证/分页/限流） | ✅ |
| 命令行工具（validate-config/import/status/devices/samples/incidents/db/report） | ✅ |
| 配置热加载（SIGHUP + admin 接口；失败保持旧配置；仅重启字段拒绝） | ✅ |
| 审计日志（重载/确认/导入/清理） | ✅ |
| 数据保留（分批删除、保护开放事件与状态） | ✅ |
| 优雅停机（SIGTERM/SIGINT，HTTP 停止、批次收尾、线程汇合） | ✅ |
| CSV/JSON 报表（summary/incident/quality；公式防护；原子写出） | ✅ |
| 运行指标（Prometheus /metrics：计数器/仪表） | ✅ |
| 数据生成器（streamforge-gen，参数化可复现） | ✅ |
| 模糊测试入口（TLM 编解码、表达式解析；libFuzzer） | ✅（构建开关 `STREAMFORGE_BUILD_FUZZ=ON`） |

## 3. 性能与稳定性测试（可重复命令）

```bash
# 生成 1000 万条 JSONL（10 设备 × 20 指标，1% 重复，10% 乱序，1% 错误）
./build/streamforge-gen --records 10000000 --devices 10 --metrics 20 \
  --duplicates 0.01 --late 0.10 --errors 0.01 --seed 42 --output perf.jsonl

# 导入并计时（吞吐与各阶段计数见 /metrics 与服务日志）
/usr/bin/time -v ./build/streamforgectl import perf.jsonl --config config/example.yaml --wait

# 查询延迟（1M 样本库中单设备单指标 24h 窗口，P95 ≤ 500ms 目标）
for i in $(seq 1 100); do
  curl -s "http://127.0.0.1:8080/api/v1/samples?device=dev-01&metric=temp&from=<t0>&to=<t1>" -o /dev/null \
    -w "%{time_total}\n"
done | sort -n | awk '{a[NR]=$1} END {print "P50="a[int(NR*0.5)]" P95="a[int(NR*0.95)]" P99="a[NR]}'

# 两小时稳定性：持续生成 + 导入 + 查询循环，监控 RSS 与日志错误
```

**执行状态**：吞吐 / 内存 / 查询 P95 与两小时稳定性测试需要在 4 核 / 8 GiB / SSD 的
目标硬件上执行，本验收记录提交时标记为**待执行**；命令与判据如上
（≥20,000 条/秒、RSS ≤ 512 MiB 且不随处理量线性增长、P95 ≤ 500 ms、停机 ≤ 30 s）。

## 4. 已知限制

- 指标 id 含 `-` 时无法在规则/派生指标表达式中引用（`-` 为减号）；建议使用
  字母数字或 `.` 命名。
- 规则窗口函数（AVG/DELTA/RATE）的滚动状态为内存环形缓冲（24h / 每序列 8192 条上限），
  重启后从新样本重建；窗口跨度超过缓冲的历史会按数据不足处理（规则保持原状态）。
- 缺失规则（type=absence）的类型字段已解析并持久化，其专门的"无样本触发"语义
  由表达式 + 缺失检测区间组合表达，专用语法待后续里程碑补充。
- 两小时稳定性与性能验收在目标硬件上执行后回填第 3 节结果。

## 5. 审计与质量

- PR #3（M3）经 code-hawk-uat 审计 + 双向复核，22 处意见全部处置（必须修/应该修已修，
  暂缓项记录在案）；修复过程另发现 4 处新问题一并修复。
- 全部 SQL 使用预编译语句与绑定参数；路径规范化并限制在配置目录内；
  表达式/解析器有深度与边界防护；`_exit` 未使用。
