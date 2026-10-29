# 配置参考

完整注释示例见 `config/example.yaml`。相对路径基于配置文件所在目录解析；
未知可选字段产生告警不阻断；`schema_version` 主版本不识别直接拒绝。
时间字面量格式：`500ms` / `10s` / `5m` / `2h` / `7d`。

## server

```yaml
server:
  bind: 127.0.0.1          # 监听地址（只读接口默认仅本地/受信网络）
  port: 8080
  admin_token_file: /etc/streamforge/admin-token   # 可选：管理令牌文件（建议 0600）
  max_query_range: 24h     # 样本/聚合查询的最大时间跨度（FR-API-003）
```

## directories / database

```yaml
directories:
  input: data/input        # 输入目录（服务监控；admin/import 的路径白名单根）
  archive: data/archive    # 成功归档（YYYY/MM/DD）
  quarantine: data/quarantine
  reports: data/reports
database:
  path: data/streamforge.db
```

## pipeline

```yaml
pipeline:
  workers: 4
  queue_capacity: 100000
  batch_size: 1000
  allowed_lateness: 5m     # 水位线乱序容忍
  scan_interval: 2s
  quiet_period: 5s         # 文件静默期
  max_error_rate: 0.10     # 文件级业务错误率阈值
  error_rate_min_records: 100
  shutdown_timeout: 25s
  history_range: 7d        # 早于该范围的时间戳按业务错误拒绝
  ambiguous_time_policy: earlier   # DST 歧义时间
  window_correction: false # 迟到数据是否重开已关闭窗口（FR-AGG-003）
```

`workers`、`queue_capacity`、`database.path`、`server.bind/port` 为**仅重启字段**：
热加载时变更会被拒绝并提示需要重启（FR-CFG-003）。

## windows

```yaml
windows: [60, 300]   # 固定窗口尺寸（秒）；聚合键含 window_type 与版本
```

## log

```yaml
log:
  level: info        # trace|debug|info|warn|error|critical
  format: text       # text|json
  file: ""           # 空表示仅 stdout
  max_size_mb: 50
  max_files: 5
  quiet_console: false
```

## metrics（基础指标）

```yaml
metrics:
  - id: temp
    canonical_unit: C
    input_units: [C, F, K]     # 允许的输入单位（导入时换算到 canonical_unit）
    valid_min: -50             # 可选有效值域
    valid_max: 200
    expected_period: 1s        # 可选：缺失检测周期
    jitter: 100ms              # 可选：周期抖动
    max_gap: 10s               # 可选：插值最大间隔
    interpolation: linear      # none|previous|linear
```

## devices

```yaml
devices:
  - id: dev-01
    timezone: Asia/Shanghai
    tags: {line: A, area: shop1}   # 规则的标签选择器按此匹配
    metrics: []                    # 空 = 允许全部（含派生指标）
```

## calibrations（分段线性校准）

```yaml
calibrations:
  - device: dev-01
    metric: temp
    reject_unmatched: false
    segments:
      - {min: 0, max: 100, slope: 0.5, intercept: -10}
```

同一 (device, metric) 重复配置报错；区间闭开 `[min, max)` 且不得重叠。

## derived_metrics（派生指标，FR-DER-001/002）

```yaml
derived_metrics:
  - id: pump_efficiency
    unit: percent
    expression: "clamp(flow_rate * outlet_pressure / max(input_power, 0.001) * 0.027, 0, 100)"
    missing_policy: recent   # null|recent|skip：缺少输入时输出空值/用最近值/跳过
    max_input_age: 5s        # 'recent' 回退值的最大年龄（0 = 不限）
```

- 表达式在配置加载时解析为 AST 并完成引用与依赖图检查（引用必须是基础或派生指标，
  依赖图不得有环）；运行期无 eval、无脚本、无外部进程。
- 派生指标随输入样本事件驱动计算；派生样本与原始样本同模型，带 `derived` 标记。
- 除零、负数开方、非有限结果计为计算错误（跳过该次输出并计数），不会使服务崩溃。
- 表达式语言支持 `+ - * / %`、比较、`AND/OR/NOT`（短路）、`IF(c,a,b)`（惰性）、
  `ABS/MIN/MAX/CLAMP/SQRT/POW`、`AVG(metric,d)/DELTA(metric,d)/RATE(metric,d)`、
  时长字面量 `5m/30s/1h`。指标 id 含 `-` 时无法在表达式中引用，请使用 `.`
  或字母数字命名。

## rules（异常规则，FR-RULE-001..003）

```yaml
rules:
  - id: temp-high
    type: threshold          # threshold|duration|rate|absence|composite
    severity: warning        # info|warning|high|critical
    trigger: "temp > 80 or RATE(temp, 5m) > 2"
    recovery: "temp < 75"    # 可选；缺省为触发条件取反
    duration: 2m             # 条件须持续满足的时长（0 = 立即触发）
    cooldown: 10m            # 事件关闭后的抑制期
    merge_interval: 5m       # 短暂恢复后在此窗口内再次触发则重开原事件
    devices:
      line: A                # 标签选择器（全部命中才评估；缺省评估全部设备）
```

- 状态机：NORMAL → PENDING → FIRING → RECOVERING → COOLDOWN，全部按事件时间推进；
  同一 (rule, device) 任意时刻至多一个开放事件；状态持久化于 `rule_states`，重启恢复。
- 指标缺失时规则保持当前状态（既不触发也不恢复）；`has_data=false`。
- 规则被删除时，其开放事件以 `RULE_REMOVED` 关闭。
- 事件确认（API/CLI）写入事件历史与审计日志，不改变规则状态。

## retention

```yaml
retention:
  samples_days: 90
  aggregates_days: 365
  incidents_days: 730
  audit_days: 365
```

清理任务每小时运行并写入审计日志；分批删除（单事务上限 10000 行）；
开放事件、规则状态与活动检查点永不清理。

## 规则与派生指标的热加载

`SIGHUP` 或 `POST /api/v1/admin/reload` 触发：新配置先完整校验，再原子替换；
校准、单位、规则、派生指标与保留策略可热加载；仅重启字段变更被拒绝；
任何失败都保持旧配置继续工作。
