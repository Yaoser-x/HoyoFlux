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
   ^
   |
  cli
```

| 层 | 目录 | 职责 |
| --- | --- | --- |
| domain | `src/domain` | 纯数据模型（Profile、LaunchRequest、Error 等），无 I/O。 |
| platform | `src/platform/win32` | Win32 边界：进程创建、注册表、PE、显示器、权限。 |
| scan | `src/scan` | 特征扫描、已编译 pattern 与模块快照。 |
| patch | `src/patch` | 补丁引擎、远程内存与远程状态。 |
| game | `src/game` | 游戏知识：适配器与各游戏签名。 |
| session | `src/session` | 会话引擎、Journal、显示守护与回滚。 |
| app | `src/app` | 命令分发、验证与组装。 |
| frontend | `src/frontend` | CLI／托盘。 |

## 设计决策

- **使用标准 Win32，不引入 syscall 层。** 旧项目出于反分析目的通过私有 NTSYSAPI syscall shim 调用系统功能；HoyoFlux 使用普通 Win32 API（`CreateProcessW`、`ReadProcessMemory` 等），不尝试规避反作弊。
- **自包含远程状态。** 旧 shellcode 跨进程读取解锁器的 `FpsValue`，导致启动器必须常驻。HoyoFlux 在游戏内分配 `RemoteState`；固定配置档在 patch + resume 后不依赖常驻启动器。
- **会话级显示配置。** 启动前快照游戏持久设置，运行时守护，退出后恢复，不污染官方启动器配置。
- **GameAdapter 生成 PatchPlan，PatchEngine 负责执行。** 游戏知识与内存写入解耦。
- **TOML 配置只解析一次。** 配置解析不进入热路径。
- **统一 UTF-8。** 旧项目使用 UTF-16LE 源文件，本项目全部采用 UTF-8。

## 会话生命周期

```text
Idle -> Preparing -> Launching -> Resolving -> Patching -> Running
                                                              |
                                                              v
                                      Restoring -> Completed
```

任一阶段失败都会进入 `Failed -> Rollback -> Completed`。只有 SessionEngine 可以恢复状态、终止游戏或退出。
