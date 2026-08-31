# HoyoFlux

> 面向 HoYoverse PC 游戏的会话级启动器、显示配置与运行时控制器。

HoyoFlux 是对经典原神／崩坏：星穹铁道 FPS 解锁工具的独立重实现与架构重设计。它**不是**任何现有项目的 fork 或“修改版”fork：本仓库拥有全新的 Git 历史、架构、配置模型和用户界面。

**当前稳定版本：`1.0.0`。** 架构阶段（A1–A10）与功能收口阶段（F0–F12）已实现并通过单元测试；下方功能表按各功能的真机验证门（B1）标明状态。

```text
hoyoflux.exe                                      # 无界面 Auto 一键启动原神
hoyoflux-cli.cmd launch genshin --profile desktop  # PowerShell 同步 CLI
hoyoflux-cli.cmd launch genshin --profile my_mobile
hoyoflux-cli.cmd launch starrail --profile auto -- -popupwindow   # -- 后为透传参数
hoyoflux-cli.cmd doctor        # 只读环境与能力报告
hoyoflux-cli.cmd profile match genshin   # 解释显示器匹配与 fallback
hoyoflux-cli.cmd state-dump genshin
```

无参数双击 `HoyoFlux.exe` 会按 `[launcher]` 读取本次显示环境，优先选择用户定义且明确命中的 Profile；没有命中时使用 `[defaults].genshin`（内置为 `desktop`）。Auto 每次重新判断，不保存上次选择。Profile 可以按 `resolution`、`aspect_ratio`、`portrait`、`device_name` 和 `priority` 定义匹配条件；其中 `resolution` 指远程设备在 Windows 中实际呈现的显示模式，不是设备原生屏幕分辨率。

生产 EXE 保持 `asInvoker`。无参数 One-click 和 `launch` 在确实需要时自动请求 UAC，并同步等待提升后的子进程；`doctor`、`profile`、`version`、`state-dump` 等只读命令不会因未管理员运行而失败。UAC 取消时 One-click 安静退出，CLI 输出 `elevation cancelled` 并返回非零码。

```toml
[launcher]
game = "genshin"
profile = "auto"
region = "auto"
notifications = true

[defaults]
genshin = "desktop"
starrail = "starrail_desktop"
```

## 功能简介

- 从 HoYoPlay 注册表安装路径启动国服或国际服原神／崩坏：星穹铁道。
- 对单次游戏会话应用一个**配置档（profile）**。配置档声明的每项功能都会映射到一项能力；若适配器无法满足，启动会在游戏进程创建前明确失败，不会静默跳过。`hoyoflux doctor` 会输出同一份能力报告。
- `hoyoflux doctor` 会区分 `[ OK ]`、`[WARN]`、`[SKIP]` 和 `[FAIL]`；未安装的可选游戏与未提升权限分别是跳过和警告，不会单独导致 Doctor 失败。
- 会话级分辨率：启动前快照游戏持久化设置；运行期间通过事件驱动的 `RegNotifyChangeKeyValue` 守护（无轮询）；退出后恢复。
- 崩溃安全：schema 2 Journal 保存回滚状态；下次运行会先恢复并验证，成功后才删除 Journal。恢复失败时保留 Journal 以便重试。
- 游戏运行期间提供运行时控制：省电模式通过前台窗口事件响应（不轮询），每次只写入四字节；热键（END 切换、Ctrl+Up/Down）通过会话 FPS 通道调节帧率。关闭省电模式时**不会注册任何监听器**，HoyoFlux 无法在 Alt-Tab 时影响 FPS。

## 功能状态

“已实现”表示自动化测试已覆盖。真机验证按功能分别通过 B1 门管理。原神 Mobile UI 的正常会话与崩溃恢复真机验证均已通过；其他项目按下表标记。

| 功能 | 原神（国服／国际服） | 崩坏：星穹铁道（国服／国际服） |
| --- | --- | --- |
| Auto Profile 选择 | B1 已通过 | B1 已通过 |
| FPS 解锁 | 已实现 | 已实现 |
| 自定义分辨率 | 已实现¹ | 已实现¹ |
| 窗口化／独占全屏 | 已实现¹ | 已实现¹ |
| 无边框全屏 | 不支持² | 不支持² |
| 显示器选择 | 不支持³ | 不支持³ |
| Mobile UI | 已支持⁵ | 验证门控中⁴ |
| 自定义 DPI | 已实现¹ | 不支持 |
| 省电模式（事件驱动） | B1 已通过 | B1 已通过 |
| FPS 控制热键 | B1 已通过 | B1 已通过 |
| 会话持久状态守护 | B1 已通过 | B1 已通过 |

- ¹ 已实现并通过单元测试；分辨率、窗口模式和原神自定义 DPI 的真机验证仍待完成（B1）。
- ² 无法用 Unity 启动参数表达；请求该功能时会明确失败，不会猜测性执行，后续将通过持久状态路径重新评估。
- ³ 尚无经过验证的机制；请求该功能时会给出原因并停止启动。
- ⁴ 星穹铁道 bootstrap 机制已实现并通过单元测试，但在真机验证前拒绝加载其 payload。
- ⁵ 原神函数入口 detour、自解除 Hook、原生命周期恢复、游戏线程 UI/Input setter 以及崩溃恢复均已通过 B1 真机验证。

## 项目范围

仅支持 Windows x64。生产 EXE 是无主窗口的 Windows 应用：双击执行 One-click，携带显式参数时附加父控制台并保留 CLI。随包提供的 `hoyoflux-cli.cmd` 用 `start /wait` 包装显式 CLI，确保 PowerShell 等待 GUI 子系统 EXE 完成；直接调用 EXE 仍可用于交互式调试。通知图标由轻量 Win32 worker 临时持有，每次通知约 6 秒后自动清理，不会贯穿整个游戏 Session；启动通知不阻塞，成功/错误终态会在退出前等待当前通知自然结束，也不提供托盘交互。当前不提供 GUI 设置编辑器、在线签名更新、驱动、通用 DLL 注入器或反作弊规避功能。

## 构建

要求：CMake ≥ 3.24、Ninja 和支持 C++23 的编译器。本项目正式支持的工具链为**面向 `x86_64-w64-windows-gnu` 的 clang**（MinGW-w64 + UCRT LLVM）；所有依赖均为仅头文件依赖，并通过 CMake `FetchContent` 获取。

```text
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## 许可证

采用 MIT 许可证，详见 [LICENSE](LICENSE)。第三方声明与上游归属信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
