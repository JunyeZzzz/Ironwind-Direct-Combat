# 铁风执刃架构

本文描述 v120 的当前结构。历史方案见 `CHANGELOG.md`，当前缺陷见 `KNOWN_ISSUES.md`。项目正式名称为“铁风执刃 / Ironwind: Hands-On Combat”；下文出现的 `WASDCombatPlugin` 是必须保留的兼容文件名，不是产品归属，完整声明见 `NOTICE.md`。

## 1. 产物链

```text
WASDCombatPlugin.cpp + WASDCombatPlugin.ini + VERSION + runtime/
                         │
                         ├─ build-local.ps1
                         ▼
        build/WASDCombatPlugin.dll + PDB + BUILD-STAMP
                         │
                         ├─ deploy-local.ps1
                         ├──────────────────────────────┐
                         ▼                              ▼
       package/IronwindCombatDev/          package/IronwindCombat/
       IronwindCombatDev.mod               IronwindCombat.mod
                         │                              │
                         ▼                              ├─ 正式 ZIP / 符号 / 事实报告
       Kenshi/mods/IronwindCombatDev/                   └─ 不安装、不改 Workshop
                         │
                         ▼
       mods.cfg 互斥启用开发版或 Workshop 正式版 3824431439
```

`tools/ironwind.ps1` 是上述步骤的统一编排入口。`DevFinish` 安装并切换到开发身份；`Finish` 只生成正式发布候选，不接触订阅目录或运行通道；`UseDev`／`UseStable` 互斥改写 `mods.cfg` 并保存备份。两个包都有 `CHANNEL.json`，且各自只能包含一个 `.mod` 身份。`SessionStart → AnalyzeLog` 才建立单次实机日志证据。日志分析的事件分组和风险正则位于 `tools/log-rules.json`，未知新事件会自动进入报告，不因规则库未更新而被丢弃。

`runtime/IronwindCombat.mod` 是 0 条 FCS 记录的占位真源；流水线按通道输出为 `IronwindCombatDev.mod` 或 `IronwindCombat.mod`。`RE_Kenshi.json` 负责加载 DLL。真正玩法逻辑在 DLL；`materials/deferred/character.hlsl` 为第一人称头部像素隐藏提供 shader 参数；`Settings.zh-CN.tsv` 为设置页和提示提供 UTF-8 中文，开发包只在生成阶段为标题增加 `[DEV]`。

## 2. 线程模型

| 执行位置 | 入口 | 可以做什么 | 禁止事项 |
| --- | --- | --- | --- |
| Windows 输入轮询线程 | `PollThread` | 读取按键、鼠标键，写输入边沿和简单标志 | 不访问世界、角色、Ogre 或 MyGUI 对象 |
| DirectInput 鼠标线程 | `fpDInputPollThread` | 累计相对鼠标和滚轮数据 | 不驱动镜头，不访问游戏对象 |
| 游戏主线程 | `mainLoop_hook`、战斗钩子、移动钩子 | 控制角色、状态机、任务、攻击、伤害与读档保护 | 不能信任跨读档缓存指针 |
| 相机更新路径 | `cameraUpdate_hook` | 驱动越肩／第一人称镜头、碰撞、准星与楼层显示 | 退出或加载时不得使用失效镜头节点 |
| UI/玩家控制路径 | `playerControl_hook`、设置窗口钩子 | 原版上下文菜单、设置控件、鼠标所有权 | 不绕开原版选项处理函数制造伪任务 |

轮询线程与游戏线程之间通过 `volatile` 输入标志和累计值通信。每帧开始把易变输入复制到 `s_frame*` 快照，避免在大量角色移动钩子中反复跨线程读取。

## 3. 顶层执行顺序

`mainLoop_hook` 的顺序是行为契约：

1. 检测真实读档、区块流式加载和世界销毁。
2. 在硬关闭期间只运行原版循环并等待安全重获条件。
3. 更新选中角色和控制锚点。
4. 处理 V 模式进入、退出和控制对象切换。
5. 更新手动战斗输入、目标与自由动作。
6. 在原版 AI 前应用直接移动意图。
7. 调用原版主循环，让动画、物理、受击和未接管角色正常推进。
8. 原版 AI 后清理会抢夺控制权的任务或移动结果。
9. 再次应用 WASD／原地保持，消除同帧竞争。
10. 发布诊断与性能统计。

改变顺序可能重新引入移动抖动、攻击 AI 抢控、库存任务恢复或读档崩溃。

## 4. 控制状态机

```text
MODE_VANILLA
   │ V
   ▼
MODE_FREE_MOVE ──选择/双击/G──► 更换 s_freeMoveAnchor
   │                         │
   │ 读档/世界失效           └─只接受玩家队伍中的有效角色
   ▼
LOAD_GUARD / POINTER_LOSS
   │ 世界、玩家、选择、movement 连续稳定 60 帧
   ▼
MODE_FREE_MOVE 重获控制
   │ V 或硬关闭
   ▼
MODE_VANILLA
```

核心对象是 `s_freeMoveAnchor`。它表示当前由 WASD 控制的角色，不等同于任意时刻的 UI 选中对象。缓存的 `s_anchorMovement` 只在世界存活且指针验证通过时使用。

## 5. 镜头状态机

```text
原版镜头
  └─ V ─► 越肩 OTS
             ├─滚轮到 3─► 第一人称
             │              └─向外滚一次─► OTS（至少 6）
             ├─F─────────► 第一人称/OTS 手动切换（P 不再使用）
             ├─库存──────► 保留 OTS/FP 意图并释放鼠标
             └─V 关闭────► 恢复原版镜头参数与跟踪状态
```

OTS 与第一人称共用脱离式 Ogre 相机基础设施，但任一时刻只有一个模式拥有镜头。室内 OTS 目标距离最多 35；墙体和地形可进一步拉近。滚轮用户距离独立保存，遮挡消失后平滑恢复。第一人称按真实头骨或备用颈部位置计算眼位，并用 shader、头发／头盔隐藏和双面身体材质控制自体可见性。

v117 删除上游 Ctrl 镜头分支。Ctrl 从未改变 `s_mode`、WASD 锚点或战斗所有权，只曾翻转 `s_camRotateToggle`，让仍处于 `MODE_FREE_MOVE` 的角色退出 OTS 回到原版镜头。现在 OTS 意图直接由 `MODE_FREE_MOVE && !firstPerson` 推导；UI 只暂时释放鼠标，V 退出、读档和世界销毁仍负责真实相机复位。通用 Ctrl 键名解析保留，供原版菜单和用户其他绑定使用。

OTS 视觉位置在原版 `CameraClass::update` 返回后由 `otsCamDrive` 写入，而原版音频也在该更新内处理。声音有两个独立距离边界：`Character::audioEvent` 会在事件发布到 Wwise **之前**按视觉相机距离拒绝非零 `SoundRange`，之后 `CameraClass::updateAudio` 才提交监听器。v89 因此只对 V 模式当前受控角色把事件范围改为原版 `SOUNDRANGE_ALWAYS`，其他角色和 V 外路径原样调用；同时沿用 v88 的监听器钩子，把位置改为角色 `audioData.Position`，无效才回退身体根节点。监听器两个朝向向量及全部音源参数保持原版。任一层的角色、更新列表、读档或销毁门失效时都不覆盖。角色根部的 `teleport` 仍只负责流式加载／rig 一致性，不能兼作声音修复。OTS 轨道使用 `pivot - fwd * distance`，所以俯角越大，水平后距按 `cos(pitch)` 缩短；接近 74° 时约剩 27%，画面会靠近头顶。它与 `sphereTrace` 的墙体拉近是两种问题，日志必须用 `dist/target/zoom/hit/normal_y/pitch` 区分。

## 6. 移动所有权

- WASD 方向按当前 OTS／第一人称／原版镜头朝向计算。
- 默认服从角色真实最大速度，因此伤势、负重、种族与装备仍生效。
- 镣铐额外限制为慢速移动，避免直接控制绕过原版限制。
- 攻击或已接受格挡期间速度缩放为 20%。
- 格挡且没有 WASD 时保持水平位置。
- 受击硬直、招架、倒地和格挡优先于普通移动。v97 起只要已接受的右键格挡请求存活，WASD 就不产生位移；松开后恢复。原版移动／战斗更新仍运行以推进动画，插件只消除横向位移。
- 确认命中后由原版 `_getHit(..., stumble)` 决定是否进入反应；只有原版排队或实际进入 STUMBLE/REACTION_BLOCK 时才拥有身体，空闲保持和两条移动注入路径必须让路，状态结束当帧恢复。
- 战斗 AI 只在玩家未拥有移动和动作控制、或必须推进真实反应状态时运行。
- 门、家具、炮台、奴隶指令、医疗任务和玩家点选命令分别处理，不能用一个通用“删任务”规则覆盖。

## 7. 手动战斗状态机

```text
IDLE/READY
  ├─按住左键─► ATTACK_AIM（只选向，不播放蓄势动画）
  │                ├─鼠标移动─► 带死区与迟滞的四向选择
  │                ├─右键─► 取消待发攻击；手动模式才进入 FREE_BLOCK
  │                └─松开左键─► ATTACK_PENDING
  │                                      ├─精确目标／附近敌对载体─► 原版 CHOP_WEAPON ─► 原版命中/格挡 ─► IDLE
  │                                      └─附近无人／非角色─► FREE_ATTACK ─► 空挥 ─► RECOVERY ─► IDLE
  │                                                       └─再松开左键─► QUEUED_ATTACK（最多一个）
  │                                      └─命中后右键─► FREE_BLOCK
  ├─右键（手动格挡）─► FREE_BLOCK ──松开─► IDLE（按多久都不转成交互）
  ├─右键（自动格挡）─► 持续请求防御，AI 选向；松开即停止
  ├─靠近视野内目标─► AUTO_INTERACTION_TARGET
  │                         └─空闲按 E─► 目标的原版上下文菜单 ──菜单关闭─► IDLE
  ├─Shift+方向─► 普通 WASD 移动（无铁风主动闪避状态）
  └─受到攻击─► STUMBLE / REACTION_BLOCK（原版状态优先）
```

### 攻击

方向采样来自 DirectInput 的短时相对位移，使用径向死区及跨对角线迟滞。水平输入按箭头/刀刃运动方向解释：右移的 `←` 对应 `CUT_LEFT`，左移的 `→` 对应 `CUT_RIGHT`，不再把鼠标所在侧误当成刀刃终点。手势窗口（约 180/220 ms）重启时只清累计位移，已选方向跨窗口保留作为下一窗口的种子——v39 之前连方向一起清空会造成空闲预览箭头按窗口频率闪烁、并丢失窗口刚重启时开始的划动。左键按住期间只更新输入状态和 HUD，不暂停或篡改动画时间轴；松开时才锁向并提交。右键可在提交前取消并转入格挡。v38 粘滞选向：每次按压内第一下手势以死区轻量选中，其后换向需当前手势窗口累计位移 ≥40 计数，防止回中/视角微漂在松开前翻转箭头；`combatRecentDirection` 衰减 800 ms 供下次按压继承。

攻击招式由 Kenshi 的武器、技能、单双手和角色状态筛选；铁风只用方向输入限定刀路。v78 的动作选择不受目标距离限制：先从原版攻击池收集当前角色可用的同方向 technique，再用原版 chooser 的中性参考距离补充，最后随机取一个；距离只在 impact 阶段由原版攻击区体现。v99 将空手武术纳入同一管线：`CombatClass::weaponReach()` 返回的 0 对 `SKILL_UNARMED` 是合法 chooser 参数，不能触发武器距离故障门。插件不生成武术招式，也不绕过等级、伤势、负重或 technique 的原版条件。`s_dirMoveAvailable` 因此只表示武器/武术方向能力，绿色与有无目标无关。当前挥刀或收招期间允许缓存一个方向，超过 1 秒或遭受硬直便丢弃。精确敌对/中立目标优先作为 AttackState 对象；无精确目标时先查前方敌对，再以 2×AimRange 内最近敌对作为纯执行载体。载体不改变朝向和命中资格，宽相位内确实无人时才使用自由动作。

所有有目标的近战命中都发生在原版 `CHOP_WEAPON`：铁风不计算距离、不扫描命中扇区、不手工选择多人，也不直接调用伤害。`_iHitYouAreYouHit` 只记录候选接触、格挡和闪避；只有受害者随后实际进入原版 `_getHit`，UI 才启动 1000 ms 红色命中标记。`weaponReach()` 只保留为空挥招式选择和诊断参数；空手的 0 不被换算为自定义命中距离。空手蓄意攻击中立只接受准星身体覆盖作为 AttackState 载体，实际接触仍由原版武术攻击区决定。

自由动作路径只负责空挥。敌对或中立目标由 `manualAttackStartDirect` 在左键释放帧取得角色自身的 `CHOP_WEAPON` 状态对象，并同步调用原版 `AttackState::initialiseAttack(target)`。成功后只进入这一刀的原版 CHOP；失败则不建立攻击状态、不寻路、不延迟重试，并降级为无伤害空挥。近战不再创建 `FOCUSED_MELEE_ATTACK` / `UNPROVOKED_FOCUSED_MELEE_ATTACK` 任务。

`s_attackDirectInitialising` 只在上述同步调用栈内成立，是 `chooseAttack_hook`、`attackInit_hook` 与 `setCombatState_hook` 的精确许可；`s_attackActive` 只表示已成功开始、尚未离开原版 STARTUP/CHOP 的这一刀，不能许可新的攻击初始化。这样原版仍拥有招式、攻击区、动画、通知和结算，AI 决策层不拥有等待、靠近、转向、重试或连击。

直接初始化的 CHOP 没有高层战斗任务，不能假定引擎每帧仍会调度完整 `CombatClass::go`。v68 实机证明状态对象 `AttackState::update` 不是动画段与 impact 的执行入口：调用 105 次后攻击计时仍固定在 0.200。v69 的 `manualAttackAdvanceNativeState` 通过 `GetRealAddress(&CombatClass::_NV_update)` 限定调用原版非虚基类实现；该实现看到 CHOP 后直接进入 `CombatClass::attackState`，不会进入 `CombatClassAI::update` 或 decisionState。

v79 将 `combatAiUpdate_hook` 作为玩家授权 CHOP 的最外层所有权入口：在外层 AI update 有机会把 CHOP 改回 DECISION 之前，直接运行 `manualAttackAdvanceNativeState` 并返回。`combatGo_hook` 与 `manualAttackLifecycleTick` 保留为兼容/兜底入口，三者用主循环帧序号互斥，禁止同帧双推进。原版受击反应一旦 pending/active，攻击推进立即让位给完整受击状态入场。完整 AI decision update 在普通挥刀期间不运行，因此状态执行与 AI 决策分离；正常结束仍只看原版 technique 完成或受击状态接管，不使用固定动作时长。

v81 对提前 DECISION 只做“事实型恢复”：`manualAttackTrack` 和 `manualAttackLifecycleTick` 在任何收权前都调用同一个 `manualAttackStillSwingingOrRecover`。它必须同时看到本刀 technique 未完成、同一 Ogre clip 仍存在且真实进度小于 0.98，才允许把 CHOP 恢复一次。看不到活片段便立即收权并记录诊断；不按毫秒猜测，也不重复播放动画。v80 只在后一个入口检查，实机中前一个入口先结束了 0/234/329 ms 的三刀，因此恢复从未执行。姓名和血条继续只跟随一个显示焦点，与 AttackState 载体和原版攻击区可命中的对象分离。

v82 把一次 CHOP 内部再分为 `IMPACT_ACTIVE → VISUAL_RECOVERY → DECISION`。`currentTechniqueHasFinished` 只结束 `IMPACT_ACTIVE`：此后不再调用原版 attackState，因此攻击区、verdict 和伤害不可能重复；同一 Ogre clip 继续由正常动画更新推进。实际 clip 达到 0.98 或已移除后才转 DECISION。真实受击反应在两阶段都可抢占身体。该模型使用原版时间轴，不使用攻击总时长估计。

v83 为 `VISUAL_RECOVERY → FREE_BLOCK` 增加排他交接。右键在 impact 前只登记等待；impact 后先调用原版状态转换离开 CHOP，并结束本帧自由动作处理。`s_attackGuardHandoffPending` 阻止活片段恢复门把这次主动退出重新拉回 CHOP；只有下一主循环确认攻击所有权释放后，格挡动画才可启动。任何帧都不得同时存在 `s_attackActive` 与新建 `s_freeBlockTech`。

中立目标首先使用屏幕身体投影精确选择。越肩相机在近距离会产生显著横向视差，因此保留 25° 窄扇区兜底和按下时目标捕获；这些逻辑只决定“打谁”。v64 随后建立原版 unprovoked 任务，由完整原版战斗状态生成攻击区和 impact。敌人同样只用准星/锁定建立目标；玩家小队始终排除。

### 目标与队友保护

- 敌人由准星敌对焦点或显式锁定选择，命中资格随后交给原版攻击状态机。
- 中立陌生人或非队伍盟友才使用屏幕身体区域与 25°/原版 reach 辅助选择。
- 玩家队伍通过 `isPlayerCharacter()` 与玩家花名册双重识别。
- 目标选择和实际命中前各检查一次玩家队伍保护。

### 格挡

`AiBlocksInManual=false` 是方向手动格挡：右键按下记录当前方向并立即维持相应格挡片段。攻击方向保存的是刀刃运动方向，因此 v56 在进入格挡时只交换 `CUT_LEFT/CUT_RIGHT`，把鼠标右移转换为原版右侧格挡、鼠标左移转换为原版左侧格挡；上下不交换。v120 删除额外的完美格挡计时与奖励：只要本次请求已接受、RMB 仍按住、来袭位于可防御前方且方向匹配，就立即返回原版 `HIT_SWORD`，不以可见姿势是否完全抬起作为第二道时间门。原版调用方继续负责格挡火花、声音和经验；方向不匹配时不返回武器格挡结果。

`AiBlocksInManual=true` 只把防御方向选择交给原版 AI，不交出输入时机：玩家仍须按住 RMB，`manualBlockTick` 只接受本次权限请求，不写方向、手动姿势或站立订单；松开即撤销。`manualBlockArmed`/`manualBlockInject` 不成立，`chooseBlock_hook` 仅在本次 RMB 请求仍有效时转发原版选择器。v111 起 `attackingYou_hook` 始终写入独立的 `IncomingAttackSlot` 来袭事件流；自动格挡读取该事件流，武器闪光仅按 `WeaponGlint` 决定是否渲染。`combatGo_hook` 只在“RMB 正在按住且该攻击仍处于 STARTUP/CHOP”时放行原版防御决策，其他帧仍压制自动防御、攻击、追击和连击。v102 中 RMB 仍不承载交互语义。

v98 进一步删除自动模式按压时的防守订单和主动 combat-idle 写入：RMB 只建立权限并由格挡锁位门禁止移动。`manualAutoBlockInject` 仅在真实来袭且原版 chooser 返回具体 technique 后开启战斗动画层并进入 BLOCK，因此无来袭按压保持原姿势。手动模式仍保留即时姿势和 DEFENSIVE_COMBAT 订单。

v96 试图查询原版选择出的普通 BLOCK technique 动画实例，但实机中成功 `HIT_SWORD` 的全部样本都返回 `clip_progress=-1.00`，不能建立可靠片尾边界。v97 不再猜测片段时长，也不再每帧强行恢复待机：每次 RMB 按压只准备一次防御层；请求存活期间 `charMovUpdate_hook` 让原版更新推进动画，随后钳住 X/Z 位置并清横向速度。该静止所有权覆盖手动与自动格挡。

v116 删除铁风主动闪避子系统，不再存在闪避按键、输入窗口、状态、招式副本、动画注入、位移、冷却、技能加成或自由闪避诊断。原版边界仍保留：敌人和其他角色照常由 Kenshi 产生 `HIT_MISSED`；自动格挡已获 RMB 授权后，原版 `chooseBlock` 若返回 `isDodge` technique，铁风只调用经字节校验的原生防御播放入口完成该次原版选择。手动方向格挡的招式池明确排除 `isDodge`。这条兼容桥不是玩家主动闪避系统，后续重设计不得在其上堆输入状态。

### 中立受害者反击时序

中立敌对使用“准备—确认—提交”的单向事务。LMB-down 只保存中立来源；直接 AttackState 初始化时，`attackingYou_hook` 仅对这名中立来源目标延后原版的提前通知，避免一次挥空先建立局部敌对、而 campaign/阵营层仍未提交。敌对目标的原版通知、玩家作为受害者的来袭通知和自动格挡窗口完全不受影响。

原版接触 verdict 仍负责激活必要的受害者 CombatClass，但不产生世界后果；只有受害者权威 `_getHit` 证明真实伤害链已接受命中后，`manualAttackProvokeVictim` 才一次性调用 campaign 与 awareness，并把受害者自身的 `attackTarget(player)` 排到本帧 `s_mainLoopOrig` 返回后。缓存只保存受害者／攻击者身份，使用前必须同时通过当前更新列表、队伍、死亡与 KO 检查；插件不写 `ST_TEMPORARY_ENEMY`，不向附近盟友强塞攻击任务。特殊身份如何反应、附近盟友是否支援，继续由完整事件提交后的原版关系、感知与 AI 决定。

攻击目标的“仍可战斗”不能使用 `isUnconcious()` 单独判断：Kenshi 对断腿爬行 `PS_CRIPPLED` 和装死 `PS_PLAYING_DEAD` 也可能返回 true。`manualVAttackTargetLive` 明确排除死亡与 `PS_KO`，保留断腿、装死和有意识倒地目标；它只决定能否作为原版 AttackState 载体，不改变攻击区或伤害。

### 受击反应所有权

`iHitYouAreYouHit_hook` 只有在原生判定既非格挡也非闪避时结束**铁风自己的**自由攻击／格挡动画层、清 `animationOverride` 并丢弃已缓存输入；若 V 自由动作曾清掉 `combatModeActive`，这里在返回原版调用者前恢复它，确保随后仍进入 `_getHit`。`getHit_hook` 读取原版 `_getHit(..., stumble)` 的真实 `stumble` 参数：`false` 不增加任何铁风硬直；`true` 放行原版 `STARTUP → STUMBLE` 入场，并由排队的或实际的 STUMBLE/REACTION_BLOCK 状态接管身体。AI 攻击 STARTUP/CHOP 的拦截仅在没有原版受击反应时执行，不能覆盖 `nextMove=STUMBLE`。`computeHoldDecision`、WASD 注入、自由动作重播和手动输入注入统一查询该原版状态，状态结束当帧恢复，不存在固定反应时长。

v109 处理第三方战斗控制器改变调度的兼容边界。正常情况下 `combatGo_hook` 记录本帧已经获得原版 `go()`；若 `_getHit` 已明确留下 `STARTUP + nextMove=STUMBLE/REACTION_BLOCK`，原版世界循环返回后该角色却从未得到本帧 `go()`，`manualHitReactionLifecycleTick` 才补调用一次原版实现。该兜底按主循环帧去重，限定当前 V 锚点、实时更新列表和精确受害者入场形状；普通 DECISION、攻击 CHOP、格挡、移动和已经进入反应的状态均不进入。补调用只推动原版状态机，反应是否进入以及何时结束仍由原版决定。

V 模式退出是铁风输入所有权的硬边界：离开 V 时清除 `s_hitReactionPending` 与原子输入锁，但不写原版 combat state；重新进入时读取角色当前 `STUMBLE/REACTION_BLOCK`，仍在原版反应中则继续封锁，否则以干净状态接受输入。这样 KO 自动退出后不会把上一场反应锁带入新的 V 会话，也不会用固定时间提前解除仍在播放的原版动作。

v110 将这个所有权边界以只读 C ABI `IronwindCombat_IsManualControlActive(Character*)` 暴露给同进程插件。返回值只在传入角色正是当前 V 锚点、非读档保护且非硬关闭时为真；接口不修改引擎对象，也不接收回调。第三方任务的保存与恢复仍由任务所有者负责，铁风不猜测任务类型。

### 主动闪避边界

v116 不存在铁风主动闪避状态。Shift+方向是普通移动，Q 没有闪避角色；主动闪避相关输入、方向、资格、动画和位移必须等重新设计后作为独立系统引入。原版攻击结果中的 `HIT_MISSED` 与自动防御 chooser 自选的 dodge 属于 Kenshi 原生反应，仍按上一节的兼容边界运行。

## 8. UI 与交互

- 准星模式可显示目标姓名、两行生命面板和来袭武器闪光。面板上排为血／头／胸／腹，下排为左臂／右臂／左腿／右腿；不生成总 HP。v119 将单条视觉尺寸改为 `38×5 px`，缩小标签留白、列隙和行距，使条更长更细但两行更紧凑。标准身体使用稳定 FCS `stringID`（28～32、100、101 的 gamedata.quack 记录），自定义种族四肢以 `PART_ARM/PART_LEG + side` 回退。`focusHealthStates` 以 `_maxHealth * age * HPMult` 为原始容量，并按 `flesh - fleshStun`、`flesh`、`flesh + bandaging`、扣除 `wearDamage` 的有效上限和 `juryRigging` 计算原版医疗分段；`isRobotic()` 逐部位决定健康段使用原版绿皮肤还是骨人蓝皮肤。渲染直接复用 `Kenshi_LifeBarSkin/Robot/Stunned/Hurt/Healed/Crushed/Splint`，不存在的部位不绘制。
- 四个方向箭头位于准星上下左右并朝向中心，但任一时刻只显示当前选中的一个；四个方向都允许出现。四向由同一程序几何旋转生成独立 RGBA 纹理并通过 `ImageBox::setImageTexture()` 绑定；保持 64×64 外框、箭头长度和约 3 px 笔画，v73 的统一中心半径为 258 px。`s_dirMoveAvailable == 1` 的所选方向为绿色，其余和未知方向为白色；格挡输入不改变箭头颜色。
- 真正命中时，四个斜线文本控件紧贴准星四个对角形成断开的红色 X，字形不放大，并在 1000 ms 内淡出；显示时间在加载清理中归零。
- v102 每帧从 40 单位内的已加载角色筛选交互候选，距离优先、屏幕偏移次之；活着的敌对角色不抢交互提示，昏迷／死亡角色仍可选。候选显示黄色 `交互：名字`，有玩家对话包时仍显示对话气泡。
- `E` 只登记一次请求；游戏线程重新验证候选与动作状态后调用原版 `ContextMenu::showContextMenu`。菜单可见期间临时释放光标并维持原版 RMB 菜单的持有语义，但选项点击完全交给 MyGUI／原版回调，菜单关闭即恢复战斗输入，不存在常驻交互模式。
- 红色断开 X 只由真实 `_getHit` 触发。v84 使用 26 px 红色斜线和同色系 1 px 阴影增加笔画重量，控件 alpha 最高 0.82，并在 1000 ms 内衰减；位置与四段布局不变。
- 插件不直接调用 `ContextMenu::optionSelected`；攻击、格挡、受击或其他 UI 占用鼠标时拒绝开菜单，真实受击会关闭由 E 打开的菜单。
- 空格暂停与 UI 菜单必须分开：世界暂停不进入 `dcUiWantsCursor`，所以 OTS／FP 继续读取鼠标与滚轮；`controlEnabled=false` 的真实菜单仍停止相机并释放光标。
- 默认保持准星：中立黄、友方绿、空处/红名白。只有确认存在玩家对话包时才显示对话气泡，不显示原版黄/绿箭头。
- V 模式库存打开时保留控制与镜头距离，自动释放鼠标；未按 WASD 时原地保持。
- v120 将游戏内设置页收束为 11 个铁风按键和 13 个常用设置；原版移动档位不重复展示。仍有运行价值的攻击缓冲、键盘格挡、特殊种族和镜头裁剪参数只保留在 INI，页面“恢复默认”不覆盖这些个人 DIY 值。
- `Settings.zh-CN.tsv` 只翻译显示文本。INI 节名、键名和代码查找用的英文原文不得改名。

## 9. 加载与生命周期安全

真实读档、导入、新游戏与普通区块流式加载不同：

- 区块流式加载中，只要控制对象仍在更新列表，可保留控制意图。
- 真实读档立即放弃旧运行时指针，但保留用户想继续直接控制的意图。
- 世界销毁执行硬关闭，释放镜头、UI和战斗缓存。
- 读档结束后，玩家、选中角色、movement 和空旧锚点必须连续稳定 60 帧才能重获。
- `clearAllState` 必须覆盖新增的跨帧指针、时间戳、动作和 UI 状态。
- v103 起，攻击瞄准、攻击缓存、原版攻击所有权、受击锁和交互层分别通过 `manualAttackClearAimInput`、`manualAttackClearQueuedInput`、`manualAttackClearActiveRuntime`、`manualHitReactionClearRuntime` 与 `manualInteractionClearRuntime` 收口。清理函数不得调用引擎对象，因此可以安全用于读档、V 退出和指针失效路径。
- `manualRuntimeInvariantTick` 只读检查互斥状态，不自动“修复”。出现 `invariant_violation` 必须保留现场并分析，不能用猜测式清状态掩盖所有权冲突。

## 10. 核心不变量

1. 玩家队伍永不成为铁风攻击的有效命中对象。
2. 一个输入只产生一次攻击或格挡提交；当前没有玩家主动闪避输入。
3. 同时只有一个自由动作拥有战斗动画层。
4. 真实受击硬直和招架反应不能被清理代码提前结束。
5. OTS、第一人称和库存特写不会同时驱动同一相机。
6. 输入线程不调用引擎对象。
7. 加载保护期间不解引用旧角色或移动指针。
8. 插件钩子失败时记录并局部降级，不得静默假装功能可用。
9. 正常动作时长取实际动画时间轴；1.5 秒保护只用于自由动画完全冻结或原版攻击任务尚未入场的失效保护，不是动作总时长上限；受击反应没有铁风固定时长。
10. 日志路径触发不等于游戏画面验证。

## 11. 代码导航

运行时目前仍是单翻译单元 `WASDCombatPlugin.cpp`。不要维护易漂移的行号；用以下稳定符号搜索入口：

| 领域 | 主要入口或所有者 |
| --- | --- |
| 初始化与卸载 | `RE_KenshiPluginInit`、`RE_KenshiPluginShutdown`、Hook 安装函数 |
| 主循环与生命周期 | `mainLoop_hook`、`clearAllState`、加载保护相关 helper |
| 原始输入 | `PollThread`、`fpDInputPollThread`、每帧输入快照 |
| 移动 | `charMovUpdate_hook`、WASD 意图与 hold helper |
| 攻击 | `manualAttack*`、`combatGo_hook`、`setCombatState_hook` |
| 格挡与来袭 | `manualBlock*`、`attackingYou_hook`、`chooseBlock_hook` |
| 命中与受击 | `iHitYouAreYouHit_hook`、`getHit_hook`、`manualHitReaction*` |
| 镜头 | `cameraUpdate_hook`、OTS／第一人称状态 helper |
| UI、准星、血条 | `playerControl_hook`、`focus*`、方向与命中标记 helper |
| 设置与本地化 | 设置窗口 Hook、INI 读取、`Settings.zh-CN.tsv` 查找 |

外围文件职责：

- `runtime/RE_Kenshi.json` 只负责加载兼容 DLL 名。
- `runtime/IronwindCombat.mod` 是启动器占位；玩法不在 FCS 中。
- `runtime/materials/deferred/character.hlsl` 只承载第一人称角色隐藏支持。
- `tools/` 只负责检查、日志、构建编排和证据；不得承载运行时规则。

新增 Hook 时必须在本节登记领域和所有权，并同步 `TESTING.md`；仅移动代码而不改变边界时无需复制一份函数清单。
