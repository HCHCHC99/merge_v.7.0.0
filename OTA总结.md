# HC32F460 双 APP OTA 升级总结

> 工程：`D:\merge_v.7.0.0`（产品：Bootloader + APP1）
> 模拟 TBOX 升级工装：`D:\ota_ddl3.3_v.4.0\merge_v.4.0.0`（工程 `update`）
> MCU：HC32F460xE —— Flash 512 KB（0x80000），扇区 8 KB（0x2000），共 64 个扇区；RAM 188 KB

本文按三部分总结：**① OTA 使用的 CAN ID 与诊断指令；② OTA 升级扇区分配；③ 升级的大体步骤（正常工况 app→bootloader→新 app；防砖流程 bootloader→app）**。

---

## 0. 方案总览

- 采用 **A/B 双 APP 槽 + 不搬移** 方案：APP1 与 APP2 各自独立、大小相同（各 168 KB / 21 个扇区），Bootloader 通过「跳转槽标志」决定本次启动哪一个。
- OTA 采用 **标准 UDS（ISO 14229）+ ISO-TP（ISO 15765-2）** 走 CAN 扩展帧，物理请求/响应各一个 ID。
- 因为 **APP 不能擦写自己所在的扇区**，所以把一次升级在时间上切成三段（本项目的核心设计）：

| 阶段 | 运行体 | 做什么 | 为什么必须在这一侧做 |
|---|---|---|---|
| **Phase 1** | 旧 APP | 接收升级请求、安全解锁、写共享状态、软复位 | 此时还跑着 APP，Bootloader 尚未接管 |
| **Phase 2** | Bootloader | 进编程会话、擦除目标槽、逐块接收并写入、置跳转槽、复位 | 只有 Bootloader 不受「自己所在扇区被擦」的限制 |
| **Phase 3** | 新 APP | 上电后补发上一条 `0x11` 的肯定响应 `51 01` | `0x11` 是边复位边回，响应只能在重启后补发 |

- 三个阶段的衔接**不靠 RAM**（复位后 RAM 内容不可信），而是靠 Flash 中固定的 **UDS 共享状态区（0x10000，扇区 8）** 里的一个 56 字节结构体：

```c
/* stc_uds_shared_t，位于 UDS_SHARED_SECTOR_BASE = 0x00010000 */
uint32_t magic;        /* 固定 0x55445300 ("UDS\0") */
uint32_t phase;        /* 0=IDLE 正常运行 / 1=ENTER_BOOTLOADER / 2=PROGRAMMING_DONE */
uint32_t target_slot;  /* 本次烧到哪个槽：0x5A5A5A5A=APP1 / 0xA5A5A5A5=APP2 */
uint32_t fw_size;
uint32_t fw_crc;
uint32_t result;       /* 0=进行中 / 1=成功 / 0xFF=失败 */
uint32_t pending_sid;  /* 需要在重启后补发响应的 SID：0x31 / 0x11 / 0=none */
uint32_t reserved[7];
```

> 说明：代码里**没有** `Phase 1/2/3` 的枚举，只有上面的 `phase` 字段；Phase 1/2/3 是流程语义上的划分。

---

## 1. OTA 相关的 CAN ID

### 1.1 诊断与状态帧

| CAN ID | 方向 | 名称 | 说明 |
|---|---|---|---|
| `0x18FF3AFD` | TBOX/工装 → 产品 | **UDS 物理请求** | 唯一的诊断接收 ID（29 位扩展帧，DLC 8） |
| `0x18FF1F11` | 产品 → TBOX/工装 | **UDS 物理响应** | 所有诊断肯定/否定响应；也用于两条「主动补发」帧 |
| `0x18FF1108` | 产品 → TBOX/工装 | **心跳 / 软件版本** | APP 周期发送，8 字节 = 主版本 4 位十进制 + 次版本 4 位十进制 |
| `0x18FF5858` | TBOX → 产品 | **强制升级 / 强制跳转指令** | 裸帧，**不进 ISO-TP**，只在 Bootloader 上电 50 ms 窗口内被响应 |
| `0x18EF5858` | 产品(Bootloader) → TBOX | 强制指令回帧 | `data[0]` 为执行结果状态码 |
| `0x18FF1109` | 工装 → 上位机 | 工装步骤上报 | `byte0`=步骤码，`byte1`=参数（如进度%），`byte2`=细节（如 NRC）。仅工装使用 |
| `0x18FF5555` / `0x18EF5555` | 测试指令 / 回帧 | 停喂看门狗（测试专用） | `0x01`=停喂狗、`0x00`=恢复，用于验证 Bootloader 的 RMU 故障计数逻辑 |
| `0x18DBFFF0` | — | 功能寻址（已废弃） | 迁移记录中已删除，仅在滤波表放行、代码不发不收 |

### 1.2 心跳 `0x18FF1108` 的两种发送者

| 运行体 | 周期 | 内容 | 用途 |
|---|---|---|---|
| APP | 随车身 CAN 周期 | `00 00 00 04 00 00 00 00`（示例，主版本=4、次版本=0） | 供 TBOX 比对版本、判断是否升级 |
| Bootloader（编程模式） | 500 ms | `data[7] = 1`，其余 0（`Bootloader_App.c:745`） | 供 TBOX 判断「当前停在 Bootloader、不是在跑 APP」 |

> 折算规则：`major = d[0]*1000+d[1]*100+d[2]*10+d[3]`，`minor = d[4]*1000+d[5]*100+d[6]*10+d[7]`，`ver = major*10000+minor`。
> ⚠ Bootloader 心跳的 `Bootloader_App.c:14-15` 注释写的是 `data[3]=2`，但实际代码是 `data[7]=1`，**以代码为准**。

### 1.3 历史 ID 变更（`boot\md_record\CAN_ID重新分配.md`）

| 旧 ID | 新 ID | 说明 |
|---|---|---|
| `0x18DA03F1` | `0x18FF3AFD` | TBOX → 推杆（产品接收） |
| `0x18DAF103` | `0x18FF1F11` | 推杆 → TBOX（产品发送） |
| `0x18DBFFF0` | 删除 | 功能寻址广播，废弃 |
| `0x18FF8118` | 删除 | 死 ID |

> 注意：新 ID **不遵循** ISO 14229 推荐的 `0x18DAxxxx` 物理寻址约定，属于客户协议自定义，上位机/TBOX/工装脚本需同步替换。

---

## 2. OTA 相关的诊断指令（UDS 服务）

### 2.1 服务支持表（产品侧，Bootloader 与 APP 共用一套代码）

| SID | 服务 | 典型请求 | 肯定响应 | 备注 |
|---|---|---|---|---|
| `0x10` | 诊断会话控制 | `10 03` / `10 02` | `50 03` / `50 02` | 子功能 0x01/0x02/0x03 全支持；**回包不含 P2/P2\* 参数块** |
| `0x11` | ECU 复位 | `11 01` | 见 2.2 | Bootloader 上下文**不立即回包**，靠 Phase 3 补发 |
| `0x14` | 清除故障码 | `14 FF FF FF` | `54` | 无 DTC，固定空回 |
| `0x19` | 读取故障码 | `19 01` / `19 02` | `59 01 00 00 00` | 固定「无 DTC」 |
| `0x22` | 按 ID 读数据 | `22 F0 00` | `62 F0 00 <val_hi> <val_lo>` | 见 2.3，**值只有 2 字节** |
| `0x27` | 安全访问 | `27 01` / `27 02 <key>` | `67 01 <seed 4B>` / `67 02` | 单级安全，算法见 2.4 |
| `0x2E` | 按 ID 写数据 | `2E ...` | 恒 `7F 2E 31` | 三个 DID 均为只读 |
| `0x31` | 例程控制 | `31 <RID_H> <RID_L> <type>` | 见 2.5 | **触发 Phase 1 的唯一入口** |
| `0x34` | 请求下载 | `34 00 44 <addr 4B> <size 4B>` | `74 20 40 00` | 进入即**立即擦除**目标区间 |
| `0x36` | 传输数据 | `36 <seq> <data…>` | `76 <seq>` | 块序号 1~0xFF，回绕 `0xFF → 0x01` |
| `0x37` | 请求传输退出 | `37` | `77`（裸，无参数） | 仅收尾 CRC，不做校验 |
| `0x3E` | 保持连接 | `3E 00` | 不回（抑制肯定响应） | 工装实际不发；每个请求都会刷新会话计时器 |

其他 SID 统一回 `7F <sid> 11`（服务不支持）。

### 2.2 `0x11` ECU 复位的双上下文行为（关键）

同一份 `uds_diagnostic.c` 被 APP 与 Bootloader 两个工程共用，用 `SCB->VTOR` 自动分流（`boot\...\UDS\uds_diagnostic.c:392-414`）：

| 上下文判据 | 行为 |
|---|---|
| `SCB->VTOR == 0`（Bootloader） | 判 `magic` 合法后把 `pending_sid = 0x11` 写入共享区 → `*resp_len = 0`（**不回响应**）→ 置 `g_delayed_reset_ms = 100` → 100 ms 后 `NVIC_SystemReset()` |
| `SCB->VTOR == APP1/APP2 起始地址`（APP） | 直接回 `51 <reset_type>`，**不复位** |

只接受 `reset_type = 0x01`（hard）与 `0x03`（soft），`0x02`（key off/on）回 `7F 11 12`。

### 2.3 DID 列表

| DID | 含义 | 取值来源 |
|---|---|---|
| `0xF000` | 固件（APP）版本 | `FlashDownload_GetFirmwareVersion()` |
| `0xF001` | Bootloader 版本 | `GetBootloaderVersion()`，当前字符串 `"2.00_AB"` |
| `0xF002` | 固件 CRC | `FlashDownload_GetFirmwareCRC()` |

响应格式：`62 <DID_H> <DID_L> <VAL_H> <VAL_L>`（payload 4 字节，值域 16 位）。非以上三个 DID → `7F 22 31`。

### 2.4 `0x27` 安全访问算法

- 种子：4 字节，伪随机 `seed = (seed_counter * 0x9E3779B9) + session_timer_ms`（配置宏 `UDS_SEED_MODE_FIXED = 0`；若置 1 则固定 `0x12345678`，用于台架调试）。
- 密钥：`seedkey_calc_lv1_key(seed, key)`
  1. 拼 6 字节缓冲：`buf[0..3] = seed[0..3]`，`buf[4..5] = 0xE7 0x38`（常量 `LV1_CODE = 0xe738`）；
  2. CRC8：多项式 **0x1D**、初值 **0xFF**、末字节按位取反（`security_access.c` 的 `crc8()`）；
  3. 链式做 7 轮（每轮把一个 CRC 结果回填到 buf 的下一个位置），得 `crc[0..6]`；
  4. 若 `crc[3..6]` 全 0 → `key = crc[1..4]`，否则 `key = crc[3..6]`。
- 线上格式：`67 01 <seed 大端 4B>` / `27 02 <key 大端 4B>` → `67 02`。
- 失败处理：未解锁最多尝试 3 次，第 3 次起 `security_delay_ms = 100 × 尝试次数`，回 `7F 27 36`；未到 3 次回 `7F 27 35`；延迟期内任何 `0x27` 回 `7F 27 37`。
- **注意**：进入编程会话（`10 02`）会重置安全状态，因此**全流程必须做两次 `27` 解锁**（APP 上下文一次、Bootloader 上下文一次）。

### 2.5 `0x31` 例程控制

请求帧布局为**自定义**（不是 ISO 14229 的 `31 01 <RID_H> <RID_L>`）：

```
byte0 = 0x31
byte1 = RID_H
byte2 = RID_L
byte3 = routineControlType (0x01 启动)
```

| RID | 名称 | 实际是否可用 |
|---|---|---|
| `0xFF02` | 跳转 Bootloader | **唯一在用的 RID**，是 Phase 1 的触发入口 |
| `0xFF00` | 擦除固件 | 有代码但 `uds_handle_routine_control()` 未调用 `uds_start_routine()`，实际不可达 |
| `0xFE00` | 计算 CRC | 同上，不可达（注释称「TBOX 约定」） |
| `0xFF03` | 跳转 APP | 同上，不可达 |

`0x31` 的响应同样是双上下文（`boot\...\UDS\uds_diagnostic.c:718-771`）：

| 上下文 | 行为 |
|---|---|
| APP（`VTOR == APP1/APP2 起始`） | 写共享区 `phase=1`、`pending_sid=0x31` → `*resp_len = 0`（**不回响应**）→ 100 ms 后复位 |
| Bootloader（`VTOR == 0`） | 回 `71 <routineControlType>`（payload 1 字节，**不回显 RID**） |

前置条件：必须先 `27` 安全解锁，否则 `7F 31 33`。
> **注意：APP 侧不校验 RID 的具体值**——只要已解锁、且当前 `VTOR` 指向 APP，任何 `0x31` 都会触发 Phase 1。工装实发的是 `31 FF 02 01`。

### 2.6 否定响应码（NRC）

| NRC | 含义 | 典型触发场景 |
|---|---|---|
| `0x10` | 一般拒绝 | 下载接口返回未知错误 |
| `0x11` | 服务不支持 | 收到未实现的 SID |
| `0x12` | 子功能不支持 | `11 02` |
| `0x13` | 报文长度错误 | `31` 少于 4 字节、`22` 少于 3 字节等 |
| `0x21` | 忙，请重试 | 下载状态机未回 IDLE |
| `0x22` | 条件不满足 | 会话不对 / 下载接口未注册 |
| `0x24` | 请求序列错误 | `0x36` 块序号跳号 |
| `0x31` | 请求超出范围 | 地址不在 APP 窗口、DID 不存在 |
| `0x33` | 安全访问被拒 | 未解锁就发 `0x31` / `0x34` / `0x36` / `0x37` |
| `0x35` | 密钥无效 | 密钥算错（未到 3 次） |
| `0x36` | 超过尝试次数 | 密钥错满 3 次并进入延迟 |
| `0x37` | 延时未到 | 安全延迟期内再请求 |
| `0x72` | 编程失败 | 擦除/写入/校验失败 |
| `0x7F` | 当前会话不支持该服务 | `0x34/0x36/0x37` 不在编程会话且不在扩展会话 |

### 2.7 ISO-TP 参数（双方一致）

| 参数 | 值 | 说明 |
|---|---|---|
| 帧格式 | 29 位扩展帧，DLC 固定 8 | 不足 8 字节用 `0xAA` 填充 |
| 流控 BS（BlockSize） | `0` | 一次性发完，中途不再要 FC |
| 流控 STmin | `5 ms` | 接收方要求的最小帧间隔 |
| 单条消息上限 | `4095` 字节 | 首帧 12 位长度域上限 |
| 接收缓冲 | `8192` 字节（`ISOTP_BUFFER_SIZE`） | 实际上层用的是 `4100` 字节数组 |
| N_Bs 等待 FC | 工装侧 10000 ms | 产品侧 `ISOTP_RX_TIMEOUT_MS` 65535 ms |
| P2（等响应） | 工装侧 5000 ms | 从多帧发完起算 |

### 2.8 非 UDS 的强制指令（`0x18FF5858`，用于防砖兜底）

裸单帧，`data[0]` 为命令码，只在 Bootloader 上电后的 **50 ms 窗口**内被接收（绕过 ISO-TP）：

| 命令码 | 含义 |
|---|---|
| `0xFF` | **强制进入 Bootloader 编程模式**（永不返回，等待刷写） |
| `0x01` | 强制下次启动 APP1 |
| `0x02` | 强制下次启动 APP2 |

回帧 `0x18EF5858`，`data[0]` 状态码：

| 状态码 | 含义 |
|---|---|
| `0x01` / `0x02` | 已切到 APP1 / APP2（并且会复位）；若当前已在目标槽则只回帧、不复位（幂等保护） |
| `0x03` | 两个 APP 槽都被判为坏块 → 进编程模式等待刷写 |
| `0x04` | 目标槽被标记为坏块（故障计数 ≥ 3），**拒绝跳转、不修改跳转槽** |

---

## 3. OTA 升级扇区分配

### 3.1 总表（512 KB / 64 扇区 / 每扇区 8 KB）

| 扇区 | 地址范围 | 大小 | 用途 | 宏定义 |
|---|---|---|---|---|
| 0 – 7 | `0x00000000 – 0x0000FFFF` | 64 KB | **Bootloader 代码区**（实际镜像约 46.5 KB） | `BOOT_START_ADDR` |
| 8 | `0x00010000 – 0x00011FFF` | 8 KB | **UDS 共享状态区**（Phase 1/2/3 握手段） | `UDS_SHARED_SECTOR_BASE` |
| 9 | `0x00012000 – 0x00013FFF` | 8 KB | FlashAdv 管理记录 | `FLASHADV_MGMT_SECTOR_BASE` |
| 10 | `0x00014000 – 0x00015FFF` | 8 KB | 预留（Bootloader 链接上限之后的第一个扇区） | — |
| 11 | `0x00016000 – 0x00017FFF` | 8 KB | **APP1 状态区**：RMU 复位/故障记录 + 喂狗控制字 | `APP1_STATE_SECTOR_BASE` |
| 12 | `0x00018000 – 0x00019FFF` | 8 KB | **APP2 状态区**：同上 | `APP2_STATE_SECTOR_BASE` |
| **13 – 33** | `0x0001A000 – 0x00043FFF` | **168 KB（21 扇区）** | **APP1 运行区（槽 A）** | `APP1_START_ADDR = 0x1A000` |
| **34 – 54** | `0x00044000 – 0x0006DFFF` | **168 KB（21 扇区）** | **APP2 运行区（槽 B）** | `APP2_START_ADDR = 0x44000` |
| 55 | `0x0006E000 – 0x0006FFFF` | 8 KB | 参数存储（magic `0x5AA55AA5`） | `STORAGE_SECTOR_ADDR` |
| 56 – 61 | `0x00070000 – 0x0007BFFF` | 48 KB | 参数管理器 | `PARAM_MANAGER_START_ADDR = 0x70000` |
| 62 | `0x0007C000 – 0x0007DFFF` | 8 KB | **跳转槽标志**（`0x5A5A5A5A`=APP1 / `0xA5A5A5A5`=APP2） | `APP_RUN_SLOT_ADDR` |
| 63 | `0x0007E000 – 0x0007FFFF` | 8 KB | 保护 / 预留 | — |

- 关键尺寸：`FLASH_SECTOR_SIZE = 0x2000`、`FLASH_TOTAL_SIZE = 0x80000`、`APP_MAX_SIZE = 0x2A000`（168 KB）。
- 写保护（`Adp\flash_advanced.h:14-16,25-26`）：扇区 **0–12 与 63** 保护；FlashAdv 允许擦写的有效区为 **13–61**。跳转槽（扇区 62）用 `EFM_SectorErase / EFM_ProgramWord` 直接写，不经过 FlashAdv。
- 两个 APP 槽容量完全相等，因此**升级不分「大/小镜像」，只分「烧到哪个槽」**。

### 3.2 TBOX 侧「伪地址」到实际 Flash 地址的映射

TBOX 协议里 `0x34` 带的地址不是真实 Flash 地址，而是**槽位标签**，由 Bootloader 做等值映射（`Bootloader_App\memory_map.h:54-60`）：

| TBOX 下发地址 | 实际写入 | 说明 |
|---|---|---|
| `0x08018000` | `0x0001A000`（APP1，扇区 13） | 客户协议固定值 |
| `0x08004000` | `0x00044000`（APP2，扇区 34） | 客户协议固定值 |

```c
#define MAP_TBOX_ADDR_TO_FLASH(addr) \
    (((addr) == TBOX_ADDR_APP1) ? APP1_START_ADDR : \
     ((addr) == TBOX_ADDR_APP2) ? APP2_START_ADDR : (addr))
```

> ⚠ 注意三点：
> 1. 这是**等值判断**（不是区间判断），必须是上面两个值之一；其他地址会原值透传，随后被地址合法性检查拒绝（NRC `0x31`）。
> 2. `app1\app\can\uds\memory_map.h` 里的 TBOX 标签是 `0x88010000 / 0x48000000`，与 Boot 侧不一致——**Phase 2 由 Bootloader 执行，以 Boot 侧为准**；APP 侧那份不参与实战路径。
> 3. 当前工装 `tool_config.h` 里 `TOOL_DL_ADDR = 0x08018000` 是**写死**的，**只刷 APP1**。要刷 APP2 必须改成 `0x08004000` 重新编译工装。

### 3.3 升级时实际被擦写的范围

一次升级只动目标槽的 21 个扇区 = 168 KB：

- 擦除时机：`0x34 RequestDownload` 一进来就整段擦除（按 8 KB 扇区对齐，逐扇区调用 `FlashAdv_EraseSector`）；
- `FlashDownload_Erase()`（`0x31 RID 0xFF00` 路径）实际是空操作，真正的擦除已在 `0x34` 内完成；
- 写入时机：`0x36` 收到一帧就立刻写 Flash（无缓存整流），因此**掉电只会损坏当前块**，不会破坏整槽以外区域。

---

## 4. 升级步骤

### 4.1 正常工况（当前跑着可用 APP）：APP(Phase 1) → Bootloader(Phase 2) → 新 APP(Phase 3)

```
TBOX/工装                 旧 APP                        Bootloader                新 APP
   |                        |                               |                        |
   |-- 10 03 -------------->|                               |                        |
   |<------------- 50 03 ---|                               |                        |
   |-- 27 01 -------------->|                               |                        |
   |<------ 67 01 <seed> ---|                               |                        |
   |-- 27 02 <key> -------->|                               |                        |
   |<------------- 67 02 ---|                               |                        |
   |-- 31 FF 02 01 -------->|  ← Phase 1 触发                |                        |
   |   (无响应)              |  写共享区: phase=1             |                        |
   |                        |            pending_sid=0x31    |                        |
   |                        |  等 100 ms → 软复位 ---------->|                        |
   |                        |                               |  Rmu_ProcessPowerUp()  |
   |                        |                               |  读共享区 phase==1     |
   |                        |                               |  → 进编程模式          |
   |<---- 71 01 FF 00 --------------------------------------|  ← 补发 Phase 1 的响应 |
   |-- 10 02 ---------------------------------------------->|                        |
   |<------------------------------------------ 50 02 ------|                        |
   |-- 27 01 ---------------------------------------------->|                        |
   |<-------------------------------- 67 01 <seed> ---------|                        |
   |-- 27 02 <key> ---------------------------------------->|                        |
   |<------------------------------------------ 67 02 ------|                        |
   |-- 34 00 44 08 01 80 00 00 02 A0 00 ------------------>|  映射→0x1A000           |
   |                                                        |  擦除 21 扇区          |
   |<-------------------------------------- 74 20 40 00 ----|                        |
   |-- 36 01 <256B> -------------------------------------->|  写入 + 累加 CRC32     |
   |<------------------------------------------ 76 01 ------|                        |
   |      … ×672 块（序号 1…0xFF,1…0xFF,1…162）…            |                        |
   |-- 37 ------------------------------------------------->|                        |
   |<--------------------------------------------- 77 ------|                        |
   |                                                        |  写共享区 phase=2      |
   |                                                        |  result=1, target_slot |
   |                                                        |  清该槽 RMU 记录       |
   |                                                        |  置跳转槽=目标槽        |
   |-- 11 01 ---------------------------------------------->|  写 pending_sid=0x11   |
   |   (无响应)                                              |  等 100 ms → 复位 ---->|
   |                                                        |                        |  ← Phase 3
   |<---- 04 51 01 00 00 00 00 00 -----------------------------------------------|  补发 51 01
   |                                                        |                        |  清共享区
```

#### Phase 1（旧 APP 侧，约 100 ms）

1. `10 03` → `50 03`：进扩展会话。
2. `27 01` / `27 02 <key>` → `67 01 <seed>` / `67 02`：安全解锁。
3. `31 FF 02 01` → **产品不回响应**。APP 内部动作：
   - 写共享区（`0x10000`）：`magic = 0x55445300`、`phase = 1`、`pending_sid = 0x31`；
   - `g_delayed_reset_ms = 100`；
   - 100 ms 后 `NVIC_SystemReset()`。

   *为什么先不回包就复位*：APP 已经要重启，断言响应没有意义；若强行先发响应再复位，TBOX 会在「已确实进 boot」之前就收到肯定响应，容易误判。因此把这条响应**推迟到 Bootloader 里补发**，TBOX 收到 `71 01 FF 00` 就等于「Bootloader 已就绪」。

#### Phase 2（Bootloader 侧，约 2 分钟，主体耗时）

Bootloader 上电后 `Boot_StartupSequence()` 的执行顺序：

1. `Rmu_ProcessPowerUp(当前槽)` —— **第一件事**，读复位原因、分类故障/正常、清标志、必要时更新故障计数；
2. **50 ms 强制指令窗口**：监听 `0x18FF5858`（本次正常升级不会有帧，超时即过）；
3. **读共享区**：`magic` 合法且 `phase == 1` → 调 `Bootloader_UdsMain()`，**永不返回**。

`Bootloader_UdsMain()` 的流程：

| 步 | 动作 |
|---|---|
| 1 | 若 `pending_sid == 0x31`，**主动补发** `71 01 FF 00`（4 字节，经 ISO-TP 单帧发到 `0x18FF1F11`），然后清 `pending_sid` |
| 2 | 初始化下载模块（上限 168 KB、扇区 8 KB、允许区 = 目标槽窗口、verify=1、不自动复位），注册下载接口，`uds_init()` |
| 3 | 主循环：每 500 ms 喂狗 + 发心跳 `0x18FF1108`（`data[7]=1`）；`UdsOta_Poll()` 处理诊断 |
| 4 | `10 02` → `50 02`：进编程会话（会重置安全状态） |
| 5 | `27 01` / `27 02`：**第二次**解锁 |
| 6 | `34` → 地址映射 + 合法性校验 + 立即擦除 21 扇区 → `74 20 40 00`（LFI=`0x20`，最大块长 `0x4000`=16 KB） |
| 7 | `36 <seq> <data>` × 672 块 → 逐块写入并累加 CRC32 → `76 <seq>` |
| 8 | `37` → CRC 取反定型（仅打印日志）→ `77` |
| 9 | 检测到 `FW_UPDATE_COMPLETE`：写共享区 `magic + phase=2 + result=1 + target_slot`；`Rmu_ResetSlotRecord()` 清目标槽故障记录；正式模式下 `Boot_SetRunSlotToAddr(实际烧录地址)`（**烧到哪跳哪**） |
| 10 | `11 01` → 写 `pending_sid = 0x11` → `g_delayed_reset_ms = 100` → 复位（不回响应） |

#### Phase 3（新 APP 侧，上电后立即）

1. 新 APP 在 `main()` 里、主循环之前调用 `UdsOta_App_CheckPendingAck()`（`app1\startup\main.c:140`）；
2. 读共享区，若 `magic` 合法且 `pending_sid == 0x11`：**手工拼一个单帧** `04 51 01 00 00 00 00 00` 发到 `0x18FF1F11`（PCI=`0x04` 表示 4 字节单帧，后 4 字节填充 0）；
3. `UdsShared_Clear()` 清零共享区，握手结束。

> 工装收到 `51 01` 即判升级成功 → 上报 `DONE` → 静默 3 s（等新 APP 心跳稳定）→ 回到监听状态。

### 4.2 防砖流程（APP 不可用）：Bootloader(Phase 1+2) → APP(Phase 3)

当 APP 槽被判坏、或 TBOX 主动要求时，Bootloader 直接接管，把原本属于 APP 的 Phase 1 一起做掉，于是只剩两段：

```
TBOX/工装                 Bootloader(编程模式)                 新 APP
   |                        |                                    |
   |  ← 上电即进编程模式     |  心跳 0x18FF1108 (data[7]=1)       |
   |<--- 18FF1108 ----------|                                    |
   |-- 10 03 -------------->|  ← 这一步由 Bootloader 代做 Phase 1 |
   |<------------- 50 03 ---|                                    |
   |-- 27 01 -------------->|                                    |
   |<------ 67 01 <seed> ---|                                    |
   |-- 27 02 <key> -------->|                                    |
   |<------------- 67 02 ---|                                    |
   |-- 31 FF 02 01 -------->|                                    |
   |<---------- 71 01 ------|  ← 立即回（不需要"补发"，无需重启）  |
   |-- 10 02 -------------->|                                    |
   |      … 后续 27 / 34 / 36×672 / 37 与 4.1 的 Phase 2 完全相同 …|
   |-- 11 01 -------------->|  写 pending_sid=0x11 → 复位 ------->|
   |                        |                                    |  ← Phase 3
   |<---- 04 51 01 00 00 00 00 00 ---------------------------------|  补发 51 01
```

**与正常工况的唯一差别**：`0x31` 由 Bootloader 直接回答 `71 01`（不必先复位），Phase 1 与 Phase 2 在时间上合并；Phase 3 仍由新 APP 完成。工装侧无需切换模式——它在 `st_jump_boot` 收到 `71 01`（`TXN_OK`）就直接进 `10 02`，收到超时则走「等补发」分支，两条路都能走通。

**进入防砖的三条路径**（都在 Bootloader 启动时判定）：

| 入口 | 条件 | 现象 |
|---|---|---|
| TBOX 强制指令 | `0x18FF5858` 发 `data[0] = 0xFF` | 上电 50 ms 窗口内收到 → 直接 `UdsOta_Bootloader_Enter()`，永不返回 |
| 双槽均坏 | 两个槽的 `faultCount ≥ 3` 或固件校验不通过 → `SelectTargetSlot()` 返回 `SLOT_NONE` | `RunBootloaderForever()` = `UdsOta_Bootloader_Enter(); while(1);`，永久停在编程模式等刷写 |
| 指定跳转被拒 | `0x18FF5858` 发 `0x01`/`0x02`，但目标槽 `faultCount ≥ 3` | 回 `0x04`（拒绝），**不修改跳转槽**，继续按原槽启动 |

**「坏块」是怎么判出来的**（防砖的核心判据，两级）：

1. **复位原因计数（RMU）**：`Utils\rmu.c` 在上电时读 RMU 复位标志，只有 `SWDT | WDT | MPU_ERR` 三类算故障；故障类且当前计数 `< 3` 才 `+1` 并写入该槽的状态扇区（扇区 11/12）。掉电、上电复位、软件复位等正常原因**不累加、不写 Flash**（保护擦写寿命）。记录 magic 为 `0x524D5532`（"RMU2"）。
2. **固件合法性**：`IsAppFirmwareValid(addr)` 只检查两个 32 位字——
   - `*(u32*)addr`（初始 SP）必须落在 `[0x1FFF8000, RAM_END]`；
   - `*(u32*)(addr+4)`（复位向量）不能是 `0xFFFFFFFF`，且必须落在 `[APP1_START_ADDR, APP2_END_ADDR]`。

   → 槽可用判据：`faultCount < 3 && IsAppFirmwareValid(addr)`。
3. **跳转槽标志**：扇区 62（`0x7C000`）落盘 `0x5A5A5A5A`（APP1）/ `0xA5A5A5A5`（APP2）；擦除态视为 APP1 并回写。选槽规则：当前槽可用则不动；否则另一槽可用则切过去；都不可用则进编程模式。

> 升级成功后 `Rmu_ResetSlotRecord()` 会把目标槽的故障计数与原因统计整体清零，因此**刷完即恢复"健康"**，不会因为历史故障被再次判坏。

### 4.3 两种流程的对比

| 项目 | 正常工况 | 防砖流程 |
|---|---|---|
| Phase 1 执行者 | 旧 APP | Bootloader |
| Phase 2 执行者 | Bootloader | Bootloader |
| Phase 3 执行者 | 新 APP 补发 `51 01` | 新 APP 补发 `51 01` |
| `0x31` 响应方式 | APP 不回 → Bootloader 重启后补发 `71 01 FF 00` | Bootloader 立即回 `71 01` |
| 复位次数 | 3 次（APP→BL→APP） | 2 次（BL→APP） |
| 前置条件 | APP 可运行、可解锁 | 无（`0xFF` 强制 / 双槽判坏 / 固件非法） |

---

## 5. 时序参数一览

| 参数 | 值 | 位置 | 含义 |
|---|---|---|---|
| `DELAYED_RESET_MS` | 100 ms | `main.h` | 先置复位倒计时，回完响应/写完共享区再复位 |
| `BOOT_FORCE_CMD_WINDOW_MS` | 50 ms | `Bootloader_App.h:60` | 上电后监听强制指令的窗口 |
| `UDS_HEARTBEAT_PERIOD_MS` | 500 ms | `Bootloader_App.c:17` | Bootloader 编程模式心跳周期 |
| `UDS_DEFAULT_SESSION_TIMEOUT_MS` | 4000 ms | `uds_diagnostic.h` | 会话超时（S3），每个请求都会刷新 |
| `FW_TRANSFER_TIMEOUT_MS` | 2000 ms | `flash_download.c` | `0x34` 后 2 s 无新 `0x36` → 下载状态回 IDLE（支持重刷） |
| `UDS_SECURITY_DELAY_BASE_MS` | 100 ms | `uds_diagnostic.h` | 安全延迟基数，实际延迟 = 100 × 尝试次数 |
| `MAX_APP_FAULT_COUNT` | 3 | `Bootloader_App.h` | 故障计数达到 3 即判坏块 |
| `ISOTP_DEFAULT_ST_MIN_MS` | 5 ms | `isotp_transport.h` | 多帧最小间隔 |
| `ISOTP_DEFAULT_BLOCK_SIZE` | 0 | `isotp_transport.h` | 一次发完，不要中途 FC |

**升级耗时估算**：672 块 × 37 帧（1 FF + 36 CF）× 5 ms ≈ **120~125 s**（不含擦除与交互开销），是整条流程的主要耗时。

---

## 6. 模拟 TBOX 的升级工装

### 6.1 定位与启动

- 工程：`D:\ota_ddl3.3_v.4.0\merge_v.4.0.0\update`，主流程在 `...\ev_hc32f460_lqfp100_v2\UDS\upgrade_tool.c`。
- 角色：**UDS Tester（客户端）**，模拟客户 TBOX。全自动，无串口菜单、无 CLI；配置全是 `tool_config.h` 里的编译期宏；日志走 SEGGER RTT。
- 入口：`template\source\main.c` → `Hardware_Init()` → `Tool_Init()` → `while(1) { Tool_Poll(); }`。

### 6.2 触发条件

1. 监听 `0x18FF1108` 心跳，连续 **10 帧**版本一致才判可信（`TOOL_HB_CONFIRM_CNT`）；
2. 若 `TOOL_FW_VERSION_NUM > 当前版本` → 开始升级。当前宏值 `0xFFFFFFFF`（显示 "FFFF.FFFF"），**任何真实版本都小于它，因此每次上电必刷一次**；恢复真实版本比较需改回 `TOOL_FW_VER_MAJOR*10000 + TOOL_FW_VER_MINOR`。
3. 客户模式 `TOOL_FLOW_RETRY_ENABLE = 0`：**每次上电只允许升级一次**，失败即停机，需重新上电；成功后永久静默直到下次上电。

### 6.3 状态机（完整序列）

`LISTEN → EXT_SESSION(10 03) → UNLOCK_APP(27) → JUMP_BOOT(31 FF 02 01) → WAIT_BOOT(等 71 01 FF 00) → PROG_SESSION(10 02) → UNLOCK_BOOT(27) → DL_REQ(34) → TRANSFER(36×672) → EXIT(37) → ECU_RESET(11 01) → WAIT_5101(等 51 01) → DONE_DELAY(3 s) → LISTEN`

另有 `RETRY_DELAY`（整流程重试延迟）与 `ERROR`（停机）两个终态。每个步骤都通过 `0x18FF1109` 上报步骤码与进度。

### 6.4 关键参数

| 参数 | 值 | 说明 |
|---|---|---|
| 固件源 | `TOOL_FW_STORE_ADDR = 0x0001A000`，长度 `0x2A000` | 直接从工装自己的 Flash 取原始 bin，**无镜像头** |
| 下载目标 | `TOOL_DL_ADDR = 0x08018000` | 映射到 APP1；工装**只会刷 APP1** |
| 每块数据 | 256 字节（`TOOL_DL_BLOCK_DATA_SIZE`） | 必须 4 字节对齐；用 258 会导致块起始地址非字对齐、EFM 编程挂死 |
| 块总数 | 672 块（172032 / 256） | 正好整除，无尾块 |
| 块序号 | 1 起，`0xFF → 0x01` 回绕 | 与产品侧 `flash_download.c` 完全对齐（非 ISO 标准的 `0xFF→0x00`） |
| 超时 | P2 5000 / 等 FC 10000 / 等 `71` 15000 / 等 `51` 15000 / 静默 3000 ms | |
| 重试 | 块级 3 次、NRC 0x22 3 次（间隔 5 s）、整流程 2 次（客户模式关闭） | |

### 6.5 工装对异常的容忍

| 情况 | 处理 |
|---|---|
| `0x31` 在 APP 侧无响应 | 视为正常，进入 `WAIT_BOOT` 等 `71 01 FF 00`（15 s 超时） |
| `0x11` 在 Bootloader 侧无响应 | 视为正常，进入 `WAIT_5101` 等 `51 01`（15 s 超时；超时后若心跳版本已变也判成功） |
| NRC `0x33` | 触发 `security_resume()`：重新解锁后回到出错的那一步继续 |
| NRC `0x78` | 无限重开 P2 计时，不计失败 |
| `0x36` 超时 / NRC | 同块重发，最多 3 次 |
| 其它步骤 NRC | `flow_fail()` 停机，停在步骤码 `0x0F` |

---

## 7. 已知限制与注意事项

### 7.1 协议偏离（非 ISO 14229 标准，TBOX 侧需按本项目约定实现）

| 项 | 本项目实现 | 标准 |
|---|---|---|
| `0x31` 请求布局 | `31 <RID_H> <RID_L> <type>` | `31 <type> <RID_H> <RID_L>` |
| `0x31` 肯定响应 | `71 <type>`（2 字节，不回显 RID） | `71 <type> <RID_H> <RID_L>` |
| `0x36` 块序号回绕 | `0xFF → 0x01` | `0xFF → 0x00` |
| `0x37` 肯定响应 | 裸 `77`（无参数） | 可带参数 |
| `0x10` 肯定响应 | 无 P2/P2\* 参数块 | 通常带 P2/P2\* |
| `0x11` 子功能 | 只支持 `0x01`/`0x03` | `0x01~0x03` + 厂商自定义 |
| 会话错误 NRC | `0x7F`（当前会话不支持该服务） | 同（符合标准） |
| 主动补发响应 | `71 01 FF 00`、`51 01` 两条与请求无关联的主动帧 | 无此机制 |

### 7.2 稳健性方面的待改进点

1. **固件无完整性校验**：镜像没有 magic、长度、版本、CRC、签名任何字段；跳转前只校验栈顶与复位向量两个 32 位字。虽然 `0x36` 全程累加 CRC32，但 `0x37` 只是取反后打印日志，**不与任何期望值比较，也不回 NRC**。
2. **CRC 相关接口有缺陷**：`FlashDownload_CalculateCRC()` 每轮把累加器重置，结果只等于最后一个 word 的 CRC；`stc_uds_shared_t.fw_crc` 字段定义了但全程无人写入。
3. **`0x78`（responsePending）从不下发**：擦除或长写操作期间若超过 TBOX 的 P2（本项目工装给了 5 s），对端会判超时。
4. **`0x34` 声明的最大块长是 `0x4000`（16 KB），但上层接收缓冲只有 4100 字节**，`isotp_receive_frame` 只拒绝 > 8192 的多帧；若 TBOX 按 16 KB 一帧发就会越界。当前工装用 256 B/块，不会触发。
5. **TBOX 地址是等值标签**：`0x08018000 / 0x08004000` 之外的值会被原值透传并拒绝；换协议时要同时改 `memory_map.h` 与 TBOX 侧。
6. **工装只能刷 APP1**，刷 APP2 需改宏重编译，没有运行时选择。
7. **`Boot_SwitchAndRunOther()` 无调用点**（死代码），`RID 0xFF00 / 0xFE00 / 0xFF03` 在实战路径里不可达。
8. **APP 与 Bootloader 的 `memory_map.h` 中 TBOX 标签不一致**（APP 侧 `0x88010000 / 0x48000000`），虽然不影响实战（Phase 2 由 Bootloader 执行），但容易在维护时误读。

### 7.3 移植/换客户时需要同步修改的地方

| 需求 | 需要改的宏 / 文件 |
|---|---|
| 改诊断 CAN ID | `UDS/uds_diagnostic.h` 的 `UDS_PHYSICAL_REQUEST_ID` / `UDS_PHYSICAL_RESPONSE_ID`；`UDS/isotp_transport.h` 的 `ISOTP_FILTER_CAN_ID_LIST`；工装的 `tool_config.h` |
| 改强制指令 ID | `Bootloader_App/Bootloader_App.h` 的 `BOOT_FORCE_CMD_CAN_ID` / `BOOT_FORCE_CMD_RESP_CAN_ID` |
| 改 APP 分区 | 两边 `memory_map.h` + Keil IROM1 链接地址 + 工装 `TOOL_DL_ADDR` |
| 改 TBOX 地址窗口 | `Bootloader_App/memory_map.h` 的 `TBOX_ADDR_APP1` / `TBOX_ADDR_APP2`（`memory_map.txt` 里还留了一套「区间映射」方案，需要时直接替换） |
| 改版本比较策略 | 工装 `tool_config.h` 的 `TOOL_FW_VERSION_NUM` |
| 现场允许重刷 | 工装 `tool_config.h` 的 `TOOL_FLOW_RETRY_ENABLE` 置 1 |

---

## 8. 关键源码位置索引

| 功能 | 文件 |
|---|---|
| Flash 分区与 TBOX 地址映射 | `boot\projects\ev_hc32f460_lqfp100_v2\Bootloader_App\memory_map.h`（APP 侧对应 `app1\app\can\uds\memory_map.h`） |
| Bootloader 启动决策 / 强制指令 / 选槽 / 补发 ACK | `boot\...\Bootloader_App\Bootloader_App.c`（`Boot_StartupSequence`、`Bootloader_UdsMain`、`App_CheckPendingUdsAck`） |
| RMU 复位原因与故障计数 | `boot\...\Utils\rmu.c` / `rmu.h` |
| UDS 服务实现（APP 与 Boot 共用一份代码，靠 `SCB->VTOR` 分流） | `boot\...\UDS\uds_diagnostic.c`、`app1\app\can\uds\uds_diagnostic.c` |
| 安全访问算法 | `security_access.c` |
| ISO-TP 传输层 | `isotp_transport.c` / `.h` |
| 下载状态机与 CRC32 | `boot\...\UDS\flash_download.c` |
| 诊断入口（CAN 帧分发） | APP：`app1\app\can\uds\uds_rx_entry.c`；Boot：`Bootloader_App.c` 里的 `BL_ISOTP_RxCallback` |
| 心跳与版本 | APP：`app1\app\can\app_can.c` 的 `build_can_1108` |
| 升级工装主流程 | `D:\ota_ddl3.3_v.4.0\merge_v.4.0.0\update\projects\ev_hc32f460_lqfp100_v2\UDS\upgrade_tool.c` |
| 工装 UDS 请求封装 | 同目录 `uds_tester.c` / `.h` |
| 工装配置 | 同目录 `tool_config.h` |
