# TLM 二进制协议说明（草案 v0.1，供 M2 前审查）

状态：**草案**。本文档定义 StreamForge 自定义 TLM 二进制遥测格式的完整编解码规则。
M2 交付的 `libtlmcodec`（C11）与 data_generator 以本文为准；字段编码、长度边界与
兼容策略请在本阶段审查定稿。

## 1. 总则

- 所有整数与浮点数一律使用**小端**（little-endian）字节序。
- 所有字符串统一 **UTF-8** 编码，不含终止符，长度由 TLV 长度字段给出。
- 文件由一个文件头与若干数据帧组成；帧 = 帧头 + 负载 + 负载 CRC32。
- 帧内数据帧负载使用 **TLV** 编码（见 §4）。

## 2. CRC32 参数（明确约定，不得只写 "CRC32"）

本协议所有 CRC32 均采用 **CRC-32/ISO-HDLC**（即 zlib / IEEE 802.3 所用参数）：

| 参数 | 值 |
| --- | --- |
| 多项式（正常序） | `0x04C11DB7` |
| 多项式（反射序，实现常用） | `0xEDB88320` |
| 初始值 init | `0xFFFFFFFF` |
| refin / refout | true / true |
| 异或输出 xorout | `0xFFFFFFFF` |
| 校验值（check，对 ASCII `"123456789"`） | `0xCBF43926` |

CRC 计算范围：

- 文件头 CRC32：文件头前 20 字节（magic 至 flags）。
- 负载 CRC32：帧负载全部字节（解压**前**的原始负载；若 flags bit0=1，即压缩后的字节）。
- CRC 值本身以小端 4 字节存储在对应位置。

## 3. 文件头与帧结构

### 3.1 文件头（24 字节）

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | ASCII | `TLM1` |
| 4 | 2 | version | uint16 | 当前为 1 |
| 6 | 2 | header_size | uint16 | 固定 24 |
| 8 | 8 | created_at_ms | int64 | UTC Unix 毫秒 |
| 16 | 4 | flags | uint32 | 当前必须为 0 |
| 20 | 4 | header_crc32 | uint32 | 前 20 字节的 CRC32 |

### 3.2 帧头（16 字节）与帧尾

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | sync | uint16 | 固定 `0xA55A`（字节序 `5A A5`） |
| 2 | 1 | frame_type | uint8 | 1=数据帧，2=设备元数据帧，3=心跳帧 |
| 3 | 1 | flags | uint8 | bit0=负载经 zlib 压缩；bit1..7 保留，必须为 0 |
| 4 | 4 | payload_size | uint32 | **解压前**负载长度，≤ 4 MiB |
| 8 | 8 | sequence | uint64 | 文件内严格递增，从 1 开始 |
| 16 | N | payload | bytes | 负载（TLV 或空） |
| 16+N | 4 | payload_crc32 | uint32 | 负载 CRC32 |

- `payload_size` 为 0 允许（心跳帧）。
- 解压后负载长度上限同为 4 MiB。
- zlib 压缩流必须自终止（zlib trailer 完整）；解压输出超限即错误。

## 4. TLV 字典（数据帧负载）

### 4.1 TLV 编码

```
+--------+-----------+------------------+
| type   | length    | value            |
| uint8  | uint16 LE | length 字节      |
+--------+-----------+------------------+
```

- 负载由 TLV 依次拼接；单帧 TLV 数量上限 **256**。
- 单个 TLV value 长度上限 **4096** 字节。
- 未知 `type` 按 length 跳过，不作错误（前向兼容，见 §6）。

### 4.2 类型码

| type | 名称 | value 编码 | 数据帧必填 | 说明 |
| ---: | --- | --- | :---: | --- |
| 0x01 | device_id | UTF-8，1..64 字节，`[A-Za-z0-9._-]` | 必填 | 设备标识 |
| 0x02 | metric | UTF-8，1..64 字节，`[A-Za-z0-9._-]` | 必填 | 指标代码 |
| 0x03 | event_time_ms | int64 LE | 必填 | 事件时间，UTC Unix 毫秒 |
| 0x04 | value | IEEE 754 double LE | 二选一 | 数值；不得为 NaN/Inf |
| 0x05 | value_null | 空（length=0） | 二选一 | 空值标记 |
| 0x06 | unit | UTF-8，1..32 可打印 ASCII | 必填 | 输入单位 |
| 0x07 | quality | uint8（0/1/2） | 可选 | 缺省 0 |
| 0x08 | sequence | uint64 LE | 可选 | 设备侧递增序号 |
| 0x09 | tags | 复合编码（见 §4.3） | 可选 | 标签 |
| 0x0A | display_name | UTF-8，≤128 字节 | 设备元数据帧 | 设备名称 |
| 0x0B | timezone | IANA 名称 UTF-8，≤64 字节 | 设备元数据帧 | 设备时区 |
| 0x0C | sent_at_ms | int64 LE | 心跳帧 | 发送时刻 |

- `0x00` 与 `0x0D..0xFF` 为保留/未知：数据帧中遇到即按 length 跳过。
- `value`(0x04) 与 `value_null`(0x05) 必须恰好出现其一。

### 4.3 tags（0x09）value 内部编码

```
重复 ≤16 次：
+----------+--------+----------+--------+
| klen     | key    | vlen     | value  |
| uint16LE | UTF-8  | uint16LE | UTF-8  |
+----------+--------+----------+--------+
```

- key：1..128 字节，`[A-Za-z0-9._-]`；value：0..128 字节，不含控制字符。
- key 重复：解码错误（拒绝该帧）。
- tags 总 value 长度受 4096 上限约束。

### 4.4 字段重复与缺失

- 必填 TLV（0x01/0x02/0x03/0x04|0x05/0x06）缺失 → 解码错误，整帧拒收。
- **任何** TLV type 在同一帧内重复 → 解码错误（严格模式，简化恢复语义）。
- 未知 TLV 跳过不视为重复。

## 5. 帧类型

1. **数据帧**（type=1）：负载为 §4.2 中 0x01..0x09 的 TLV 集合。
2. **设备元数据帧**（type=2）：负载为 device_id(0x01) + display_name(0x0A) 和/或
   timezone(0x0B)；仅供审计与目录参考，不产生样本。
3. **心跳帧**（type=3）：负载为空或仅 sent_at_ms(0x0C)；用于确认流活性，不产生样本。

## 6. 版本与兼容策略

- `version == 1`：按本文解析。
- 未来**同主版本**（version=1）新增：
  - 新 TLV type：旧解析器跳过（见 §4.1），不报错。
  - 新 frame_type（>3）：旧解析器**跳过整帧**（帧头完整可定位），sequence 仍参与
    严格递增校验。
  - 保留 flags 位置出现非 0：拒绝该文件（无法安全解释）。
- **主版本升级**（version=2+）：文件头 CRC 合法但版本不识别 → 返回
  `UNSUPPORTED_VERSION`，调用方拒绝整个文件，不得猜测解析。
- `schema_version` 语义等价于文件头 version；无独立协商步骤。

## 7. 错误码（libtlmcodec 返回）

| 错误码 | 场景 |
| --- | --- |
| `TLM_OK` | 成功 |
| `TLM_ERR_TRUNCATED` | 文件/帧在任意边界提前结束 |
| `TLM_ERR_BAD_MAGIC` | magic 非 `TLM1` |
| `TLM_ERR_BAD_HEADER_CRC` | 文件头 CRC 不符 |
| `TLM_ERR_BAD_HEADER_SIZE` | header_size != 24 |
| `TLM_ERR_UNSUPPORTED_VERSION` | version 不可识别 |
| `TLM_ERR_BAD_FLAGS` | 文件头 flags 非法，或帧 flags 保留位非 0 |
| `TLM_ERR_BAD_SYNC` | 帧起始 sync 非 `0xA55A` |
| `TLM_ERR_PAYLOAD_TOO_LARGE` | payload_size（压缩前或解压后）超 4 MiB |
| `TLM_ERR_PAYLOAD_CRC` | 负载 CRC 不符 |
| `TLM_ERR_SEQUENCE_REGRESSION` | sequence 未严格递增 |
| `TLM_ERR_BAD_COMPRESSION` | zlib 流损坏或解压输出超限 |
| `TLM_ERR_BAD_TLV` | TLV 截断/越界、必填缺失、重复、字段非法 |

## 8. 损坏恢复

- 解码器遇坏帧（CRC/sync/TLV 非法等）后，**最多向前扫描 1 MiB** 寻找下一个
  `5A A5` 同步字；找到后校验新帧头并继续。
- 恢复后继续解码的数据，其样本必须标记 quality 为**可疑**（`quality=Suspicious`）。
- 扫描 1 MiB 仍找不到同步字 → 返回 `TLM_ERR_BAD_SYNC`，由上层决定终止。

## 9. 人工可核对十六进制示例

以下示例可逐字节核对。文件头 `created_at_ms = 1785598530125`
（= 2026-08-01T15:35:30.125Z），数据帧 sequence=1，未压缩，记录：
`pump-07 / bearing_temp / 76.4 C / quality=0 / sequence=18342`。

文件头（24 字节）：

```
54 4C 4D 31                                magic "TLM1"
01 00                                      version = 1
18 00                                      header_size = 24
4D 52 F7 BD 9F 01 00 00                    created_at_ms = 1785598530125
00 00 00 00                                flags = 0
14 24 44 A0                                header_crc32（前 20 字节）
```

帧头（16 字节）：

```
5A A5                                      sync = 0xA55A（小端）
01                                         frame_type = 1（数据帧）
00                                         flags = 0（未压缩）
42 00 00 00                                payload_size = 66
01 00 00 00 00 00 00 00                    sequence = 1
```

负载（66 字节，TLV 依次拼接）：

```
01 07 00 70 75 6D 70 2D 30 37              device_id = "pump-07"
02 0C 00 62 65 61 72 69 6E 67 5F 74 65 6D 70   metric = "bearing_temp"
03 08 00 4D 52 F7 BD 9F 01 00 00           event_time_ms = 1785598530125
04 08 00 9A 99 99 99 99 19 53 40           value = 76.4（double 0x4053199999999999A 小端）
06 01 00 43                                unit = "C"
07 01 00 00                                quality = 0
08 08 00 A6 47 00 00 00 00 00 00           sequence = 18342
```

负载 CRC32（小端存储）：

```
1B AE FB 60                                payload_crc32 = 0x60FBAE1B
```

## 10. 边界与限制汇总

| 项 | 上限 |
| --- | --- |
| 文件头大小 | 24 字节（固定） |
| 帧负载（压缩前/解压后） | 4 MiB |
| 单帧 TLV 数量 | 256 |
| 单 TLV value | 4096 字节 |
| tags 条数 | 16 |
| tag key/value | 128 字节 |
| device_id / metric | 64 字节 |
| 同步字扫描 | 1 MiB |
