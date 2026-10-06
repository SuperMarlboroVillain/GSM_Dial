# GSM_Dial 技术手册

基于 STM32F407 + uC/OS-III + emWin 的 GSM 拨号/短信终端

版本：V1.0
适用工程：`E:\STM\GSM_Dial`（Keil MDK-ARM 工程 `Projects/MDK-ARM/atk_f407.uvprojx`）

---

## 1. 项目概述

GSM_Dial 是一套运行在正点原子"探索者"STM32F407 开发板上的 GSM 通信终端软件，通过 USART1 控制 SIM800C/SIM900 系列 GSM 模块（AT 指令），在 480×800 触摸屏上提供拨号、短信、收件箱和 AT 指令测试功能，并支持通过短信远程控制开发板上的 LED 与蜂鸣器。

### 1.1 主要功能

| 功能 | 说明 |
| --- | --- |
| 任意号码拨打 | DIAL 页数字键盘输入号码，CALL 拨出、END 挂断、ANSW 接听 |
| 来电处理 | 来电时全屏弹窗显示号码，可 ANSWER / REJECT；首次振铃蜂鸣器短鸣提示 |
| 任意号码短信发送 | SMS 页内置 QWERTY/123 双模式屏幕键盘，发送结果反馈（SENDING/SEND OK/SEND FAIL） |
| 收件箱 | INBOX 页列表显示最近 8 条收发短信，点击查看详情 |
| 新短信弹窗 | 收到短信时弹出提示（通话中或正在 INBOX 页时自动抑制） |
| 短信远程控制 | 向模块发送控制指令短信，控制 LED0/LED1/BEEP，并自动短信回复执行结果 |
| AT 测试台 | AT 页手动下发任意 AT 指令，滚动显示响应日志（2048 字节环形缓冲） |
| 状态栏 | 底部实时显示模块就绪状态、信号强度（CSQ 换算 dBm）、网络注册状态、通话状态 |

### 1.2 软硬件平台

| 项目 | 配置 |
| --- | --- |
| 主控 | STM32F407ZGT6（Cortex-M4F，168 MHz，1 MB Flash / 192 KB SRAM + 64 KB CCM） |
| 开发板 | 正点原子探索者 F407 |
| GSM 模块 | SIM7600X |
| 显示屏 | 480×800 竖屏 RGB LCD，电容触摸（GT9xxx / FT5206 自适应） |
| 操作系统 | uC/OS-III（1000 Hz 节拍，时间片轮转调度） |
| GUI | SEGGER emWin |
| 编译环境 | Keil MDK-ARM（AC5/AC6 均可） |

---

## 2. 系统总体架构

### 2.1 软件分层

```
┌────────────────────────────────────────────────────┐
│  应用层  gsm_ui.c          （emWin 四页界面 + 弹窗） │
├────────────────────────────────────────────────────┤
│  服务层  gsm.c             （GSM AT 引擎/短信/远程控制）│
├────────────────────────────────────────────────────┤
│  RTOS    uC/OS-III         任务调度、临界区保护      │
│  GUI     emWin             窗口/控件/皮肤           │
├────────────────────────────────────────────────────┤
│  BSP     LED/BEEP/LCD/TOUCH/SRAM/KEY/TIMER/24CXX/IIC │
│  SYSTEM  delay / sys / usart（环形缓冲行接收）        │
│  HAL     STM32F4xx_HAL_Driver                       │
└────────────────────────────────────────────────────┘
```

设计要点：**GSM 引擎与 UI 完全解耦**。`gsm.c` 独占 USART1，运行在独立任务中；UI 任务不直接收发 AT 指令，而是通过 `gsm.h` 中的请求函数（`gsm_req_dial` 等）设置请求标志，由 GSM 任务在轮询中执行；结果通过 `volatile` 全局状态变量回传给 UI 刷新。

### 2.2 任务划分（uc-os3_demo.c）

| 任务 | 优先级 | 堆栈 | 职责 |
| --- | --- | --- | --- |
| start_task | 3 | 128 | 初始化 CPU 库/SysTick、开启时间片轮转、创建其余任务后自删除 |
| user_task | 4 | 1024 | emWin 界面：皮肤设置 → `gsm_ui_create()` → 周期 `gsm_ui_update()`（10 tick ≈ 10 ms） |
| touch_task | 5 | 512 | `GUI_TOUCH_Exec()` 触摸采样，5 ms 周期 |
| led0_task | 6 | 128 | 心跳指示：GSM 未就绪时闪烁 LED0，就绪后 LED0 移交给短信远程控制 |
| gsm_task | 7 | 512 | GSM AT 引擎：初始化、URC 分发、指令收发、短信处理、周期维护，10 ms 轮询 |

> 注意：emWin 控件操作集中在 user_task 一个任务中；触摸任务仅做 `GUI_CURSOR_Show()` 和周期 `GUI_TOUCH_Exec()`/`GUI_Delay()`，满足 emWin 非多任务安全的要求。

### 2.3 启动流程（main.c）

1. `HAL_Init()` → 时钟 168 MHz（`sys_stm32_clock_init(336,8,2,7)`）→ `delay_init` → `usart_init(115200)`
2. 外设初始化：LED、蜂鸣器、LCD、按键、触摸屏、外部 SRAM
3. 三个内存池初始化：`my_mem_init(SRAMIN / SRAMEX / SRAMCCM)`（emWin 使用外部 SRAM）
4. `uc_os3_demo()`：关中断 → `OSInit` → 创建 start_task → `OSStart`
5. start_task 内：`CPU_Init` → SysTick 配置 → 时间片轮转 → 使能 CRC（emWin 需要）→ `GUI_Init` → 创建 4 个任务 → 自删除

---

## 3. 目录结构

```
GSM_Dial/
├── User/                    用户应用代码（本手册重点）
│   ├── main.c               主函数，硬件初始化与内存池初始化
│   ├── gsm.c / gsm.h        GSM AT 引擎（短信、通话、远程控制）
│   ├── gsm_ui.c / gsm_ui.h  emWin 多页界面
│   ├── uc-os3_demo.c/.h     uC/OS-III 任务创建与调度入口
│   ├── stm32f4xx_it.c/.h    中断服务程序（USART1 等）
│   └── stm32f4xx_hal_conf.h HAL 裁剪配置
├── Drivers/
│   ├── BSP/                 板级驱动：LED、BEEP、KEY、LCD、TOUCH(FT5206/GT9xxx)、
│   │                        SRAM、TIMER(btim)、24CXX(EEPROM)、IIC(myiic)
│   ├── SYSTEM/              delay、sys(时钟)、usart(环形缓冲行接收)
│   ├── CMSIS/               内核与器件支持
│   └── STM32F4xx_HAL_Driver HAL 库源码
├── Middlewares/
│   ├── uC-OS3/              uC-CPU / uC-LIB / uC-OS3 内核与移植
│   ├── emWin/               emWin 库、配置与 Demo
│   └── MALLOC/              内存池管理（内部/外部 SRAM/CCM）
├── Projects/MDK-ARM/        Keil 工程（atk_f407.uvprojx）、调试配置、编译日志
├── Output/                  编译产物（atk_f407.axf/.hex/.map 等）
└── ReadMe/                  本手册
```

---

## 4. 硬件资源与连接

| 资源 | 引脚 | 用途 |
| --- | --- | --- |
| USART1_TX | PA9（AF7） | 发往 GSM 模块（AT 指令） |
| USART1_RX | PA10（AF7） | 接收 GSM 模块响应/URC |
| LED0 | PF9（低电平点亮） | 心跳指示 / 远程控制 |
| LED1 | PF10（低电平点亮） | 远程控制 |
| BEEP | PF8（高电平响） | 来电提示 / 远程控制 |
| FSMC | PD/PE/PG 等多脚 | 外扩 SRAM（IS62WV51216，emWin 显存）与 LCD |
| I2C 模拟时序 | PH4/PH5 等 | 电容触摸芯片（GT9xxx/FT5206） |

**重要：USART1 已被 GSM 模块独占**，工程的 `fputc()` 重定向也指向 USART1——向串口 `printf()` 会把调试字符注入 AT 指令流，直接破坏通信（gsm.c 中有明确警示）。调试请使用断点/逻辑分析仪，不要打印。

GSM 模块接线：模块 VCC 接开发板 5V（注意模块峰值电流可达 2A，电源需足够），模块 TX→PA10、RX→PA9、GND 共地。SIM 卡需开通语音/短信业务，开机后模块自动注册网络。

---

## 5. 核心模块设计

### 5.1 串口接收：环形缓冲 + 行切分（Drivers/SYSTEM/usart）

- 中断服务程序只负责把每字节压入 **1024 字节环形缓冲**（写满时丢弃最新字节，不阻塞 ISR），115200 波特率下约 90 ms 缓冲余量。
- 任务侧通过三个接口取数：
  - `usart_get_line(buf, max)`：按 CRLF 切行取出一行（去 CRLF），1=取到一行，0=无完整行；
  - `usart_rx_pop(&b)`：取出单个原始字节（用于捕获无 CRLF 的 `>` 短信提示符）；
  - `usart_rx_flush()`：丢弃全部已缓冲数据（GSM 任务启动时清一次垃圾字节）。
- 相比早期"单缓冲整行接收"方案，连续多行响应（如 `+CMGR:` 头/正文/OK）到达时不再丢行首字节。

### 5.2 GSM AT 引擎（User/gsm.c）

整个引擎是一个 10 ms 周期的轮询状态机，要点如下。

#### 5.2.1 行分发与 URC 处理

- `gsm_drain_lines()` 取出所有完整行；命中 `gsm_is_urc()` 表（`RING`、`+CLIP`、`+CMTI`、`+CSQ`、`+CREG`、`NO CARRIER`、`BUSY`、`RDY`、`SMS READY` 等）的行交给 `gsm_dispatch_urc()` 处理，其余存入 `s_last_line` 供解析。
- 阻塞式指令等待 `gsm_cmd(cmd, want, timeout)` 在等待期间同样分发 URC，因此**指令收发过程中来电/来短信不会丢失**。

#### 5.2.2 通话控制

| 操作 | AT 流程 |
| --- | --- |
| 拨打 | `ATD<num>;` 等 OK → 状态 DIALING；ERROR/超时分别记录 `GSM_DIAL_FAIL_ERROR / TIMEOUT`，UI 显示真实失败原因 |
| 挂断 | `ATH` |
| 接听 | `ATA` → 状态 TALKING |
| 来电 | `RING` URC → INCOMING 并蜂鸣 120 ms；`+CLIP` 提取来电号码；`NO CARRIER/BUSY/NO ANSWER` 回到 IDLE |

#### 5.2.3 短信收发（文本模式，GSM 字符集）

- **发送**：`AT+CMGS="<num>"` → 专用 `gsm_wait_prompt()` 捕获无 CRLF 的 `>` 提示符（等待中照常分发 URC）→ 发正文 → 发 `0x1A` 结束符 → 等 `+CMGS:` 与 OK，超时 8 s。
- **接收（主路径）**：配置 `AT+CNMI=2,1,0,0,0` 使模块推送 `+CMTI: "SM",idx` → 引擎用 `AT+CPMS` 切到指示的存储区 → `AT+CMGR=idx` 读头行（提取发件人）与正文 → `AT+CMGD=idx` 删除存储副本 → 入收件箱并执行远程控制解析。
- **接收（兜底路径）**：若开机注册期间 CNMI 被拒导致推送丢失，每 30 s（仅空闲时）执行 `AT+CMGL="REC UNREAD"` 回扫未读短信，先整体收完响应再逐条处理+删除，每轮最多 4 条。
- **文本清洗**：`gsm_ascii_clean()` 将正文压缩为可打印 ASCII（丢弃控制/高位字节、合并空格），兼容手机发来的 UCS-2/UTF-8 编码短信。**因此短信内容请使用英文**。
- 发送成功的短信会留一份本地副本进收件箱（`gsm_inbox_add`）。

#### 5.2.4 周期维护（gsm_poll）

| 维护项 | 周期 | 说明 |
| --- | --- | --- |
| 模块重连 | 2 s | 未就绪时重发 `AT` + 重新配置（最多 10 次尝试后仍按 2 s 重试） |
| CNMI 重试 | 5 s | 直到模块接受（注册忙时可能拒绝） |
| 未读回扫 | 30 s | 空闲时 `AT+CMGL`，兜底接收 |
| 信号/注册查询 | 60 s | `AT+CSQ`、`AT+CREG?` 静默执行 |

所有自动维护流量都置 `s_log_silent`，**AT 测试台日志里只保留用户手动下发的指令、响应和 URC**，不受后台轮询刷屏干扰。

#### 5.2.5 线程安全模型

- UI → GSM：请求槽（`s_req_dial/s_req_sms/...` + 参数缓冲），写入时用 `CPU_CRITICAL_ENTER/EXIT` 关中断保护，GSM 任务轮询取走。
- GSM → UI：共享 `volatile` 状态（见第 6 节），计数器类变量用 `g_gsm_xxx_dirty` 递增通知变化，UI 比对缓存值后刷新控件，避免每帧重绘。

### 5.3 短信远程控制协议

向 GSM 模块号码发送表内任一指令（**大小写不敏感**、自动忽略首尾空格与控制字符）：

| 指令 | 动作 |
| --- | --- |
| `LED0 ON` / `LED0 OFF` | 点亮/熄灭 LED0 |
| `LED1 ON` / `LED1 OFF` | 点亮/熄灭 LED1 |
| `LED ON` / `LED OFF` | 两个 LED 同时 |
| `BEEP ON` / `BEEP OFF` | 蜂鸣器长响/关闭 |
| `ALL ON` / `ALL OFF` | LED0+LED1+蜂鸣器全部 |
| `STATUS` | 仅查询状态 |

- 执行成功后自动短信回复：`OK LED0:x LED1:x BEEP:x`（x=0/1 实际状态）。
- 无法识别的指令回复：`ERR CMD. USE: LED0/LED1/BEEP/ALL ON|OFF, STATUS`。
- LED 指令执行时蜂鸣器短鸣 100 ms 作为现场 audible 反馈（BEEP 指令本身除外）。

### 5.4 人机界面（User/gsm_ui.c）

- 结构：`FRAMEWIN`（隐藏标题栏）+ `MULTIPAGE` 四页 + 底部双行状态栏。
- **DIAL 页**：号码编辑框（32 号字）、12 键数字键盘、DEL/CALL/END/ANSW、通话状态区。
- **SMS 页**：号码框 + 多行正文框 + QWERTY/123 可切换屏幕键盘（点击输入框切换键盘目标；默认输入正文）、SEND/CLEAR、结果提示。正文上限 140 字符。
- **INBOX 页**：LISTBOX 列表（最新在前，最多 8 条）+ 详情区；切到该页强制重建。
- **AT 页**：指令编辑框 + SEND/AT/CSQ/CREG 快捷键 + 信号/注册行 + 深色终端风格滚动日志（CLRLOG 清空）。
- **弹窗**：来电弹窗（任意页弹出，显示号码，ANSWER/REJECT）；新短信弹窗（显示发件人与摘要，VIEW 跳转 INBOX 页，CLOSE 关闭；通话中或正处 INBOX 页时抑制弹出，但收件箱照更新）。
- 视觉：暗色主题；按钮统一用 Classic 皮肤（FLEX 皮肤会忽略自定义颜色），各功能键按语义着色（绿=呼叫/发送/接听，红=挂断/拒接，蓝=强调，灰=功能）。
- `gsm_ui_update()` 每次只做"缓存比对后刷新"，状态文本、信号、日志、收件箱、弹窗分别独立比对，变化才重绘。

---

## 6. 关键全局状态与 API（gsm.h）

### 6.1 共享状态（GSM 任务写，UI 读）

| 变量 | 含义 |
| --- | --- |
| `g_gsm_ready` | 模块初始化完成 |
| `g_gsm_signal` | CSQ 信号 0..31（99=未知） |
| `g_gsm_call_state` | `GSM_CALL_IDLE/DIALING/INCOMING/ACTIVE` |
| `g_gsm_call_evt` | 通话状态变化计数（UI 可用于触发刷新） |
| `g_gsm_call_fail` | 拨打失败原因：`NONE/ERROR/TIMEOUT` |
| `g_gsm_reg` | 注册状态：`UNKNOWN/OK/OFFLINE/SEARCHING/DENIED` |
| `g_gsm_call_num[21]` | 当前通话号码 |
| `g_gsm_sms_result` | 发送结果：`NONE/SENDING/OK/FAIL` |
| `g_gsm_inbox_cnt` / `g_gsm_inbox_dirty` | 收发短信总数 / 收件箱变化计数 |
| `g_gsm_at_log_dirty` | AT 日志增长计数 |
| `g_gsm_inbox[8]`（`gsm_sms_t`） | RAM 收件箱环形缓冲（号码 20 + 正文 140） |

### 6.2 API 一览

| 函数 | 说明 |
| --- | --- |
| `gsm_task(p_arg)` | uC/OS-III 任务入口（由 uc-os3_demo 创建） |
| `gsm_req_dial(num)` | 请求拨打（UI 侧任意任务可调用） |
| `gsm_req_hangup()` / `gsm_req_answer()` | 请求挂断 / 接听 |
| `gsm_req_sms(num, msg)` | 请求发短信 |
| `gsm_req_at(cmd)` | 测试页原始 AT 指令（自动补 CRLF） |
| `gsm_get_sms_result()` | 读取发送结果 |
| `gsm_format_call(state_txt, num_txt)` | 临界区内拷贝通话状态文本 |
| `gsm_at_get_log(buf, max)` / `gsm_at_clear_log()` | 读取（过长取最新尾部）/ 清空 AT 日志 |

限制常量：`GSM_SMS_MAX_LEN=140`、`GSM_NUM_MAX_LEN=20`、`GSM_INBOX_MAX=8`。

---

## 7. 编译、下载与使用

1. 用 Keil MDK-ARM 打开 `Projects/MDK-ARM/atk_f407.uvprojx`（目标器件 STM32F407ZGTx）。
2. 编译（Output 目录已含 axf/hex 产物）。烧录到探索者 F407 开发板。
3. 接好 GSM 模块与天线，插入 SIM 卡，上电：
   - 状态栏先显示 `GSM: INIT...`，模块注册后显示 `GSM:READY CSQ:x(-xxxdBm) REG:OK`；
   - 无模块/SIM 时显示 `GSM: NO MODULE / CHECK SIM`，引擎每 2 s 自动重试。
4. DIAL 页拨号；SMS 页编辑发送；INBOX 页查看；AT 页调试模块指令。
5. 用手机向 SIM 卡号码发送 `LED0 ON` 等指令即可远程控制，开发板自动回复执行结果。

---

## 8. 开发与调试注意事项

1. **禁止 printf**：USART1 是 GSM 链路，任何打印都会污染 AT 流（`fputc` 直写 USART1）。
2. **短信只支持英文**：模块工作在 `AT+CMGF=1` 文本模式 + `AT+CSCS="GSM"` 字符集；中文会经清洗后失真。
3. 发送短信的 `>` 提示符没有 CRLF，不能走行接收路径——改动接收逻辑时保留 `gsm_wait_prompt()` 的字节级探测。
4. `+CMGR/+CMGL` 头行的第一个引号字段是状态文本（"REC UNREAD"），解析发件人必须用 `gsm_pick_sender()` 找第一个以 `+` 或数字开头的引号字段，勿用第一个引号字段。
5. SIM7600 等模块可能把短信存到 "ME" 而非 "SM"：接收路径已按 `+CMTI` 指示的存储区切换读（读毕恢复默认），移植其他模块时注意保留这一行为。
6. emWin 只能由 user_task 调用（`gsm_ui_*`）；GSM 任务不得直接操作控件。
7. 请求槽均为单缓冲：UI 快速连点同类请求时只有最后一次生效；`s_reply_req` 回复队列同理。
8. 修改任务堆栈/优先级后建议用 uC/OS-III 统计任务验证余量，emWin 任务当前为 1024 字。

---

## 9. 已知限制与可扩展方向

- 限制：收件箱仅 RAM 8 条、无联系人簿、无通话记录、无 PDU 中文短信、无免提音频通路（模块需外接咪头/喇叭才能实现通话语音）。
- 可扩展：接入 24CXX EEPROM 或 SPI Flash 持久化收件箱；利用 BSP 的 KEY 做实体按键拨号；btim 定时器资源预留（当前未初始化）；在 AT 页基础上扩展常见指令按钮；接入模块语音编解码引脚实现免提通话。

---

*本手册依据工程源码整理：User/gsm.c、User/gsm_ui.c、User/uc-os3_demo.c、User/main.c、Drivers/SYSTEM/usart/usart.c 及 MDK 工程配置。*
