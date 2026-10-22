# 铁风执刃维护规则

本文件适用于 `_moddev/BannerlordCombat/` 及其子目录。游戏根目录的 `AGENTS.md` 只规定 Kenshi 汉化流程，不是本插件的开发说明。

## 项目身份

- 正式名称是“铁风执刃 / Ironwind: Hands-On Combat”。面向玩家的标题、包名和报告不得再把项目称为 WASD Mod。运行时旧日志名在下一次代码版本前作为诊断兼容字符串保留。
- 当前项目由本项目所有者独立维护。上游 Direct Control for Kenshi 的原有著作权和 GPL-3.0 义务必须保留，详见 `NOTICE.md` 与 `LICENSE`。
- `WASDCombatPlugin.dll`、`.ini`、`.cpp`、工程文件名、旧日志前缀及英文查找键只属兼容接口。普通维护不得为了改名破坏现有安装、个人配置、加载清单或分析器。

## 按任务读资料

不要每次通读全部历史文档，只读与任务相关的真源：

- 玩家行为、按键或设置：`README.zh-CN.md`。
- 状态机、线程、Hook、模块边界：`ARCHITECTURE.md`。
- 未完成项与当前限制：`KNOWN_ISSUES.md`。
- 验证方法：`TESTING.md`。
- 历史方案或回归来源：仅按版本查 `CHANGELOG.md`。
- 构建、部署、日志分析：`tools/README.zh-CN.md`。

`CODE_STRUCTURE.md`、`NOTES.md`、`PROGRESS.md` 是旧链接兼容入口，不再承载独立事实。

## 真源与生成物

- C++ 真源：`WASDCombatPlugin.cpp`；默认配置：`WASDCombatPlugin.ini`。
- 版本真源：`VERSION`，并同步源码中的 `IRONWIND_VERSION`。
- 运行资源真源：`runtime/`；Steam Workshop 正式版 `3824431439` 只读。本地开发版必须使用 `IronwindCombatDev.mod`，不得复用正式版身份。
- 统一入口：`tools/ironwind.ps1`；构建：`build-local.ps1`；部署：`deploy-local.ps1`。
- `build/`、`package/`、`backup/`、`reports/` 和安装目录都是输出或证据，不是源码真源。
- 发布事实由 `BUILD-STAMP.json`、`BUILD-MANIFEST.json` 和 `reports/releases/` 生成，不再手抄进度哈希。

## 不得破坏的核心边界

- 目标平台固定为 Kenshi 1.0.65、RE_Kenshi 0.3.5、x64、Visual C++ 2010 ABI。
- 优先使用 KenshiLib 的 `GetRealAddress` 或已验证导出；不得恢复旧裸 RVA。
- 输入线程只采集原始输入并写简单状态，不得访问 Character、Ogre、MyGUI 或世界对象。
- 世界、角色、移动、战斗、相机和 UI 对象只在对应游戏线程路径访问；读档、导入和退出时不得解引用旧指针。
- 玩家队伍保护是硬约束；选目标与命中回调都必须复核。
- 不得用固定毫秒数截断攻击动画；正常结束以实际 Ogre 时间轴为准。
- V 手动攻击按目标分流：敌对目标走原版 `STARTUP → CHOP_WEAPON` 与原版命中/格挡链；中立与空挥走自由动作路径；一次攻击只能结算一次。
- v116 起没有玩家主动闪避；v117 起没有 Ctrl 镜头体系；v118 起 Z/X 归还原版速度；v120 起没有固定“完美格挡”时间窗。不得把这些旧系统带回。
- 普通设置页维持紧凑；兼容和校准细项保留在 INI。

## 流程

1. 开始前运行 `./tools/ironwind.ps1 Status`，改动后运行 `Check`。
2. 仅文档或工具说明改动不重编 DLL。代码改动日常验证用 `DevFinish`；正式发布用 `Finish`。`DevFinish` 安装并启用开发通道；`Finish` 只产出正式包，绝不覆盖 Workshop 订阅目录。
3. 实机前运行 `SessionStart -Label <语义名>`；退出后用 `AnalyzeLog`。
4. 玩家确认值得长期保留的版本才运行 `Snapshot`。
5. `Housekeeping` 只报告归档占用和可清理候选，不自动删除。

## 文档单一归属

- README：当前玩家行为；ARCHITECTURE：结构与不变量；KNOWN_ISSUES：只写尚未解决的项。
- TESTING：可重复验证方法；CHANGELOG：已发生的历史；NOTICE：身份、上游和兼容命名。
- 行为变化才更新 README；结构变化才更新 ARCHITECTURE；修复完成后从 KNOWN_ISSUES 删除并写入 CHANGELOG。
- 不再逐版本复制同一段状态，不把自动生成的哈希手抄进 Markdown。

## 验证与报告

- 使用 `TESTING.md` 的证据等级：静态、启动、日志、画面、回归。
- 编译成功不等于功能修复；有日志不等于画面正确；近似挥刀扇区不得描述为精确刀刃碰撞。
- 完成报告应列出改动文件、是否编译/部署、实际测试及证据等级、未验证项、回滚与归档情况。
