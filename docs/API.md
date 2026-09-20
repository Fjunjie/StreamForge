# HTTP API 参考（M3/M4 实现状态）

API 前缀 `/api/v1`；请求与响应为 UTF-8 JSON；时间字段为 UTC ISO 8601（保留微秒）。
错误响应统一为 `{"code": "<error_code_name>", "message": "...", "request_id": "..."}`。
列表接口游标分页，`limit` 默认 100、最大 1000；客户端可通过 `X-Request-Id` 头传入请求标识，
未提供时服务端生成并在响应头回显。

## 无需认证

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/health` | 进程存活（不访问数据库），`{"status":"ok"}` |
| GET | `/ready` | 数据库与配置就绪状态，未就绪返回 503 |
| GET | `/metrics` | Prometheus 文本格式运行指标（v0.0.4） |
| GET | `/api/v1/devices` | 设备列表（id、时区、标签、启用状态） |
| GET | `/api/v1/devices/{id}` | 单设备详情 |
| GET | `/api/v1/samples` | 样本查询 |
| GET | `/api/v1/aggregates` | 窗口聚合查询 |
| GET | `/api/v1/incidents` | 事件查询 |
| GET | `/api/v1/incidents/{id}` | 事件详情与状态变更时间线 |

## 需要管理令牌（FR-API-004）

管理接口要求 `Authorization: Bearer <token>`。令牌来源（优先级从高到低）：
环境变量 `STREAMFORGE_ADMIN_TOKEN`、`server.admin_token_file` 指向的文件（建议 0600）。
未配置令牌时管理接口一律返回 503。令牌校验为恒定时间比较，令牌不写入日志。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/v1/incidents/{id}/ack` | 确认事件：`{"by":"ops","comment":"..."}`；确认不改变规则状态 |
| POST | `/api/v1/admin/reload` | 热加载配置（成功 `{"ok":true,"config_version":N}`） |
| POST | `/api/v1/admin/import` | 提交导入任务：`{"path":"<input目录内路径>","recursive":false}` → 202 + `job_id` |
| GET | `/api/v1/admin/jobs/{id}` | 查询任务状态：`queued/running/completed/failed` |

## 查询参数

### GET /api/v1/samples

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `device` / `metric` | 是 | 设备与指标 id |
| `from` / `to` | 是 | ISO 8601；必须 `from < to` 且跨度不超过 `server.max_query_range`（默认 24h） |
| `limit` | 否 | 默认 100，最大 1000 |
| `cursor` | 否 | 上一页响应的 `next_cursor`（不透明整数游标） |

响应：`{"samples":[{"id","device_id","metric_id","event_time","value","unit","quality","derived"}],"next_cursor"}`。
`value` 为 `null` 表示空值样本；`derived=true` 表示派生指标样本。

### GET /api/v1/aggregates

`device` / `metric` / `window`（如 `60s`）可选，`from`/`to` 必填。默认只返回最新版本
（FR-AGG-003），字段含 count/min/max/avg/stddev/p50/p95/p99 等。

### GET /api/v1/incidents

`state`、`severity`、`device`、`rule`、`from`、`to`、`limit` 均可选；事件详情含 `timeline`
（open/closed/acknowledged/reopen 的历史记录）。

## 请求体上限

除报表下载外，HTTP 请求体默认上限 1 MiB。
