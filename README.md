<p align="center">
  <img src="assets/hoyoflux-logo.png" width="128" alt="HoyoFlux Logo">
</p>

# HoyoFlux

> 面向 HoYoverse PC 游戏的便携式会话启动器、显示配置与运行时控制器。

HoyoFlux 是对经典原神／崩坏：星穹铁道 FPS 解锁工具的独立重实现与架构重设计。项目拥有独立的代码、架构、配置模型和 Git 历史。

## 使用

发布包只有一份 `hoyoflux.exe`，不需要安装，也不会在 AppData 中创建新的配置或运行状态。

1. 把 EXE 放进一个普通、可写的文件夹并双击。
2. 首次运行会在 EXE 旁生成 `config.toml`，用默认编辑器打开后停止，不会启动游戏。
3. 修改并保存配置，再次双击 EXE 即可启动。

运行后目录如下：

```text
HoyoFlux/
├─ hoyoflux.exe
├─ config.toml
└─ data/
   ├─ state/          # 崩溃恢复记录
   ├─ backups/        # 旧配置和迁移备份
   └─ diagnostics.txt # 按需生成的诊断报告
```

程序没有公共命令行功能。游戏选择、配置档、可选游戏路径和透传参数都写在配置文件中。

## 简洁配置

```toml
schema = 2

[launcher]
game = "genshin"
profile = "auto"
action = "launch"

[profiles.desktop]
game = "genshin"
fps = 120

[profiles.starrail_desktop]
game = "starrail"
fps = 120
```

每个配置档可直接填写这些常用字段：

```toml
[profiles.example_mobile]
game = "genshin"
fps = 60
resolution = "1920x1080"
mobile_ui = true
dpi_scale = 2.0
power_save = true
power_save_fps = 30
hotkeys = true
match = { resolution = "1920x1080", priority = 100 }
```

`match` 支持 `device_name`、`resolution`、`aspect_ratio`、`portrait` 和 `priority`。只要写入匹配条件，该配置档就会参与 Auto；可用 `auto_select = false` 显式停用。匹配分辨率是远程设备在 Windows 中呈现的当前显示模式，和游戏的 `resolution` 相互独立。

高级字段包括 `fullscreen`、`persistence`、`monitor`、进程 `priority`、`exe` 和 `args`。相对 `exe` 路径以 HoyoFlux 所在目录为基准。完整注释示例见 [config.example.toml](config.example.toml)。

## 诊断

将配置改为：

```toml
[launcher]
action = "diagnose"
```

再次双击后，HoyoFlux 会生成并打开 `data/diagnostics.txt`。报告包含版本、便携路径、游戏安装与能力、显示器、配置、恢复记录和 Auto 选择解释。诊断不会启动游戏、执行恢复、迁移文件、请求管理员权限或写入游戏设置。即使配置、迁移或恢复检查失败，程序也会先尝试写出一份基础报告。

配置错误和迁移错误会显示固定的 Windows 错误窗口，其中可直接打开需要修复的配置或本次诊断报告。HoyoFlux 不会自动改写格式错误的配置，也不会在保存后自动重试。

## 从旧版迁移

首次运行便携版时，程序会检查旧版 `%LOCALAPPDATA%\HoyoFlux`：

- 先把原配置完整归档到 `data/backups`，再转换为 schema 2 并重新解析验证。
- 本地已有 `config.toml` 时保留本地版本，只归档旧配置。
- 旧版恢复记录会迁入 `data/state`，继续由原有恢复流程处理。
- 只有本地副本、备份和转换结果均验证成功后，才删除对应的 AppData 源文件。
- 两处恢复记录不同、记录损坏、游戏仍在运行或文件在迁移期间变化时，迁移停止并保留源数据。
- 中断中的迁移会在 `data/state/portable-migration.toml` 保留阶段和摘要；再次双击只会在源文件和本地副本仍与记录一致时续接。
- 不认识的 AppData 文件不会被移动或删除。

## 功能

- 从 HoYoPlay 注册表安装路径启动国服或国际服原神／崩坏：星穹铁道。
- Auto 每次采集当前显示环境，按匹配具体程度和优先级选择配置档；同秩候选会明确报错。
- 会话级分辨率与持久状态守护：启动前快照，运行期间事件驱动守护，退出后恢复。
- schema 2 Journal 提供崩溃恢复；恢复并验证成功前不会删除记录。
- 原神 Mobile UI、自定义 DPI、省电模式和 FPS 热键按各游戏能力门控。
- 生产 EXE 保持 `asInvoker`；读取配置和生成诊断不提权，实际启动游戏时才按需请求 UAC。UAC 必须使用启动 HoyoFlux 的同一 Windows 账户；使用其他账户会被安全拒绝。
- 通知图标只短暂存在，不提供常驻托盘程序。

## 功能状态

“已实现”表示自动化测试已覆盖。真机验证状态与版本支持边界见 [兼容性矩阵](docs/compatibility-matrix.md)。

| 功能 | 原神（国服／国际服） | 崩坏：星穹铁道（国服／国际服） |
| --- | --- | --- |
| Auto Profile 选择 | B1 已通过 | B1 已通过 |
| FPS 解锁 | 已实现 | 已实现 |
| 自定义分辨率 | 已实现 | 已实现 |
| 窗口化／独占全屏 | 已实现 | 已实现 |
| Mobile UI | 已支持 | 验证门控中 |
| 自定义 DPI | 已实现 | 不支持 |
| 省电模式 | B1 已通过 | B1 已通过 |
| FPS 控制热键 | B1 已通过 | B1 已通过 |
| 会话持久状态守护 | B1 已通过 | B1 已通过 |

## 构建

要求 CMake ≥ 3.24、Ninja 和支持 C++23 的编译器。正式支持的工具链是面向 `x86_64-w64-windows-gnu` 的 clang（llvm-mingw UCRT）。Release 构建静态链接所需 C++ 运行库，生成独立 EXE。

```text
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release
ctest --preset release
```

项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
