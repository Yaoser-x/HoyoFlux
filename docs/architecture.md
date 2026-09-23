# HoyoFlux 架构

## 分层与依赖图

依赖严格向下：底层不得引用上层。每层均为独立 CMake target，由编译器强制执行边界。

```text
        domain
        ^   ^
        |   |
platform   scan
   ^        ^
   |        |
 patch  <---+
   ^
   |
 game
   ^
   |
 session
   ^
   |
  app
```

| 层 | 目录 | 职责 |
| --- | --- | --- |
| domain | `src/domain` | 纯数据模型（Profile、LaunchRequest、Error 等），无 I/O。 |
| platform | `src/platform/win32` | Win32 边界：进程创建、注册表、PE、显示器、权限。 |
| scan | `src/scan` | 特征扫描、已编译 pattern 与模块快照。 |
| patch | `src/patch` | 补丁引擎、远程内存与远程状态。 |
| game | `src/game` | 游戏知识：适配器与各游戏签名。 |
| session | `src/session` | 会话引擎、Journal、显示守护与回滚。 |
| app | `src/app` | 便携路径、旧数据迁移、配置驱动入口、诊断报告与 LaunchService 组装。 |
| frontend | Win32 Shell | 无主窗口双击入口、UAC 引导、文件打开与瞬时通知。 |

## 设计决策

- **使用标准 Win32，不引入 syscall 层。** 旧项目出于反分析目的通过私有 NTSYSAPI syscall shim 调用系统功能；HoyoFlux 使用普通 Win32 API（`CreateProcessW`、`ReadProcessMemory` 等），不尝试规避反作弊。
- **自包含远程状态。** 旧 shellcode 跨进程读取解锁器的 `FpsValue`，导致启动器必须常驻。HoyoFlux 在游戏内分配 `RemoteState`；固定配置档在 patch + resume 后不依赖常驻启动器。
- **会话级显示配置。** 启动前快照游戏持久设置，运行时守护，退出后恢复，不污染官方启动器配置。
- **GameAdapter 生成 PatchPlan，PatchEngine 负责执行。** 游戏知识与内存写入解耦。
- **便携路径是显式依赖。** `AppPaths` 从当前 EXE 的绝对路径派生配置、数据、备份、诊断和 Journal 位置；Session 层只接受显式 Journal 路径，不读取 AppData 或环境变量。
- **TOML 配置只解析一次。** schema 2 配置档使用平铺字段；schema 1 经备份、转换和重新解析后再原子替换，不进入热路径。
- **统一 UTF-8。** 旧项目使用 UTF-16LE 源文件，本项目全部采用 UTF-8。
- **Auto 是无状态决策。** 显示事实只采集一次，匹配按 `(specificity, priority)` 排序；同秩候选报错，无匹配时使用 per-game fallback，不保存上次 Profile。
- **配置驱动的单一启动路径。** 双击入口先完成便携目录检查和旧数据迁移，再读取 Profile 并交给 `LaunchService` 与 `SessionEngine`。首次配置和诊断在未提权进程完成；只有 `action = "launch"` 才通过 `ShellExecuteExW(runas)` 按需提权。通知是 best-effort，不参与会话成败；轻量 Win32 worker 使用内嵌 Logo，约 6 秒后由同一线程清理，不形成常驻托盘程序。
- **迁移先证明再删除。** AppData 文件先完整归档，本地副本原子写入并由正式解析器或 Journal 解析器验证，删除前再次读取源文件确认内容未变化。不同恢复记录、损坏记录和活动会话均停止迁移并保留数据。

## 会话生命周期

```text
Idle -> Preparing -> Launching -> Resolving -> Patching -> Running
                                                              |
                                                              v
                                      Restoring -> Completed
```

任一阶段失败都会进入 `Failed -> Rollback -> Completed`。只有 SessionEngine 可以恢复状态、终止游戏或退出。
