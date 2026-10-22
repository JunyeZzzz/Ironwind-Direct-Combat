# 铁风开发工具

统一入口：`tools/ironwind.ps1`。所有命令都可从 `_moddev/BannerlordCombat/` 执行，不依赖当前终端所在目录。

## 日常命令

```powershell
# 查看当前启用通道、游戏进程、构建新鲜度及开发/正式各处 DLL 哈希
.\tools\ironwind.ps1 Status

# 只做静态检查，不编译、不安装
.\tools\ironwind.ps1 Check

# 日常代码验证：安装为独立开发 Mod、切到开发通道、核对
.\tools\ironwind.ps1 DevFinish

# 随时在本地开发版与 Steam Workshop 正式版之间切换；两者不会同时启用
.\tools\ironwind.ps1 UseDev
.\tools\ironwind.ps1 UseStable

# 确认正式发布时：只生成正式包、归档和事实报告，不覆盖订阅目录
.\tools\ironwind.ps1 Finish
.\tools\ironwind.ps1 Finish -ReplaceArchive  # 已核实同版本重打包时显式替换归档

# 只读查看构建、归档、符号、备份和报告占用
.\tools\ironwind.ps1 Housekeeping

# 实机测试前记录日志起点；不会删除或清空 RE_Kenshi_log.txt
.\tools\ironwind.ps1 SessionStart -Label v85-melee

# 游戏退出后，仅分析刚才标记的测试范围
.\tools\ironwind.ps1 AnalyzeLog -OpenReport

# 用户确认某版值得保留后，制作完整恢复快照
.\tools\ironwind.ps1 Snapshot -Label melee-baseline
```

运行 `.\tools\ironwind.ps1 Help` 可查看简表。

## `DevFinish` 与 `Finish`

`DevFinish` 是日常迭代默认入口。它执行静态检查、构建，把产物安装到 `Kenshi/mods/IronwindCombatDev/`，将 `mods.cfg` 原位置切换为 `IronwindCombatDev.mod`，再做一致性验证。开发设置页标题显示 `[DEV] 铁风执刃`。它仍会在安装 DLL 发生变化时保留回滚 DLL，但不制作版本 ZIP，也不把同一 PDB 再复制进永久符号目录。

`Finish` 自动执行：

1. 版本、源码、脚本语法、运行资源、中文 TSV 和 Mod 加载顺序检查。
2. Visual C++ 2010 x64 Release 编译。
3. 同时生成与 DLL 精确匹配的 PDB 和 `BUILD-STAMP.json`。
4. 重新生成 `package/IronwindCombat/` 正式暂存目录并制作版本 ZIP。
5. 核对构建与正式包 DLL 哈希，生成事实报告。
6. 保持 Steam Workshop `3824431439` 和当前 `mods.cfg` 不变；正式包需由维护者随后上传。

它不会自动把版本号加一，也不会自动声称画面或完整回归已经验证。同版本 ZIP 中若已有不同 DLL，部署会在安装前停止；应正常增加 `VERSION`，不能用覆盖掩盖版本差异。`-ReplaceArchive` 只用于维护者明确核实后的特殊恢复操作。

底层 `Deploy` 接受 `-Channel Development|Release`；默认是 Development。Release 通道强制要求 `-SkipInstall`，`Finish` 会自动带上，避免把未发布代码伪装成 Steam 正式版。`Deploy` 和 `Finish` 也接受 `-SkipArchive` 与 `-SkipSymbols`。

## 双通道约束

- 正式版唯一来源：Steam Workshop `3824431439`，启动器身份 `IronwindCombat.mod`。
- 开发版唯一来源：本地 `mods/IronwindCombatDev/`，启动器身份 `IronwindCombatDev.mod`。
- `UseDev`／`UseStable` 只改这两个身份，并先保存 `mods.cfg` 备份；其他 Mod 的顺序和内容原样保留。
- 两个身份同时出现属于配置冲突，`Check`、部署和开发验证都会拒绝继续。
- 两个通道可保留兼容 DLL 名 `WASDCombatPlugin.dll`，因为它们位于不同 Mod 目录；不得手工把两个 `.mod` 文件放进同一目录。

## 动态日志分析

分析器为 `tools/Analyze-IronwindLog.ps1`，规则在 `tools/log-rules.json`：

- 自动识别最近一次插件加载，或读取 `SessionStart` 保存的行号，仅分析本轮测试。
- 从每行首个事件名称动态建立事件清单；新增 C++ 日志标签即使不在规则库中也会出现在“尚未分类的新事件”。
- 配置文件只描述事件分组和风险正则，不需要为每个新标签修改分析器代码。
- 自动配对原版攻击启动、impact、verdict、真实 `_getHit`、红色命中标记与结束事件。
- 按当前样本的中位数和 MAD 检测时长离群，而不是固定假定每种武器都应使用同一时长。
- 同时输出 Markdown 人读报告和 JSON 机器报告，风险项附带前后日志上下文。

报告只代表代码路径与日志证据。动作是否自然、箭头是否好看、格挡手感是否正确仍必须由游戏画面确认。

## 目录语义

- `runtime/`：运行资源真源，包含 `.mod`、RE_Kenshi 清单、中文设置和 shader。
- `build/`：当前构建输出、PDB 与构建指纹。
- `package/IronwindCombatDev/`：日常开发暂存；只含 `IronwindCombatDev.mod`。
- `package/IronwindCombat/`：正式发布暂存；只含 `IronwindCombat.mod`。
- `package/symbols/`：按版本和 DLL 哈希保存的符号对。
- `reports/log-analysis/`：动态日志 Markdown/JSON 报告。
- `reports/releases/`：自动构建与部署事实报告。
- `reports/session-state/`：当前实机测试起点。
- `backup/`：回滚 DLL 和显式稳定快照。

`Housekeeping` 只统计这些目录的文件数和 MiB，不自动删除。需要长期保留的节点用 `Snapshot` 明确冻结；实际清理必须另行核对目标，不能把生成物清理与功能修改混在一次操作中。

## 安全规则

- 游戏运行时拒绝替换 DLL或制作稳定快照。
- `-SkipBuild` 只有在源码、版本、DLL、PDB 与构建指纹全部匹配时才允许部署。
- 开发安装目录的个人 INI 默认保留。
- 原 `WASDCombatPlugin.mod` 与铁风同时启用时停止部署。
- `IronwindCombatDev.mod` 与 `IronwindCombat.mod` 同时启用时停止部署。
- 快照和发布事实不等于完整游戏回归。
