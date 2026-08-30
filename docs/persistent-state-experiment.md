# 持久状态实验（计划 §7.2）

目标：确认原神／崩坏：星穹铁道运行时**实际改写哪些注册表值**，使 HoyoFlux 只保护必要状态，不多也不少。不要依据本文扩展代码中的字段列表；本文只记录实验步骤与结果。代码会捕获受监控根键下现有的全部 `Screenmanager*` 值并逐字节恢复，因此实验真正要验证的是**根键覆盖范围**：所有被游戏修改的值都必须出现在受监控根键中。

## 状态：等待真机验证

1.0.0 发布门（计划 Tests A–D）要求在安装真实游戏的机器上完成此实验。在此之前，会话守护会通过能力报告如实标记状态，兼容性矩阵也会将其记为待验证。

## 工具

```text
hoyoflux state-dump genshin     # 只读输出受监控根键
hoyoflux state-dump starrail
```

输出会逐个候选根键显示：键是否存在，以及每个 `Screenmanager*` 值解码后的 DWORD 数值。该命令不会写入任何内容。

## 实验步骤（每个游戏、每种安装区域：国服／国际服）

1. **Dump A（桌面基线）。** 通过官方 HoYoPlay 启动游戏，在游戏内设置已知桌面分辨率，退出后执行 `hoyoflux state-dump <game> > dump-A.txt`。
2. **污染运行。** 通过 HoyoFlux 使用 iPad／移动端配置档启动，游玩或进入主菜单后退出。
3. **Dump B。** 执行 `hoyoflux state-dump <game> > dump-B.txt`。
4. **比较。** 执行 `git diff --no-index dump-A.txt dump-B.txt`。每一处变化都是游戏改写的值。F2/F3 门要求所有变化都属于已输出根键下的 `Screenmanager*` 值，否则说明适配器的根键列表不完整。
5. **恢复检查。** 执行 `hoyoflux recover`（或等待后续任一会话正常结束）后再次输出；所有变化值都必须恢复为 Dump A。

## 当前受监控根键（候选，需通过本实验验证）

| 游戏 | 区域 | 根键（位于 HKCU 下） |
| --- | --- | --- |
| 原神 | 国服 | `Software\miHoYo\原神` |
| 原神 | 国际服 | `Software\miHoYo\Genshin Impact` |
| 崩坏：星穹铁道 | 国服 | `Software\miHoYo\崩坏：星穹铁道` |
| 崩坏：星穹铁道 | 国际服 | `Software\Cognosphere\Star Rail` |

若真机输出表明游戏在其他位置写入显示状态（文件配置或其他注册表键），应将该存储位置加入适配器的 `persistent_state_roots()`，然后重新执行本实验。
