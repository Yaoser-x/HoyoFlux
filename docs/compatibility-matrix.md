# 兼容性矩阵：HoyoFlux 1.0.0

旧工具仍是行为基准。本表逐项记录 HoyoFlux 已对齐的行为，以及有意做出的改进。“已验证”要求通过真实游戏门（B1）；自动化测试只能证明机制，不能代替游戏内结果。

## 行为对齐

| 功能 | 上游旧工具 | HoyoFlux | 结果 | 验证状态 |
| --- | --- | --- | --- | --- |
| 原神 FPS 解锁 | 签名重定向 + 同步线程 | RIP 相对重定向到 `RemoteState` 槽位 | 对齐 | 机制已测试／B1 待完成 |
| 星穹铁道 FPS 解锁 | 直接写变量 | 直接写变量 + mov 翻转 | 对齐 | 机制已测试／B1 待完成 |
| 原神自定义 DPI | 替换 `GetDPI` prologue | 相同补丁与 prologue 字节 | 对齐 | 机制已测试／B1 待完成 |
| 原神 Mobile UI | 生命周期函数入口 Hook + 进程内 IL2CPP 调用 | 自解除函数入口 detour、恢复原生命周期、游戏线程 UI/Input setter | 对齐 | B1 正常会话与崩溃恢复通过 |
| 星穹铁道 Mobile UI | 进程内 IL2CPP 调用 | code stub + 远程调用 | 机制对齐 | payload 仍受 B1 门控 |
| 启动参数渲染 | `-screen-width/height` | 仅使用同一组已验证参数 | 对齐 | B1 待完成 |
| 安装检测 | 启动器注册表遍历 | 启动器注册表遍历，并正确打开渠道子键 | 对齐并修错 | 注册表测试通过 |
| Auto Profile 选择 | 按显示器条件选择配置 | 一次采集显示事实后按 specificity/priority 选择 | 对齐 | B1 已通过 |

## 有意的行为改进

| 行为 | 上游旧工具 | HoyoFlux | 原因 |
| --- | --- | --- | --- |
| 持久分辨率污染 | 会话分辨率残留在游戏设置中并影响官方启动 | 快照 → 事件驱动守护 → 验证恢复；崩溃时保留 Journal | 这是本次重写要解决的根本问题 |
| Alt-Tab 限帧 | 通过 `GetAsyncKeyState`／`Sleep` 轮询；关闭功能仍残留 Hook | `power_save disabled` 时不注册任何内容；启用时使用 `EVENT_SYSTEM_FOREGROUND`，每次精确写入 4 字节 | 无隐式 Hook，可做回归测试 |
| 热键 | `GetAsyncKeyState + Sleep(50)` 轮询 | `RegisterHotKey` + 消息循环 | 避免忙等 CPU 消耗 |
| 不支持的功能 | 配置后静默无效 | 通过能力契约在创建进程前给出原因并停止 | 不允许静默跳过 |
| 崩溃恢复 | 无恢复／直接清理 Journal | 恢复 → 验证 → 成功后清理；失败则保留 Journal | 启动器崩溃后仍可恢复状态 |
| 命令行引用 | 手工预引用后再次引用 | 单一严格 `CommandLineToArgvW` 编码器，并验证往返一致 | 正确处理带空格、引号与反斜杠的路径 |
| 配置解析 | `stoi` 异常路径 | `from_chars` + 显式范围检查，并提供 schema 键 | 错误配置会报告问题而非崩溃 |
| 注入面 | 手工 DLL loader、通用 shellcode | 固定页面 code stub，由各 PatchPlan 独立持有，不加载 DLL | 只保留最小必要机制 |

## 验证状态

- 自动化：Debug 与 Release preset 均已完成构建；Debug 与 Release 的完整
  CTest 均为 11/11 通过（Windows HKCU 可写环境）。
- 原神 Mobile UI：B1 正常会话、功能门与崩溃恢复均通过；telemetry 偏移修复通过逐字段有效地址测试，修复后真机复测按计划跳过。
- 省电模式、FPS 热键和持久状态守护的 B1 真机回归均已通过；分辨率配置档、星穹铁道 Mobile UI 以及持久状态 Tests A–D 的剩余专项门仍按各自状态推进。持久状态 A/B 步骤见 [persistent-state-experiment.md](persistent-state-experiment.md)。
