# 变更记录

铁风版本号独立于上游。当前铁风版本为 **v134**，历史代码基础来自 Direct Control for Kenshi **v1.5.0**。下列“已编译”不等同于“已完成游戏内验证”；当前问题见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md)，构建和部署事实由生成报告保存。

## 未发版工程维护（不改运行时）

- 建立互斥双通道：Steam Workshop `3824431439`／`IronwindCombat.mod` 作为只读正式版，本地 `IronwindCombatDev.mod` 作为开发版；开发设置页带 `[DEV]` 标识。新增 `UseDev`／`UseStable` 安全切换和 `CHANNEL.json`，`DevFinish` 只部署开发目录，`Finish` 只生成正式发布包，不再覆盖订阅版或改变当前通道；未修改 DLL、INI、Hook 或战斗逻辑。
- 正式采用“铁风执刃 / Ironwind: Hands-On Combat”为项目名称，新增 `NOTICE.md` 区分当前项目归属、上游著作权和兼容旧名；GPL-3.0 与上游署名保持不变。
- 将 `CODE_STRUCTURE.md`、`NOTES.md`、`PROGRESS.md` 收束为兼容入口，结构导航并入架构文档；`KNOWN_ISSUES.md` 只保留当前事项，停止在多份文档手抄同一状态与哈希。
- 增加日常 `DevFinish`、只读 `Housekeeping` 和可跳过永久符号归档的部署选项，避免每次开发验证都产生完整发布归档；未修改 DLL、INI、Hook 或战斗逻辑。

## v134

- 将 v132/v133 分散的门、储具和地面物品直接交互收束到一个能力路由器 `manualInteractionTryDirectWorldObject`。统一规则是：人物和确实需要选择的特殊对象走原版菜单；其余非人物世界对象按自己暴露的原版主要能力直接执行，不再按“储具、床、炮台……”逐个扩展物件名称表。
- 保留实机已确认的普通门开／关与建筑 `Inventory` 直开路径；地面 Item 继续提交原版默认右键任务。新增通用建筑默认任务路径：床、椅、炮台、工作台、机器及 Mod 设施只要暴露 `getDefaultTask()`，就交给角色自己的 `playerMoveOrderDefault` 处理任务映射、距离和寻路。
- 所有需要角色移动的直接任务先经过同一 `manualInteractionYieldToNativeOrder`，释放 V 模式 WASD 停位但不退出 V；锁门／破门仍回退菜单。v133 本次日志已确认库存直开 7 次、普通门直开 1 次；物品与通用设施路径尚待 v134 实机验证。

## v133

- 根据 v132 实机日志修正储具能力判断：测试储具使用原版 `LOOT_TARGET`，但不是 `StorageBuilding` 子类，故 `getFunctionStuff()` 永远没有进入直接库存路径。现改以 `Building::getInventory()` 的真实虚函数结果判断容器，并继续调用原版 `showInventoryBuilding`；已确认正常的门路径不改。
- 删除没有在失败场景返回任何候选的 `ITEM/WEAPON/ARMOUR` 三套局部球形查询。隐藏鼠标时改用当前镜头中心射线和 Kenshi 原版 `UtilityT::rayTrace` 获取实际物理对象；只接受 30 单位内仍在地面、可见、未禁用且未进库存的任意 Item 子类。
- 地面物品按 E 不再弹冗余单选菜单，而是调用受控角色自己的 `playerMoveOrderDefault`。插件不调用 `giveItem`、不直接搬进库存；PICKUP／偷窃语义、容量、犯罪、移动、声音和失败提示继续由原版默认任务决定。新增 `interact_direct_item` 与带 `ray_type` 的失败诊断。本版尚待游戏内验证。

## v132

- 重做单一用途建筑的 E 体验。普通未锁且未损坏的门根据自身原版默认任务直接调用 `openDoor/closeDoor`，一次 E 完成开／关，不再先暂停、弹出两项菜单，再让关门任务撞上 V 模式旧门抑制。
- 真正储具通过 `Building::getFunctionStuff()` 识别，一次 E 直接调用 Kenshi 的 `showInventoryBuilding` 打开原版建筑库存；不创建铁风上下文菜单或暂停。物品转移、所有权与偷窃仍由原版 Inventory 处理。
- 候选不再因为 `isFurniture()` 一个标志就接受所有家具。装饰家具没有原版光标、默认任务或储具功能时不显示 E，消除日志中连续的 `interact_menu_open_failed reason=no_native_options`。锁门、破门、人物和真正多选设施继续回退原版菜单。

## v131

- 手动上格挡匹配原版 `CUT_DOWNWARD`／`CUT_REAR_DOWNWARD` 下劈时，取消额外的角色朝向夹角判定。只要本次 RMB 请求仍有效、方向匹配、玩家没有正在挥刀且不在原版受击反应中，就直接返回原版 `HIT_SWORD`；格挡姿势是否完全抬起仍不是隐藏时间门。
- 这项修复针对实机中“同一敌人、同一 `bigchopv2`、同一上格挡偶尔成功偶尔受伤”的证据：失败前 `combat_target_lost`，而旧判定读取了可能仍保存绕圈切向的 `CharMovement::direction`。左右及下方格挡暂时保留既有前方夹角边界，避免在没有对应实机证据时扩大规则。
- `directional_guard_block` 新增 `overhead_arc_bypass=0/1`，动态日志报告另列玩家手动格挡数量，便于把玩家防御与“敌人格挡玩家攻击”区分开。
- 已通过静态回归门、Visual C++ 2010 x64 编译、开发通道部署和三处 DLL 一致性验证；DLL SHA256 为 `025191A4C422A348BA9C4309131ACF38AA033E3D786BF5DA7DF12E65B07D4AB8`，PDB SHA256 为 `9937281BDCD617FE2775066F4ACEEBCAE3BBD9630A6027AC27120B85D4B347CA`。游戏内结果仍待 v131 实测。

## v130

- 再次提高两行八部位面板可读性：标签字体由 12 px 增至 14 px，血条由 `50×8 px` 增至 `56×10 px`，标签框同步增至 29 px。沿用 v129 的紧凑规则，标签与条以及四列之间仍然没有额外像素间隔；面板总宽为 340 px。
- 本次只改 MyGUI 尺寸与坐标，不改目标选择、八部位真实数据、原版伤势颜色或任何战斗逻辑。身体骨骼分布方案仍未启用。

## v129

- 按实机观感继续压缩两行八部位面板，但不再改变可读元素大小：字体保持 12 px、单条保持 `50×8 px`。标签盒由 27 px 收至 25 px，标签与条之间的 1 px 间隔和四列之间的 1 px 列隙全部取消，面板总宽由 315 px 收至 300 px；整体同时向人物头部靠近 2 px。
- 本次只改 MyGUI 坐标常量，不改部位数据、原版伤势颜色、目标选择或战斗。把血条分散贴到身体部位的方案保留为后续设计议题，不在未解决骨骼兼容、倒地姿势、遮挡和画面抖动前混入稳定面板。

## v128

- 提高准星观察面板的可读性：标签字体由 9 px 增至 12 px，中文标签宽度增至 27 px，单条由 `38×5 px` 增至 `50×8 px`；单元格取消空余内边距、列隙收至 1 px，两行间距随内容同步调整，因此文字和伤势分段更大但排列仍紧凑。真实八部位数据、原版颜色皮肤、目标选择与战斗逻辑不变。
- 修复越肩／第一人称隐藏原版鼠标时，准星对着地面物品按 E 没有候选的问题。日志证明失败场景从未产生 `type=4` 候选，问题发生在菜单之前。现在保留原版 `mouseRightTarget` 精确结果优先，同时局部查询 30 单位内的普通物品、地面武器和护甲；只有仍在地面、可见、未禁用、未进入库存且准星覆盖投影范围的对象才显示 E。
- 物品操作仍完全交给对象自己的原版上下文菜单：铁风不直接调用 `giveItem`、不绕过容量／偷窃／犯罪判定，也不生成拾取任务。新增静态回归门保护三类物品查询和面板尺寸。本版本完成静态检查和开发部署后仍需游戏内验证物品菜单选项是否能完成原版拾取，以及不同 UI 缩放下的新面板画面。

## v127

- 修正跑步中按住格挡后镜头停住、可视人物却继续向前跑的问题。旧实现先运行原版移动更新，再只倒带 `CharMovement::pos.x/z`，把镜头使用的逻辑锚点与已经推进的可视根节点拆开；现在格挡直接把 WASD 当作未按下，走统一的 `halt + MOVE_DIRECTION + 零速度` 静止入口，并禁止格挡路径事后单独回写逻辑坐标。格挡动作仍由原版时间轴推进，松开右键立即恢复移动。
- 修正受击时持续按住方向格挡必须松开重按的问题。原版 `STUMBLE/REACTION_BLOCK` 期间攻击输入仍丢弃、格挡也不能提前生效；只保存物理上仍按住的 RMB 及首次选定方向，松开即取消。原版反应结束的第一游戏帧才建立新格挡请求并复用该方向，不使用固定僵直时长，也不会补发已经松开的旧点击。
- 增加上述两条所有权的静态回归门和日志事件：`reaction_guard_hold_preserved`、`reaction_guard_hold_cancelled`、`reaction_guard_rearmed`。本版本尚待游戏内画面与日志确认。

## v126

- v125 实机确认准星 E 提示、原版交互菜单和切磋模组兼容的整体体验稳定，保留该交互架构。
- 修正 F 进入第一人称后滚轮不能返回越肩：第一人称统一作为 V 镜头缩放范围的近端，不再按进入方式区分；F 仍可独立双向切换。
- 最新日志中跑步格挡／受击交界出现一次 `zoom=62` 却被 `normal_y=1` 的近端碰撞瞬间拉到 `dist=4.5`、下一秒恢复 62。相机只忽略最小镜头泡内部、法线朝上的这种不可表示接触；真实墙体与更远的坡面继续避让。
- 修正准星明确覆盖人物却降级为无伤害空挥：五次失败均为 `attack_pick mode=none`，人物最近距离 28.6～34.5，但精确身体投影被 23 单位武器长度提前过滤。现在 AimRange 内的精确覆盖只提供原版 AttackState 目标，不再替原版判断攻击距离；近似中立窄角候选仍受武器选择距离限制，伤害、格挡和闪避不变。

## v125

- v124 实机日志确认 `interaction_hint_created`、原版菜单 `visible=1 orders=3`、铁风暂停和关闭恢复链均已运行；问题不在菜单。日志同时证明候选算法不符合画面：曾选择 38.9 单位外人物，且 `miss_px` 多次达到 245～261；E 按下时二次选取还会从提示目标切换到另一个距离不同的人。
- 删除角色交互自身的宽松投影矩形和距离优先评分，直接复用统一观察面板已经验证过的严格“准星覆盖投影身体”结果，并把人物／建筑最大候选距离由 40 收到 30。活着的敌人继续排除；KO／尸体仍可交互。同屏侧边近人不再抢目标。
- 建筑改为准星覆盖投影 AABB 才能入选，只有 18 px 视觉留白和最多 24 px 外沿容差；地面物品只接受原版已精确识别且位于准星 45 px 内的对象。屏幕偏差优先、世界距离只作同点次序。若人物或物品已经命中，不再执行当帧建筑空间查询。
- E 请求直接消费同一主循环刚显示提示的 `hand`，不在按键时重新挑目标。提示从可能越过屏幕顶部的无底色头顶文字，改为胸口／对象中心右侧的 `32×28 px` 深色半透明底和 20 px 黄色 `E`，并增加 `interaction_hint_visible/hidden` 坐标诊断。
- 已通过静态检查、Visual C++ 2010 x64 编译、开发通道安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `58A2AEF00140ABF61364FE433157804D30E154C59F7DC8BE8FCD98101AA4C4A5`，PDB SHA256 为 `911DF3E6AD5A4B59CA5E98DA08322D903BF17C1EBD798EF398AE3838F46EAF0F`。个人 INI 保留；提示画面、严格准星归属和建筑 AABB 仍待 v125 游戏内确认。

## v124

- 重做 V 模式近距交互层。候选从仅角色扩展到非敌对人物、KO／尸体、门、家具和暴露原版鼠标光标／默认任务的建筑；Kenshi 已精确选中的近距地面物品也可进入。目标身份使用可失效的 `hand` 而不是长期裸指针，建筑空间查询以 75 ms 节流，按下 E 时重新即时选取；攻击、格挡、锁敌和观察面板仍是独立路径。
- 删除顶部 `交互：名字`，改为附着人物头部或建筑 AABB 中心、随世界目标移动的黄色 `E`。移除已经无调用者的角色专用交互字段和翻译；活着的敌人继续不抢交互提示，无原版菜单项的候选不会被铁风伪造动作。
- 修正原版右键菜单的输入相位：世界线程只登记请求，`PlayerInterface::playerControl` 在原版更新前同时设置 `ContextMenu::rightMouseActivated` 与 RMB 持有态，再打开原版菜单，避免旧实现下一帧观察到释放、约 19 ms 内关闭并执行默认项。菜单成功保留后才由铁风暂停世界；选项仍完全由原版 MyGUI 回调执行，未调用 `ContextMenuGUI::optionSelected`。
- 增加暂停所有权。菜单关闭后只解除这次 E 创建的暂停；若选项进入对话、库存、地图或角色页，则把暂停交回原版 UI。V 模式、近战状态机和相机模式不切换；受击、再次按 E 或上下文失效只关闭本次菜单并走同一恢复边界。静态检查新增世界目标、原版菜单相位、暂停生命周期和已删除顶部提示的回归门。
- 已通过静态检查、Visual C++ 2010 x64 编译、开发通道安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `2A154EA005E0FC51815464424E41452D7BB0B55E368947C9232CE912813BB93B`，PDB SHA256 为 `552B8AAFADFE8B69BCCF04A699C2AB72DB6C78321DCAD7B81869A63A8AB23E54`。个人 INI 保留；人物、门、设施、地面物品、菜单停留、原版选项和暂停恢复仍待 v124 游戏内画面验证。

## v123

- 将敌对、中立、友军和玩家队友统一为一套只读观察逻辑：只有准星实际覆盖已加载 NPC 的投影身体时，才显示该角色的姓名与两行八部位面板；重叠时取最近者，移开即隐藏。删除敌人侧原有的锁定目标、AI 当前目标与威胁目标显示回退，因此界面显示谁只由玩家实际观察决定。
- 姓名按关系显示敌对红、友方绿、中立黄；八个医疗条继续只使用 Kenshi 原版伤势皮肤和真实部位数据。观察路径不写锁敌、E 交互候选、攻击目标、CombatClass、攻击意图或阵营关系。
- 删除已经重复的 `ShowFriendlyHealth` 配置、设置行和翻译，统一由“显示 NPC 名称与生命状态”控制全部关系；普通设置页收束为 11 项，并增加静态回归门，禁止关系专用观察分支重新进入。
- 已通过静态检查、Visual C++ 2010 x64 编译、开发通道安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `9D990B09F057DB577FB40EBE695BBF4EBF63CCEAA66668B660E3BD6065AC498F`，PDB SHA256 为 `2E103742FA942EF79B6F2874329199CBB2C4831334B75D9CA99A9AB61F7064DF`。个人 INI 保留其他选择并移除已失效的 `ShowFriendlyHealth`；准星身体覆盖精度、关系颜色和倒地角色显示仍待游戏内画面确认。

## v122

- 新增并默认开启可关闭的“显示友军与队友生命状态”。仍只复用现有一套姓名＋两行八部位真实医疗面板；准星必须直接覆盖非敌对友军 NPC 或玩家队友的投影身体，才临时显示该角色。它不写锁敌、交互候选、攻击意图或 CombatClass，也不生成全场多人血条。
- 明确自动格挡的出厂默认与设置页重置默认均为关闭，初始使用手动方向格挡。开发部署继续保留已有个人 INI，因此不会借版本升级强制改掉玩家主动保存的自动／手动选择。
- 已通过静态检查、Visual C++ 2010 x64 编译、开发通道安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `5FEEDDC1A0C7D744F1B361B80BA62A520050A79A0391F3610D4D0003D00E867D`，PDB SHA256 为 `5ABA0C6B1BA45FB23ECEAD7FBC333535B8D51E994388041C13A4A08534335BE7`。个人 INI 保留并只补入新观察开关；准星覆盖精度、友军／队友关系分类和面板切换仍待游戏内画面确认。

## v121

- 完整删除铁风自制的敌人手部／武器发光：移除软光纹理、MyGUI widget、骨骼投影、亮度动画、防御颜色分级、创建日志、设置页行、默认 INI 项和中文文案。敌人来袭不再由铁风叠加白／橙／红光。
- 保留 `attackingYou_hook` 与 `IncomingAttackSlot`，因为按住右键的自动格挡仍需知道哪次原版攻击正在来袭；事件只做状态跟踪与到期清理，不再有视觉消费者。impact 诊断改名为 `incoming_attack_impact`，并删除只服务颜色分级的攻击分类计算。
- 不改原版 `HIT_SWORD` 返回、声音、格挡经验或碰撞火花。若实机仍在武器接触瞬间出现火花，应先归因于 Kenshi 原版格挡结算，而不是重新拦截原版特效链。
- 已通过静态检查、Visual C++ 2010 x64 编译、开发通道安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `CC754CD19004010594663239D6E4946C8A935263F0BEAE9C3FAF33A54D02462C`，PDB SHA256 为 `F4965AC89134537C26C82BE2EF4F5073F36A8C730F96D5F16338DF1153AFC890`。v120 开发 DLL 已备份，个人 INI 只删除三个已失效发光键，其他设置保留；游戏内仍需确认敌人起手无自制发光、自动格挡不受影响以及原版碰撞火花边界。

## v120

- 手动方向格挡改为单一空间规则：有效 RMB 请求仍按住、来袭位于可防御前方且方向匹配，就从按压接受时立即具备原版 `HIT_SWORD` 资格；可见姿势是否完全抬起不再是隐藏时间门。自动格挡继续只授权原版 AI 选向并允许失败。
- 完整删除铁风额外的 `PerfectBlockWindowMs`、`PerfectBlockRewardMs`、连击跟随窗口、奖励冷却状态、`perfect_block`／`block_press_reward` 日志和玩家武器蓝色奖励光。声音、火花与格挡经验仍由原版 `HIT_SWORD` 调用方执行。
- 将游戏内设置页从 58 个可调项收束为 11 个铁风按键和 13 个常用设置，保留用户指定的准星目标夹角、越肩高度及第一人称眼位高低／前后。原版移动档位不再重复展示；其余仍有效的攻击缓冲、键盘模式、裁剪和特殊种族参数保留为 INI-only DIY 项。“恢复默认”只影响本页可见设置。
- 删除个人安装 INI 中已经无代码读取的旧 `AttackApproachMs`，并增加静态回归门，防止完美格挡计时状态或过量普通设置重新进入。已通过静态检查、Visual C++ 2010 x64 编译、安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `52F13C3B40F3D6EC8C5C073B377F0C91B824CC1F5323277417947B11A8D0D07F`，PDB SHA256 为 `75112833BF858A399A35544F56B733F3BB8ECE550E67804AFAB45E457DA9B876`。v119 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v120-20261022-142234.dll`，并生成 `package/IronwindCombat-actions-v120.zip`；方向格挡和设置页画面仍待实机确认。

## v119

- 将焦点目标的八项两行生命面板由 `22×6 px` 短色块改为 `38×5 px` 细长条；标签宽度由 20 缩至 18 px、标签条间距由 2 缩至 1 px、列隙由 3 缩至 2 px、行距由 12 缩至 10 px。条长增加约 73%，两行总高度缩小，仍保持血／头／胸／腹与四肢两行语义。
- 本次只改 MyGUI 布局常量，不改八个真实数据槽、原版七层皮肤、伤势端点、目标选择或战斗逻辑。已通过静态检查、Visual C++ 2010 x64 编译、安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `55BE20D5C3C78A55D6CB706A4B6AE1C251E6D4EF1C1B372022A4682BFA91065D`，PDB SHA256 为 `C41DA5145854D46474A4C23E5F1E0C28A99FDFD6F9EA7CDFF8B04E54B56DC4CA`。个人 INI 保留，v118 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v119-20261022-140041.dll`，并生成 `package/IronwindCombat-actions-v119.zip`；画面仍待实机确认。

## v118

- 确认本机原版 `controls.cfg` 中 `Z=speed_1`、`X=speed_3`，分别是 1×／5×游戏速度。删除已无运行入口的 `ManualAttack=Z`、`KR_MANUAL`，并删除只会在原生命令注册失败时复活的 `SpeedCycle=X`、`KR_SPEED`、`s_xPressed` 和旧循环处理器。
- 保留原版 `cycle_run_speed` 作为角色步行／慢跑／奔跑的唯一入口；铁风只在该原版事件完成后同步受控角色，不再主动调用 `speedNext`。Z/X 在 V 内外都完全由 Kenshi 原版处理。
- 增加静态回归门，禁止上述旧配置和符号重新进入。已通过静态检查、Visual C++ 2010 x64 编译、安全部署及构建／暂存／安装一致性验证；DLL SHA256 为 `BBC8D10118FE7FDB179F0E4BCF4383829A663C1C5936760F6E2F99F40AA7EBC9`，PDB SHA256 为 `2025BBB464C03952014296BD12FA16BA9A5099E77493DAF26971C502B36E0278`。个人 INI 保留，v117 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v118-20261022-135414.dll`，并生成 `package/IronwindCombat-actions-v118.zip`；游戏内 Z/X 回归尚未进行。

## v117

- 审计上游 Ctrl 第三人称动作镜头：它不是第二个移动／战斗 `ControlMode`，而是在人物仍处于 `MODE_FREE_MOVE` 时翻转 `s_camRotateToggle`，令脱离式 OTS 退出并露出原版镜头，因此产生“两套控制体系”的割裂感。
- 删除 Ctrl 轮询边沿、`s_camRotateToggle` 与 `s_ctrlPrevPoll`。V 模式第三人称现在恒为 OTS；`F` 和滚轮在 OTS／第一人称间切换，`P` 不再使用，V 退出与读档仍完整恢复原版相机。V 模式仅在 F 实际按下时消费冲突的原版 `focus_char` 事件，退出 V 后不影响原版 F。为避免 `F` 同时触发视角和格挡，旧键盘格挡默认迁到 `Q`；右键方向格挡不变。`KeyLayout=3` 只迁移旧默认 P/F 组合，不覆盖自定义键位。UI 光标门改名为 `s_cameraUiOpen`，明确它与旧 Ctrl 开关无关。
- 保留通用 `VK_CONTROL` 键名解析和原版 Ctrl+点击，不吞掉菜单输入，也允许用户把其他命令绑定为 Ctrl 组合。同步操作说明、架构、代码索引、已知问题、测试规范和维护不变量。键位调整后的静态检查与 Visual C++ 2010 x64 编译、安全部署及构建／暂存／安装一致性验证通过；最终 DLL SHA256 为 `0141082E68F26E290BEE5096DDD51136E1A332978D88EB014F8A5CFD7B35E2FF`，PDB SHA256 为 `E2B7D36E157C5DF5B8C33520CCB25B9E4FEF07FE4D3EA98EAC73998A4CA5B2A8`。v116 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v117-20261022-134532.dll`，生成 `package/IronwindCombat-actions-v117.zip`，个人 INI 保留并已按用户要求更新为 F/Q；游戏内回归尚未进行。

## v116

- 完整删除铁风旧玩家主动闪避子系统：Q／Shift+方向输入、配置与设置页、输入窗口、冷却／恢复、方向与状态成员、招式副本、自由动画注入、人工位移、固定技能加成钩子，以及对应日志分组和已知失败规则。Shift+W/A/S/D 恢复为普通移动。
- 谨慎保留 Kenshi 原生闪避语义：敌人与其他角色仍可产生 `HIT_MISSED`；自动格挡已获 RMB 授权后，若原版 `chooseBlock` 自己选择 `isDodge` technique，仍由经字节校验的原生防御播放入口完成。手动方向格挡招式池明确排除 dodge；这条兼容桥不接受玩家闪避输入。
- 同步默认 INI、中文设置 TSV、架构、代码索引、已知问题、测试规范、进度和维护不变量。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和构建／暂存／安装一致性验证；DLL SHA256 为 `2DACEC8281E4BA1C3D87097880A564C10FDC24092FC5D257BAC200C21813DBE6`，PDB SHA256 为 `9F8775FF4F7199DE26FC2A781E1724F4E26896F61599D413E2B1D9FE918D7AFC`。个人 INI 保留，v115 DLL 已备份并生成 `package/IronwindCombat-actions-v116.zip`；游戏内仍需回归 Shift 移动、Q 无旧动作、无旧 dodge 日志、敌人原生闪避和自动格挡。

## v115

- 撤销 v114 的自定义“健康绿／受伤琥珀／危险红”整条阈值配色。原版医疗 UI 并不是按剩余比例给整条换色，而是用多个真实伤型图层共同组成一条。
- 直接复用 `Kenshi_MainPanel.layout` 对应的七套原版皮肤：`LifeBar` 健康绿、`Robot` 骨人／机械蓝、`Stunned` 钝伤灰、`Hurt` 未处理切割红、`Healed` 包扎／修理黄、`Splint` 夹板白和 `Crushed` 机械磨损。每段长度来自 `flesh`、`fleshStun`、`bandaging`、`juryRigging`、`wearDamage`、`age/HPMult` 与 `isRobotic()`；血液按原版红条显示真实剩余量。
- 两行八项、标准部位 FCS ID、自定义种族左右肢体回退和缺失部位隐藏保持不变。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和构建／暂存／安装一致性验证；DLL SHA256 为 `5D36024543F517AB1D18531364B615DFF4351EFD164A72C112D0CA514E2EA2FE`，PDB SHA256 为 `B45EAC6C915C946A7295B593462E73061CCB1249A28AE40C1AA124E42A8C6091`。个人 INI 保留，v114 DLL 已备份并生成 `package/IronwindCombat-actions-v115.zip`；游戏内有机／骨人／机械义肢分层画面仍待验证，闪避体系未改动。

## v114

- 将 v113 的单行四项改为真正的两行八项解剖面板：上排血／头／胸／腹，下排左臂／右臂／左腿／右腿。标签与短条在同一行内并排，整体仍保持小尺寸，不再用一个宽条伪造总生命。
- 每个短条独立读取 Kenshi 原版 `MedicalSystem`。血液显示血红；身体部位按剩余比例显示健康绿、受伤琥珀、危险红。补充标准左臂、右臂、左腿、右腿的 28～31 FCS ID 映射，并以 `PART_ARM/PART_LEG + side` 支持自定义种族；不存在的部位隐藏。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和构建／暂存／安装一致性验证；DLL SHA256 为 `7F41F3CED94162B6EB464EB449D76FE25102D44C009E988E83106420DA141152`，PDB SHA256 为 `057DC3EF8BE434019BB2692DCA091FD3B4C6745945746CB2B330B0B960B28B31`。个人 INI 保留，v113 DLL 已备份并生成 `package/IronwindCombat-actions-v114.zip`；游戏内布局、逐部位变色和气泡避让仍待验证，闪避体系未改动。

## v113

- 根据 v112 实机反馈撤销纵向四条固定部位色：blood 红、head 白、chest/stomach 绿虽然能显示数值，但既占高度又把颜色误读为四种不同含义。
- 新生命面板在姓名下方横排四个 28 px 分段，中文显示“血／头／胸／腹”。颜色只表达剩余比例：≥67% 浅灰、34%～67% 琥珀、<34% 红色；不存在的部位隐藏。没有合成 Kenshi 不存在的总 HP，也不改变原版 MedicalSystem 数值。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；DLL SHA256 为 `9F4CE7A4DB8235FFD105A8A21B7AA0BBD4407117F10E67E740C3C806FDCAA127`，PDB SHA256 为 `68D2615DB33177D19A73BF14F3CC9B0DB64123643E646055C2749A5B8E5DD7E2`。个人 INI 保留，v112 DLL 已备份并生成 `package/IronwindCombat-actions-v113.zip`；游戏内布局、受伤变色、气泡避让仍待验证。

## v112

- v111 部署后的原版数据核查发现，标准身体部位 `stringID` 是数字 FCS 标识，不是 `head/chest/stomach` 英文词：头为 `32-gamedata.quack`、胸为 `101-gamedata.quack`、腹为 `100-gamedata.quack`。v111 的类型兜底只能可靠补头部，胸腹仍可能不显示。
- `focusHealthSlot` 增加上述三个原版 ID 的精确映射，同时保留 `PART_HEAD` 与语义化自定义 ID 兜底。血条仍不读取本地化显示名；缺失部位继续隐藏。v111 的来袭／闪光解耦与死代码删除保持不变。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；DLL SHA256 为 `032E6674116D2441A750A69A39F3E75F28D91126BB601F9DE4AFAC3203C5EEC1`，PDB SHA256 为 `F7309FEFE7D5DD128D2F02C4FDF0A932CC5372D8DC822FCF0323A580BEC0C8A6`。个人 INI 保留，v111 DLL 已备份并生成 `package/IronwindCombat-actions-v112.zip`；游戏内血条和自动格挡回归仍待实测。

## v111

- 修复中文环境下分部血条只有 blood 正常的问题。旧实现用本地化后的 `GameData::name` 匹配英文 `head/chest/stomach`，日志已证明实际名称为“头部／胸部／腹部”；现改用语言无关的 FCS `stringID`，并以原版 `PART_HEAD` 作头部兜底。种族没有某个标准部位时隐藏该行，不再显示伪造的空血条；新增逐部位 `aim_health_part` 诊断。
- 将原生 `attackingYou` 来袭记录从视觉 `GlintSlot` 拆为独立 `IncomingAttackSlot`。自动格挡、来袭有效性和闪光渲染分别读取同一原生事件；`WeaponGlint=false` 只关闭画面，事件仍会过期清理，不能再与自动防御开关互相拥有状态。
- 删除已证明无运行读者的旧 `AttackApproachMs` 配置及设置页文案、攻击重试／重新接近日志字段、未读取的 Shift 闪避边沿字段，以及所有调用均为空操作的旧 HUD 壳。闪避输入、技能判定与现有失败路径均未改变，闪避体系仍留待重新设计。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；只出现既有 Ogre C4819 与 MyGUI C4091 警告。DLL SHA256 为 `9D786F5A5B394AB0EC134FAA5D03C3B59D8945C300D74EC5230321DF3004A964`，PDB SHA256 为 `8A0F91D73884F2F5C9C2EDA2B8B773985C1DE24803BE36C3477BABB138F8BB8F`。个人 INI 保留，v110 DLL 已备份并生成 `package/IronwindCombat-actions-v111.zip`；血条画面、关闭闪光后的自动格挡和完整回归仍待实机验证。

## v110

- 针对切磋中“进入铁风 V 后退出仍不恢复自动战斗”的所有权边界，新增只读 C ABI `IronwindCombat_IsManualControlActive(Character*)`。接口只在指定角色是当前有效 V 锚点时返回真，不修改角色、任务或战斗状态。
- 铁风继续按设计在 V 内抑制自主战斗，也不猜测退出时应恢复哪个第三方任务。任务所有者可记录该接口的 `true → false` 边沿，只恢复自己拥有的高层任务；这避免按帧检测停手、固定延时或通用任务快照。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 哈希一致性验证；DLL SHA256 为 `B258C149EF9BB4079F5B5E4CD4A3537339B7B80C88F981E9D29057ACF04D6074`，PDB SHA256 为 `4ADFBDF80546B6107AD1D4D780E960DC9D3FA0CB1DCA9C78664D56B37CDBBE1C`。个人 INI 保留，v109 DLL 已备份并生成 `package/IronwindCombat-actions-v110.zip`；游戏内切磋边界仍待验证。

## v109

- v108 与 Kenshi Sparring Reborn 1.4.3 的联合日志显示：对方命中后原版 `_getHit(stumble=1)` 已留下 `STARTUP(3) / nextMove=STUMBLE(8)`，铁风也释放了在途攻击，但切磋控制器的双边任务重发与虚拟敌对边界使玩家 CombatClass 后续不再获得 `go()`；反应既未进入也未取消，输入锁遂永久保留。这是第三方耦合触发的铁风容错缺口，与 v108 中立真实命中后敌对事务无关。
- 新增按主循环帧去重的受击入场兜底。只有当前 V 锚点、原版真实 stumble pending、状态正是 STARTUP、nextMove 正是 STUMBLE/REACTION_BLOCK，且本帧正常 `combatGo_hook` 未推进时，才补调用一次原版 `CombatClass::go()`。不使用固定时长、不直接写 STUMBLE、不清锁伪恢复，也不在普通攻击、格挡、移动或 DECISION 状态放行 AI。
- 新增 `hit_reaction_fallback_advance` 和带 `source=engine/main_fallback/main_reconcile` 的反应诊断。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `C6A169DF2627E9E7F0758E4AA7F0C9BBB54E446FFAB7415FFAAF7DBAF5627AC1`，PDB SHA256 为 `3DA915C8F4D97C871BBAFDA55C63CC7B004D609DBE1C8BDE8BF9090459666D01`。个人 INI 保留，v108 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v109-20261022-111458.dll`，并生成 `package/IronwindCombat-actions-v109.zip`；游戏内切磋回归仍待实测。

## v108

- v107 实机日志显示一次中立攻击在 `contacts=0 hits=0` 时，目标已被 AttackState 初始化阶段的原版 `attackingYou` 变为局部敌对并开始反击；由于没有权威 `_getHit`，campaign／阵营事件没有提交，附近盟友自然得不到同一完整攻击事件。这是起手通知与真实命中通知分属两个提交点造成的半状态，不是玩家侧 AI 抑制误伤盟友。
- 将中立敌对收束为单一事务：`attackingYou_hook` 只在玩家直接初始化这一刀的精确同步作用域内，对 LMB-down 记录的同一中立来源目标延后提前通知；敌对目标、玩家受击、武器闪光和自动格挡通知不变。挥空、闪避和被格挡不再提前局部敌对；真实 `_getHit` 继续通过既有 `manualAttackProvokeVictim` 一次性提交 campaign、awareness 和受害者响应。
- 不扫描盟友、不写临时敌对记忆，也不给盟友强塞攻击任务。真实命中后，盟友是否支援仍由 Kenshi 原版的队伍／阵营关系、感知、距离和 AI 决定。新增 `neutral_precontact_notice_deferred` 诊断。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和构建／暂存／安装一致性验证；最终 DLL SHA256 为 `0373F5C19D2A0B74179BE7789227A6230706D8034039DD8E93CA73CD335ADF51`，PDB SHA256 为 `9F0034EA88AD87DC5A6B4850444BA303DA70EA02BA00FC46E21E47204CB66BE7`。个人 INI 保留，v107 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v108-20261022-105250.dll`，并生成 `package/IronwindCombat-actions-v108.zip`；本条不代表游戏内行为已经验证。

## v107

- v106 调停人实机日志证明：4 次完整原版挥刀中有 2 次接触判定返回 `HIT_FLESH`，却全部没有进入受害者 `_getHit`，因此没有伤害、红 X 或 confirmed-hit 反击。退出 V 后原版攻击任务会先激活受害者战斗层，所以表现恢复正常。
- 修补两段断链。其一，LMB-down 记录目标当时是否中立；即使 `initialiseAttack` 返回失败却提前改变关系，对同一目标的后续挥刀仍保留真实来源。其二，仅当原版 verdict 已确认不是格挡／闪避时，若该中立受害者 CombatClass 尚未激活，就开启其原版战斗层，让 `Character::hitByMeleeAttack` 继续自己的伤害、护甲、`_getHit` 和反应流程。
- 不在点击、攻击初始化、挥空或被格挡时调用挑衅；受害者反击仍只能从权威 `_getHit` 的 `source=confirmed_gethit` 触发。新增 `neutral_origin`、`victim_combat`、`restored` 与 `outgoing_hit_victim_combat_restored` 诊断。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `B5662B7EA0757676DFE5AA9967A865E6D17F82B7C2C6D91FCB6AA80D47D5F856`，PDB SHA256 为 `C7C33B0A2C1CC90D82182291732B06032294CDDF7D39FAC92699F3DEF37384F1`。个人 INI 保留，v106 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v107-20261022-104309.dll`，并生成 `package/IronwindCombat-actions-v107.zip`；实机验证仍待进行。

## v106

- 根据用户澄清撤销 v105 的“HUD 1200 ms、输入 800 ms”分离：箭头就是当前真实方向状态，显示什么，下一次攻击／手动格挡就使用什么。
- 最后有效方向统一保持 1200 ms；这不是锁定期，保持期间任何新方向手势都会立即替换旧方向和箭头。例如右划选中后 0.1 秒改划上方，会立刻改为上方。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `38542A433280C344B2A935C07F34E64220BAE2AFD647D0926575292FA0F3504F`，PDB SHA256 为 `B7344A841D9273A7F65D9006A9B4805895151941305A04B7AE1E1FCB80E264AF`。个人 INI 保留，v105 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v106-20261022-102508.dll`，并生成 `package/IronwindCombat-actions-v106.zip`；实机验证仍待进行。

## v105

- 最后选中的四向箭头和顶部方向文字在鼠标停止后由 800 ms 延长到 1200 ms，只增加 400 ms 的视觉停留，避免方向提示刚出现就消失。
- HUD 保持时间与攻击／格挡输入的方向有效期拆开；实际输入仍沿用 800 ms，因此不会因为箭头多显示一会儿而改变下一刀或格挡方向。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `12C3EAB232E87AE3D47322B08518445B451C34ADFD96AA5B12FBEBC679E9BDA7`，PDB SHA256 为 `98B0E0DB7B167E98F391F0AB5832CC85334868585C96828E6CA2DC6EA3B95CB9`。个人 INI 保留，v104 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v105-20261022-102244.dll`，并生成 `package/IronwindCombat-actions-v105.zip`；实机验证仍待进行。

## v104

- 修复暂停和 Esc 菜单仍隐藏、锁定鼠标的严重输入回归。v103 实机在世界停止后持续输出 `ui=0`，说明 v102 为允许暂停时旋转镜头而移除 `ou->isPaused()` 后，当前 Kenshi 构建的 Esc 菜单也没有稳定通过 `CameraClass::controlEnabled` 被识别。
- 恢复暂停优先级：`dcUiWantsCursor()` 在任何引擎暂停下统一释放 FP、OTS 和准星的鼠标捕获；恢复游戏后沿用既有上升沿逻辑只回中一次。此修复不改战斗、攻击、格挡、E 交互或生命周期状态。暂停时转镜头的旧目标让位于可操作且不会锁死的光标边界。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `5D4379697AAB33CBB4A9D63F9B35C9366273003AC7104550C51786D993A28675`，PDB SHA256 为 `C4A7B324AF4389D316389FBFD03DB3E4FFA934CD66763C59CC66AE582CF9AFDF`。个人 INI 保留，v103 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v104-20261022-101118.dll`，并生成 `package/IronwindCombat-actions-v104.zip`；游戏内回归仍待实测。

## v103

- 把攻击瞄准、下一击缓存、在途原版攻击、受击反应和近距 E 交互的清理动作收束到各自生命周期 helper。退出 V、读档清理、受击打断、正常收招和缓存提交改用同一组边界，减少一处新增字段只在部分路径复位的风险；同时补齐全局清理此前遗漏的 `s_attackObservedUnfinished`。
- 新增只读 `manualRuntimeInvariantTick`。它只报告互斥所有权重叠、残留目标／时间戳、无活动攻击却保留初始化或 impact 标志等不可能状态，不自动修改玩法状态；日志分析规则把 `invariant_violation` 归为错误。静态 `Check` 还会验证 helper 存在，并限制核心攻击字段的直接复位只能出现在所属 helper 内。
- 本版的优化终点是“生命周期清理集中、静态所有权约束、只读运行时诊断、文档和发布闭环”。不拆分 1.6 万行物理编译单元，不重写整套状态机，不改变攻击时序、距离、方向、格挡、伤害、镜头或交互规则。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `17494002BFFE8723526CF10E2A62DEEBAA9982F4F61E7CBEE81A53C9C5C739A9`，PDB SHA256 为 `80CE9CB2143C14CE78AA4706B9902C443A0331B5DD3A7A92D911BC1AD7884660`。个人 INI 保留，原安装 DLL 回滚备份为 `backup/IronwindCombat-pre-deploy-v103-20261022-005221.dll`，并生成 `package/IronwindCombat-actions-v103.zip`；游戏内回归仍待实测。

## v102

- 根据用户反馈撤销 v101 常驻交互层。40 单位内、位于中央视野的可交互角色会按距离优先自动选中，并显示 `交互：名字`；活着的敌人不抢提示，昏迷或死亡角色仍可选。
- `E` 改为一次性交互：仅为空闲状态下的当前自动目标打开原版上下文菜单，菜单关闭即恢复战斗。插件只维持原版菜单需要的 RMB 持有状态，不直接执行 `optionSelected`；右键继续始终只负责格挡。
- 修复空格暂停无法转动和缩放 V 镜头。世界暂停不再等同于 UI 鼠标占用，OTS／第一人称仍处理鼠标与滚轮；Esc、库存、对话、地图及上下文菜单仍按原规则释放光标。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `F5EF7B481A42A874DA44B431802431BA24A4D3199143835354B221B0F426029F`，PDB SHA256 为 `9D7A61D0F6C30A6E36703C0E1157B9804577484D17A2A2C10FD3A15AF41121E0`。个人 INI 保留，v101 DLL 备份为 `backup/IronwindCombat-pre-deploy-v102-20261022-002501.dll`，并生成 `package/IronwindCombat-actions-v102.zip`；交互菜单持有和暂停镜头仍待 v102 实机验证。

## v101

- 重做 V 方向战斗的交互所有权。战斗层 RMB 不再以 200 ms 时长复用为交互：无论短按、长按、目标或格挡模式，右键都只负责格挡，因此不会在防御过程中延迟弹出菜单。
- 方向战斗中的 `E` 改为显式切换原版交互层。进入后释放系统光标并停止战斗鼠标拦截，真实 LMB/RMB 直接交给 Kenshi `PlayerInterface::mouseScan/playerControl`；角色、尸体、物品、建筑、门、床、机器、炮台和地面命令均由原版对象扫描、菜单和执行路径处理。再按 `E` 返回战斗层。
- 删除 Character-only 的手工交互执行、直接对话和 `ContextMenu::optionSelected` 注入。战斗准星的角色身体投影只保留为对话气泡提示，不再决定实际交互资格。攻击、格挡、闪避、受击、读档指针丢失或已有 UI 时不能切入；交互中真实受击会自动退出。新增 `interact_mode_on/off/denied` 诊断并更新中文设置说明。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和构建／暂存／安装一致性验证；最终 DLL SHA256 为 `CF3B4DBB5C228E528BD147663B962F38BEEE043FA46195BE0511261310D1AF6A`，PDB SHA256 为 `CDE9FB9F0FCE772CD577451B42B8AD9E2D624A4E0AF63ECB5392BC0B0674A696`。个人 INI 保留，v98 DLL 备份为 `backup/IronwindCombat-pre-deploy-v101-20261022-000647.dll`，并生成 `package/IronwindCombat-actions-v101.zip`；完整原版对象覆盖仍待 v101 实机验证。

## v99

- 修复空手武术完全无法方向攻击。v98 把空手原版 `CombatClass::weaponReach()==0` 当成访问器损坏，在 HUD 和自由攻击调用 `chooseAttack` 之前就退出；v99 只对 `SKILL_UNARMED` 接受该原版零值，并在方向能力、空挥和有目标直接 AttackState 三路继续使用 Kenshi 自己的招式选择。
- 不给武术伪造刀剑距离或强制四向招式。武术技能、负重、伤势与 technique 解锁仍由原版 chooser 决定；敌人命中、格挡、闪避、伤害、护甲和 XP 仍由原版 AttackState 决定。空手攻击中立只允许准星身体覆盖建立意图，不启用依赖武器 reach 的 25° 近似扇区；只有真实 `_getHit` 才触发敌对。
- 新增 `unarmed=1` 与 `weapon_ready unarmed` 诊断，用于区分原版确无该方向动作和插件未运行 chooser。已通过静态检查与 Visual C++ 2010 x64 编译；游戏内行为仍待 v99 实测。

## v98

- v97 实机日志把“右键一按就进入怪姿势”精确对应到 `block_press_accepted → ai_guard_ready_pose neutral_combat_idle → block_hold vanilla_order=applied`。当时没有敌人来袭，姿势完全是插件在按压边沿主动制造的，不是原版 BLOCK。
- 自动格挡改为纯权限请求：按下 RMB 不设置 combat idle、不修改上身 idle，也不应用 DEFENSIVE_COMBAT 站立订单；只记录授权并沿用 v97 的原地锁位。只有真实敌人处于 STARTUP/CHOP、原版 `chooseBlock` 返回具体招式时，才在进入原版 BLOCK 前开启战斗动画层。松开立即撤销授权。手动方向格挡仍保留即时姿势和原版防御订单。
- 新增 `ai_guard_request armed=... pose=unchanged` 诊断；正常自动模式按压不得再出现 `ai_guard_ready_pose` 或 `block_hold vanilla_order=applied`。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `1F3D5372BA372069A5D69CBDA425FC337BEEC9044E515F2F11A979891A072BBB`，PDB SHA256 为 `1EB2CB7C5F1F69FE6C58C22EF20FBACAEAB730ED3B791A6485D6F6622EF94DCB`。个人 INI 保留，v97 DLL 备份为 `backup/IronwindCombat-pre-deploy-v98-20261021-231524.dll`，并生成 `package/IronwindCombat-actions-v98.zip`；自动请求姿势与真实来袭格挡仍待 v98 实机确认。

## v97

- 中立敌对改为“真实命中后触发”。v96 日志中 `attack_victim_provoked source=direct_start` 出现在一次 `contacts=0 hits=0` 的攻击起手，证明只要左键成功初始化原版 AttackState 就会挑衅，即使没有实际攻击到该人物。v97 删除起手挑衅；只有受害者通过原版接触判定并真正进入 `_getHit` 后，才以 `source=confirmed_gethit` 执行 campaign、awareness 与受害者反击任务。挥空、被格挡、只点中目标或动作未成立均不触发。
- 放弃不可靠的“格挡中移动”叠层。v96 所有成功 `HIT_SWORD` 的普通 BLOCK 都无法由 Animation API 找到对应实例（`clip_progress=-1.00`），因此无法在不猜时长的前提下判断何时可让 WASD 覆盖。v97 采用稳定边界：已接受的右键格挡请求存活期间，WASD 输入不产生位移；原版移动更新仍运行以推进格挡／受击动画，之后只消除横向位移。松开右键后移动立即恢复。
- 自动防御待机只在每次已接受的右键按压上准备一次，不再每帧重设 combat idle；避免原版 BLOCK 已回到 DECISION 但视觉片段仍在播放时，被待机姿势反复覆盖。新增 `guard_wasd_blocked` 与 `ai_guard_state_exit` 诊断。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `B5820C4ECBC36FE676D381949DE3ED9E30E67E071904944624C7BA4287076481`，PDB SHA256 为 `15B4738E642566CD1D46B3631C62AD8DCD76E2361A4C531C124E5AEEF9D9F97C`。个人 INI 保留，v96 DLL 备份为 `backup/IronwindCombat-pre-deploy-v97-20261021-220809.dll`，并生成 `package/IronwindCombat-actions-v97.zip`；两项行为仍待 v97 实机确认。

## v96

- v95 实机确认首个中立目标已会立即还手。自动格挡的权限和判定也没有被 WASD 清除：本场按住右键期间前三次来袭均由原版返回 `HIT_SWORD`。剩余异常来自错误的动画结束边界：三次 `ai_block_attempt` 后，原版都在同一帧把 `BLOCK` 切回 `DECISION/WAIT`，v95 因而立即记录 `ai_guard_block_exit`，但实际 Ogre 格挡片段尚未结束，后续移动会覆盖该片段。
- 自动格挡动作所有权改为“具体 technique 正在 BLOCK 初始化，或该 technique 的真实动画实例尚未 `hasEnded`／到达片尾”。原版状态提前回到 DECISION/WAIT 不再清令牌；WASD 会继续暂存到真实片段结束，随后按住右键恢复防御待机和 20% 移动。结束依据来自实际动画时间线，没有增加固定格挡时长，也没有放行玩家攻击、AI 寻路或连击。
- `ai_guard_block_exit` 新增 `clip_progress`，`ai_block_attempt` 的 `played` 改名为 `explicit_play`，避免把普通 BLOCK 由原版状态启动片段时的 `0` 误读为“动作没有播放”。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处 DLL 一致性验证；最终 DLL SHA256 为 `6E707223A9D08D95C291829FB1B8F2AC354EDF2969FD75A758F174C6197AE0C3`，PDB SHA256 为 `82391888A4C4F2069DA925032A2AF3881254083793B5FB492F0BA168B9D6A6F3`。个人 INI 保留，v95 DLL 备份为 `backup/IronwindCombat-pre-deploy-v96-20261021-215244.dll`，并生成 `package/IronwindCombat-actions-v96.zip`；真实片段边界与长按右键 WASD 仍待 v96 实机确认。

## v95

- 修正首个中立受害者有时不接战、攻击第二人才触发敌对的时序竞态。中立攻击仍在原版 AttackState 成功起手时调用 campaign 与 `attackingYou`，但受害者侧 `attackTarget(player)` 改为当前原版主循环返回后再下达；因此阵营／犯罪关系先完成本帧传播，任务不会在同一嵌套调用栈读取旧中立关系。缓存角色每次使用前均验证实时更新列表，读档清理覆盖全部槽位；不写临时敌对记忆，也不恢复玩家侧 focused/unprovoked 任务。
- 自动格挡选出的真实原版 `BLOCK` 现在以“本次具体 technique＋原版 BLOCK 状态”作为动作所有权。来袭闪光在接触时结束后，`combatGo_hook` 仍只让这一个防御片段自然推进到原版状态退出；WASD 在片段内暂存，不再清掉动作或防御状态。没有固定格挡时长，也没有放行 AI 攻击、寻路或连击。
- 按住自动格挡且未处于真实 BLOCK 时，20% WASD 现在保留原版防御请求和普通持械战斗层，不再因移动路径清空 `combatModeActive`。BLOCK／REACTION_BLOCK 退出的精确状态边沿会恢复防御待机；松开右键仍立即撤销请求。新增 `attack_victim_response`、`ai_guard_block_exit` 诊断。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署和构建／暂存／安装一致性验证；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终 DLL SHA256 为 `C3F53282C85A316AB1A25B7EA896B0180BF934B89558EF5C6DD2E444F147F0F3`，PDB SHA256 为 `1ED4AFA3F2628187FA1E1915C31BC445E3BAA006922675B26EEB88996C3AB79B`。个人 INI 保留，v94 DLL 备份为 `backup/IronwindCombat-pre-deploy-v95-20261021-214137.dll`，并生成 `package/IronwindCombat-actions-v95.zip`。游戏内首个中立反击、格挡被破后的姿势和按住右键 WASD 仍待 v95 实机验证。

## v94

- v93 实战日志确认 DLL 正确加载，但该场 7 次原版挥刀全部 `hits=0`，所以放在 `_getHit` 的中立通知一次都没有执行；与此同时所有目标起手前已为 `enemy=1`，却没有受害者侧主动攻击任务。结论是直接 AttackState 能改变／读取敌对关系，却没有恢复原版 unprovoked 任务在“攻击尝试开始”时负责的受害者接战入口。
- 明确目标在直接 AttackState 成功初始化后立即收到原版 campaign 通知（此前中立时）、`attackingYou` 和受害者自身的 `attackTarget(player)`。它不再要求先打出伤害，因此挥空或被格挡也会像原版主动攻击一样构成挑衅。该任务建立在受害者 NPC 上，玩家仍只由一次输入授权一刀，禁止 AI 走位、定时、追击或连击。
- 原版横扫攻击区可能接触非初始目标。v94 在 verdict 前保存额外受害者的中立快照；若其随后真正进入 `_getHit`，再执行同一受害者侧通知和应战入口。每刀每名受害者最多一次，玩家队伍继续硬排除。
- 新增 `attack_victim_provoked source=direct_start/incidental_hit was_neutral=... victim_enemy=... attacker_enemy=... target_set=...` 诊断。已通过静态检查、Visual C++ 2010 x64 编译、安全部署和三处哈希一致性验证；DLL SHA256 为 `841E305D778A4EE5EB3E08C915B1C78C4F7856F0055998F35E028B756590EE73`，PDB SHA256 为 `BB670881689054E602E43486B8C5755650CB595F075DB1F35BC7BDD36B6BD854`。个人 INI 保留，v93 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v94-20261021-212301.dll`，并生成 `package/IronwindCombat-actions-v94.zip`。部署时游戏仍运行 v93，重启后才加载 v94。

## v93

- v92 实机反馈证明：删除自动模式的 DEFENSIVE_COMBAT 站立命令既没有消除怪姿势，又让右键不再开启原版防御状态。v93 恢复右键期间的防御命令和状态语义；在真正来袭前，仅清除当前受控角色的移动动画覆盖与上身 idle，并保持正常持械战斗 idle。实际来袭仍沿用 v91 的一次性原版 `chooseBlock`，不改变概率，也不放行 AI 攻击、追击或连击。
- 修补直接原版 AttackState 的世界通知缺口：中立受害者确认进入 `_getHit` 时，每次授权挥刀、每名受害者至多执行一次 `notifyTheCampaignOfAnAttack` 与 `attackingYou`。伤害、格挡、命中范围仍完全走原版；不调用 `attackTarget`，因此受害者是否反击、逃跑、呼救或受身份规则约束继续由 Kenshi 原版决定。
- 新增 `ai_guard_ready_pose neutral_combat_idle` 与 `neutral_world_notified victim_enemy=... attacker_enemy=...` 诊断。SparringReborn 的现有钩子对非活跃切磋对象会调用原函数，不是普通中立通知缺失的来源。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署与构建／暂存／安装哈希一致性验证。DLL SHA256 为 `736FB2E94ED878BC6D348FBE1DE6B08D9E6C51D63657F7E93917BB8EAF28856A`，PDB SHA256 为 `7C573141AB5972042368CB6B356ECD63D1519026D7A38C989AE34204F92305F8`。个人 INI 保留，v92 DLL 备份为 `backup/IronwindCombat-pre-deploy-v93-20261021-211129.dll`，并生成 `package/IronwindCombat-actions-v93.zip`。部署时 Kenshi 仍在运行，当前进程继续使用已加载的 v92；完整重启后才会加载 v93。

## v92

- 用户确认 v91 自动格挡已经能够正常工作，但按住右键后、敌人来袭触发真正格挡前存在怪异姿势。v91 会话报告记录 36 次 `block_hold vanilla_order=applied` 和 17 次 `ai_block_attempt`；典型样本在右键按下时立刻开启 DEFENSIVE_COMBAT，而真正的 AI 招式在约 0.3～0.6 秒后才选出，中间画面正是无关的原版防御站立姿势。
- 自动模式不再在右键按下时切换 DEFENSIVE_COMBAT 站立命令。等待真实来袭时保持现有 V 战斗待机并维持既有移动限制；`chooseBlock` 返回非空招式后才进入原版 BLOCK 和对应方向姿势。手动方向格挡继续使用站立命令及原有姿势，未改概率、方向或命中判定。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署与构建／暂存／安装哈希一致性验证；仅有既有 Ogre/MyGUI C4819/C4091 警告。DLL SHA256 为 `125C87FE4D7084202268C2F76FB06B8414CA37740B22F5E6EF6FDF789AC95BFD`，PDB SHA256 为 `D2BB78389CBFC40B2B112C1E5E7A79FB8F5BACF05875334E3801B555E4DFA5CC`。个人 INI 保留且 `AiBlocksInManual=true`，v91 DLL 备份为 `backup/IronwindCombat-pre-deploy-v92-20261021-210120.dll`，并生成 `package/IronwindCombat-actions-v92.zip`。

## v91

- 用户确认自动格挡开启后虽然不再按鼠标方向摆防御，但角色实际一直不格挡。上一场落盘日志仍是手动模式，无法给出自动模式事件链；静态审查确认 v86 仅在“右键按住＋`glintIncoming()`”时放行原版 AI 决策帧，而 V 模式同时会把角色空闲态固定在 WAIT。原版经常未在 impact 前自行走到 `CharStats::chooseBlock`，因此请求没有可靠落成防御动作。
- 自动模式改为每次原生 `attackingYou` 来袭只提交一次原版 AI 选择：使用敌人当前真实 impact 方向、来袭侧和攻击技能调用原版 `chooseBlock`。返回格挡或闪避招式才进入原版 BLOCK；返回空即为本刀原版防御失败，不会逐帧重抽、不会变成百分百格挡。
- 权限边界不变：只有玩家按住已接受的右键请求且敌人处于真实 STARTUP/CHOP 时才尝试；松开、未按右键、受击中或玩家攻击中均不触发。新增 `ai_block_attempt chosen=0/1` 诊断，每次敌方挥刀最多一条。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署与构建／暂存／安装哈希一致性验证；仅有既有 Ogre/MyGUI C4819/C4091 警告。DLL SHA256 为 `7313432AE296FFDB28DDF8659CA8A471BD52495C608212972AA912CC793BA9BF`，PDB SHA256 为 `34BA3B2AC99D866B34F6DA10684B7798390D3A5B4A3FCC81A9F7E1F4B6B96E08`。个人 INI 在部署中保留，随后按用户当前选择把安装项 `AiBlocksInManual` 设为 `true`；v90 DLL 备份为 `backup/IronwindCombat-pre-deploy-v91-20261021-205305.dll`，并生成 `package/IronwindCombat-actions-v91.zip`。

## v90

- 分析 v89 实战会话：69 次释放、48 次原版攻击起手、47 次正常闭合，唯一未闭合攻击在 `CHOP_WEAPON` 启动 101 ms 后被命中。原版 `_getHit` 已把状态写为 `STARTUP(3)`、下一状态排为 `STUMBLE(8)`，但铁风的攻击跟踪先把共享的 STARTUP 认作“攻击仍在进行”，使旧攻击所有权与受击 pending 同时存活。其后输入持续被 `hit_reaction_input_dropped`，退出并重新进入 V 后每次左键也只进入 `attack_queued`。
- 修正所有权优先级：`manualAttackStillSwingingOrRecover` 现在先检查真实原版受击反应，再判断 STARTUP/CHOP 是否仍属于玩家攻击。攻击途中被命中会结束铁风攻击账本，让原版 `STARTUP → STUMBLE/REACTION_BLOCK` 自然推进；没有固定受击时间，也不写原版状态、伤害或动画时长。
- 增加 V 退出防线：在丢失受控角色指针前释放仍存活的铁风攻击账本，并清一次攻击提交、缓存和选向目标；原版 CombatClass 状态和动作继续由游戏接管。该路径记录 `attack_v_boundary_released`，用于识别异常边界而不是日常强制恢复。
- 用户已确认 v89 的角色中心声音修复生效。v90 未修改音频、四向手势、原版命中/格挡/伤害或攻击片段推进。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署与构建／暂存／安装哈希一致性验证；仅有既有 Ogre/MyGUI C4819/C4091 警告。DLL SHA256 为 `986AC2B5EB53AA8A3CF747E3CF7097E494B6EB26015D7289CE1CF7946518D7AC`，PDB SHA256 为 `325AB8D0FB7949AD55DC0396C48CDBF2D1868624507C3F777C13FD08A2C0EE23`。个人 INI 保留，v89 DLL 备份为 `backup/IronwindCombat-pre-deploy-v90-20261021-204552.dll`，并生成 `package/IronwindCombat-actions-v90.zip`；受击打断和 V 重进仍待实机验证。

## v89

- v88 实机日志确认监听器已经精确等于角色 `audioData.Position`，但视觉镜头拉远后角色声音仍消失。反汇编 Kenshi 1.0.65 的 `Character::audioEvent` 后确认：当 `SoundRange != 0` 时，原版会先按视觉相机距离拒绝事件，监听器提交发生得更晚，因而 v87/v88 只移动监听器无法挽回已经被丢弃的声音。
- 新增角色音源发布门覆盖：仅在 V 模式、仅对当前受控角色把传入 `Character::audioEvent` 的范围改为原版 `SOUNDRANGE_ALWAYS`；其他角色、环境音、V 外行为和 Wwise 音量/衰减保持原版。原有角色坐标监听器继续负责空间位置和当前视角朝向。低频日志 `dc_audio_source_range_bypass` 记录实际被放行的事件及原生范围。
- 分析本场 72 次方向开始：左右 47 次（65.2%），上下 25 次（34.8%，其中下划突刺仅 6 次）。旧代码的裸 DirectInput X/Y 虽以 45° 为基础边界，但 1.35 倍“保留上一轴”迟滞会把已选中的左右方向继续扩张，并非操作意义上的均匀。v89 在判向空间对纵轴作 1.25 倍归一化，并把对角迟滞缩到 1.15；四方向、径向死区、箭头语义和攻击招式映射均不变。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署与构建／暂存／安装哈希一致性验证；仅有既有 Ogre/MyGUI C4819/C4091 警告。DLL SHA256 为 `ED0D3B0F21E730BE3835B51646B74723FEE18B9A3E9E12B10259C7F0AC01C721`，PDB SHA256 为 `5077E5756A8C5FD89FCDA70726AF494E5235C70D8C3FB8C97FA9CBB66FD0AE89`。个人 INI 保留，v88 DLL 备份为 `backup/IronwindCombat-pre-deploy-v89-20261021-203326.dll`，并生成 `package/IronwindCombat-actions-v89.zip`；两项行为改动仍待 v89 实机验证。

## v88

- 分析 v87 实战日志：46 次释放中 35 次形成有效攻击提交，34 次原版攻击和 1 次自由空挥全部闭合，没有上一刀永久卡住。用户感到“一段时间无法攻击”的直接证据出现在第 11 次受击：原版 STUMBLE 后角色 KO，V 自动退出；约 85 秒后重新进入 V，旧的铁风原子受击锁仍在，首次左键两次被 `hit_reaction_input_dropped`。
- 新增 V 边界同步：手动战斗所有权关闭时只清 `s_hitReactionPending` 与 `s_hitReactionInputLock`，不写原版 combat state；重新进入时读取角色当前 STUMBLE/REACTION_BLOCK，真实反应仍在则继续锁，否则清除跨会话残留。没有增加固定受击时长，也未改攻击、伤害、格挡或原版反应链。
- v87 的音频钩子已由日志确认成功安装并三次进入 `dc_audio_listener_anchor_on`，但用户确认远镜头仍无声。v88 不再从 Ogre 身体根节点推导监听器坐标，改用角色自身 `audioData.Position`，与角色 Wwise 音源处于相同坐标域；值异常时才回退身体根节点。入口日志新增 `source`、listener、原版 camera 和 body 对照。
- 已通过静态检查、Visual C++ 2010 x64 编译、安全部署与构建／暂存／安装哈希一致性验证；仅有既有 Ogre/MyGUI C4819/C4091 警告。DLL SHA256 为 `870B70DDE09F63954BD67A5B8CBB2D0C56BF42C41F92A908ADCA0453E41A17D1`，PDB SHA256 为 `F1EFB15A593F7CEDC0808002FF8F66E4223713FE91AFBA006146FF61DFEBA0B6`。个人 INI 保留，v87 DLL 备份为 `backup/IronwindCombat-pre-deploy-v88-20261021-200306.dll`，并生成 `package/IronwindCombat-actions-v88.zip`；两项改动均待 v88 实机验证。

## v87

- 修正 V 模式视觉镜头拉远后近处声音随镜头衰减的问题：在 Kenshi 原版音频监听器提交边界，只把监听器世界位置替换为受控角色躯干坐标；两个朝向向量、左右／前后定位、全局音量和各音源衰减全部保持原版。
- 覆盖所有 V 模式视角而非仅越肩镜头，包括普通 V、脱离式 OTS、第一人称和 V 模式库存镜头。退出 V、读档／销毁场景、受控角色失效或离开更新列表时立即停止覆盖，恢复原版相机监听。
- 监听器提交函数不使用裸 RVA：从 KenshiLib 符号解析后的私有 `CameraClass::updateAudio` 函数体中搜索经前后字节校验的唯一调用点，并验证目标函数序言；游戏可执行文件变化时只禁用音频锚定并记录错误，不影响其余 V 模式。
- 新增启动日志 `audio listener anchor hook OK` 以及低频状态日志 `dc_audio_listener_anchor_on/off`。已通过 Visual C++ 2010 x64 静态检查、编译和安全部署；构建、暂存和安装 DLL SHA256 均为 `A55314976FF1A4F0F1D9FF15584F976728AB24A73317240F709E4ED8630FA4C3`，PDB SHA256 为 `C4DC936A2FF557D09C904630A541E7D1A8C2165870906E2D7A9FE5BEA3829030`。个人 INI 保留，v86 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v87-20261021-012137.dll`，并生成 `package/IronwindCombat-actions-v87.zip`；实机听感仍待验证。

## v86

- 修正 v85 对“自动格挡”的语义误解：开启后不再被动常开，也不会清除每帧的右键输入。玩家仍须按住右键发出防御请求，松开即停止；仅方向和具体原版防御招式交给 Kenshi AI。自动模式不写手动方向、自由格挡姿势或完美格挡时窗，且只有“有效右键请求＋真实来袭”同时成立时才放行防御决策，继续禁止 AI 自主防御、攻击、追击和连击。
- 修正断腿爬行／趴地敌人无法成为攻击目标：Kenshi 的 `isUnconcious()` 对 `PS_CRIPPLED` 与 `PS_PLAYING_DEAD` 也可能为 true，旧筛选因此误删目标。新统一筛选允许断腿、装死和仍有意识倒地目标进入原版 AttackState，继续排除死亡和真正 `PS_KO`；命中范围、攻击区、格挡和伤害仍完全由原版判断。
- 新增 `block_press_accepted mode=automatic_ai`、`ai_block_choice` 及 `attack_direct_native prone/down/unconscious` 诊断，用于下一场区分“玩家未请求”“AI 未选出防御”和“趴地目标被原版初始化拒绝”。
- 已通过 Visual C++ 2010 x64 静态检查、编译和安全部署；构建、暂存和安装 DLL SHA256 均为 `9AA03D1DE52F9B3E299EA0D5E90D0C8748F3B73940945A1A22585C9375882102`，PDB SHA256 为 `82686BB842154F089598C1F96BA6A83355DBFDE7CF388EEFF5E53B563F48E4B1`。个人 INI 保留且当前 `AiBlocksInManual=true`，v85 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v86-20261021-011100.dll`（SHA256 `80DF7F5B4004ADB04A0050F8C1B95A73B4F44625A47796ADB11CCB31BFDBE8BB`），并生成 `package/IronwindCombat-actions-v86.zip`。自动右键防御与断腿目标攻击仍待实机确认。

## v85

- 将已有 `AiBlocksInManual` 重新定义为互斥的格挡模式开关。关闭时保留方向手动格挡与完美格挡；开启时立即清理全部手动格挡姿势、方向、时窗和命令，由原版 AI 独立选择格挡/闪避时机与方向。
- 自动模式只在原版 `attackingYou` 通知对应的真实来袭状态中放行完整防御决策；未受攻击时仍抑制 AI 攻击、寻路和连击。即使关闭武器闪光显示，来袭通知仍单独供自动防御使用。
- 自动模式下短按右键不产生手动格挡，也不改变 AI 方向；按住 200 ms 的非敌对交互仍可使用。方向 HUD 不再把自动模式的右键显示为玩家格挡。
- 后续实机确认该实现错误地把自动模式做成了被动 AI 防御，并在每帧清除 `s_blockHeld`，导致玩家无法用右键请求格挡；由 v86 修正。此条仅保留问题演进记录，不代表当前设计。
- 已定位但未在本版猜测修复两项镜头问题：远镜头声音衰减来自原版音频监听更新仍受脱离相机位置影响；极端向下看时镜头靠近头顶来自完整俯仰轨道压缩水平后距，而非本轮日志中的墙体碰撞拉近。
- 已通过 Visual C++ 2010 x64 编译并安全部署，匹配 PDB 与构建指纹已生成；构建、暂存和安装 DLL SHA256 均为 `80DF7F5B4004ADB04A0050F8C1B95A73B4F44625A47796ADB11CCB31BFDBE8BB`，PDB SHA256 为 `5C333394CE19EEAC75C8D5458BA7F35927F1206BF8A363C0F268A3DE1FAD1FAC`。个人 INI 保留，v84 稳定 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v85-20261021-005620.dll`，并生成 `package/IronwindCombat-actions-v85.zip`。自动格挡、远镜头音频和极端下俯仍待实机验证。

## v84

- 分析 v83 实战：69 次释放中 58 次成功启动原版 CHOP，28 次候选接触全部进入真实 `_getHit` 并触发红 X；30 次 post-impact 格挡交接、11 次自然 clip 收尾，零崩溃、零 `attack_native_state_recovered/early_exit`、零寻路或 AI 自动攻击接管。所有有目标攻击仍由原版 AttackState 的攻击区、verdict、伤害和多人接触结算。
- 本场没有出现敌人格挡或闪避（28 hit、0 block、0 dodge），但代码只对受控玩家应用手动防守覆盖；所有其他角色的 `chooseBlock` 和 `_iHitYouAreYouHit` 仍原样调用原版，因此能力和概率未被铁风关闭。
- 红色断开 X 保持位置和四段布局，字体高度 24→26，并增加深红 1 px 文字阴影以加粗；最大 alpha 由 1.00 调为 0.82，尾端为 0.22，继续按 1000 ms 淡出。
- 修正纯诊断误名：clip 存在且已到 0.98 后的 technique 完成现在记录 `source=technique_finished_clip_complete`，不再错误写成 `no_clip`。未修改攻击、伤害或状态行为。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `BBD90449030A09F8EEF563D23639C74447CA272304676148DBC4B76F19B74B71`；个人 INI 保留，部署前 v83 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v84-20261020-235852.dll`（SHA256 `4166F6B435DF6C5EB4037B1628D89F4C040DB992656C012FF98485023D8D5B83`），并生成 `package/IronwindCombat-actions-v84.zip`。尚未启动 v84，红 X 观感及稀有短动作仍待实机确认。
- 2026-10-21 用户完成 v84 实战后确认整体体验已经较舒服，近战系统可作为后续细节打磨与弩系统开发的稳定基线。该确认不等同于完整回归矩阵通过；四向闪避、低频短动作及其他既有待测项仍保留原状态。
- 2026-10-21 增加工程自动化但不改运行 DLL：`tools/ironwind.ps1` 统一状态、检查、构建、部署、验证、实机会话和快照；动态日志分析器由 JSON 规则驱动，并自动发现未分类新事件、配对攻击链、按中位数/MAD 标记时长离群。构建流程从后续正式版本起强制生成匹配 PDB 与构建指纹，部署拒绝陈旧 DLL 和同版本不同二进制的静默覆盖；运行资源真源迁至 `runtime/`。

## v83

- v82 首次实机在 `attack_native_impact_finished progress=0.257` 后按右键，同一 CHOP 尚在视觉收招时又执行了 `free_block_play anim=blk right`，442 ms 后崩溃。转储为 `0xC0000005` 读取 `0x18`，故障地址 `kenshi_x64.exe+0x5B357A`，位于原版 `AnimationClass::endCombatAnimation()`；确认是攻击与自由格挡同时占用同一动画层，而非随机游戏崩溃。
- 新增排他的 post-impact 格挡交接：impact 前只保留按压，不取消攻击；impact 后先用原版状态转换离开 CHOP，并在本帧立即返回，下一主循环确认 `s_attackActive=false` 后才允许播放格挡。`!s_attackActive` 同时成为自由格挡启动的硬门。
- 新增 `s_attackGuardHandoffPending`，让统一活片段恢复门识别这次主动退出，避免把它误判成外部状态抢占并重新恢复 CHOP。诊断新增 `attack_guard_wait_for_impact` 与 `attack_guard_handoff before/after/accepted`。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `4166F6B435DF6C5EB4037B1628D89F4C040DB992656C012FF98485023D8D5B83`；个人 INI 保留，部署前 v82 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v83-20261020-234843.dll`（SHA256 `590E60ED26002A827B507C2E6737BBDB82E6C93AFFDC52D226A93CBB8B5C95AE`），并生成 `package/IronwindCombat-actions-v83.zip`。尚未启动 v83，攻击完整收招和 impact 前后转格挡待实机确认。

## v82

- v81 实机 22 次释放全部成功进入原版 CHOP，18 次记录原版 technique 完成、3 次真实受击；没有自由动画或无法出招方向，也没有 `attack_native_state_recovered/early_exit`。日志层状态正常，但用户明确观察到未命中时动作像卡帧一样停在一半。
- 根因修正：`currentTechniqueHasFinished` 是原版攻击/impact 阶段完成边沿，不是整段 Ogre 动画播放完。此前在该边沿立即 `setCombatState(DECISION)`，大量无接触攻击在 657～812 ms 被截断；同一 `Great sword low slash` 的完整自由动画历史证据约为 1562 ms，符合“一半”的画面反馈。
- 新增 `s_attackNativeImpactFinished` 两阶段执行。原版标记 impact 完成后立即停止再次调用 `CombatClass::attackState`，避免重复接触和伤害；保持 CHOP 身体所有权，让同一原版 clip 仅由动画系统自然推进。clip 达到 0.98 或消失后才转 DECISION 并收权，不使用固定毫秒数。
- 新增 `attack_native_impact_finished`、`attack_native_recovery_clip`、`attack_native_finish_transition source=clip_finished` 诊断。真实受击仍优先中断；活片段状态恢复门同时识别 impact 后的视觉收招。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `590E60ED26002A827B507C2E6737BBDB82E6C93AFFDC52D226A93CBB8B5C95AE`；个人 INI 保留，部署前 v81 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v82-20261020-234031.dll`（SHA256 `2963270E5665A778FE192E389E97AAA6DE9815451977E512CA8114FC50EC20F1`），并生成 `package/IronwindCombat-actions-v82.zip`。尚未启动 v82，完整视觉收招及 impact 后不重复伤害待实机确认。

## v81

- v80 实机 45 次提交中，44 次尝试原版初始化：35 次成功，9 次因当前重武器没有所选方向动作而 `started=0`；另有 1 次场上已无 2×准星范围内敌人，按设计走纯动画空挥。
- 35 次成功 CHOP 中，29 次按原版 technique 完成，3 次分别在 172/187/609 ms 被敌人真实命中打断，另有 3 次在 0/234/329 ms 无受击、无完成边沿地提前退出。后者就是用户仍看到的“只挥一半”。
- v80 的 `attack_native_state_recovered` 为 0，不是恢复条件被证伪，而是 `combatGo_hook` 内较早的 `manualAttackTrack` 已先调用 `manualAttackFinish`；主循环末尾的恢复检查永远来不及看到状态。
- 新增统一的 `manualAttackStillSwingingOrRecover`。`manualAttackTrack` 与 `manualAttackLifecycleTick` 都必须先通过该门：同一 technique 的活片段确实未完成时最多恢复一次 CHOP；真实受击反应直接让位；不能恢复的异常记录 source、clip、progress、finished 与 recovered 后再收权。不使用固定时长。
- v80 的伤害确认已通过本场验证：22 次 `hit_candidate` 全部对应 22 次受害者 `_getHit` 和红 X；两次 `attack_incidental_carrier_anywhere` 均进入原版链，其中一次实际命中了非载体对象。完整挥刀仍允许原版 `contacts=0`，不手工补伤害。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `2963270E5665A778FE192E389E97AAA6DE9815451977E512CA8114FC50EC20F1`；个人 INI 保留，部署前 v80 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v81-20261020-233444.dll`（SHA256 `EFBF5A025FC291FFEF72BA4C3CCFAD3522FEBA90566726863960A4FAC489F310`），并生成 `package/IronwindCombat-actions-v81.zip`。尚未启动 v81，统一恢复门待实机确认。

## v80

- v79 实机 32 次成功原版攻击中 31 次按原版完成，1 次在 328 ms、无受击且 technique 未完成时提前回到 DECISION；外层 AI update 竞态已从 10/31 降为 1/32，但仍有一个更晚的状态覆盖入口。
- v79 共 22 次原版未格挡 verdict，21 次真正进入受害者 `_getHit` 并携带约 58.79～81.96 的原版伤害；另 1 次 verdict 没有后续 `_getHit`。红色 X 和 `hits` 汇总因此从 verdict 移到确认的 `_getHit`，候选接触不再产生假反馈。
- 32 刀中有 8 刀命中本刀初始对象之外的角色，证明原版攻击区本来支持顺带命中；但另有 5 次释放瞬间无目标的攻击直接降级为永久 `damage=0` 自由动画，即使挥刀途中准星随后取得敌人也不能伤害。这是“目标决定有效性”体感的主要来源。
- 无精确目标且前方载体搜索失败时，新增 2×准星范围内的 360° 最近敌对执行载体；该宽相位覆盖 v79 五次纯空挥中约 44～50 的最近敌人。它只让原版 AttackState 存在，不自动转身、不接近、不授权命中，也不选择中立或玩家队伍。刀路实际碰到谁、格挡、闪避和伤害仍完全由原版处理。
- 对无受击提前离开 CHOP 的残余情况，只在同一原版 technique 的真实动画片段仍在播放、进度小于 0.98 且 `currentTechniqueHasFinished=false` 时恢复一次 CHOP；否则记录 `attack_native_early_exit` 并正常收权。不使用固定时长。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `EFBF5A025FC291FFEF72BA4C3CCFAD3522FEBA90566726863960A4FAC489F310`；个人 INI 保留，部署前 v79 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v80-20261020-232605.dll`（SHA256 `4D19597912E2604AD503543A89647EC87E21E21D31997310F402E94EB346D14A`），并生成 `package/IronwindCombat-actions-v80.zip`。随后实机确认载体与真实命中反馈生效，但活片段恢复被更早的跟踪出口绕过，见 v81。

## v79

- v78 实机 44 次释放、43 次提交中，31 次进入原版 CHOP、7 次走空气挥刀、5 次因当前大型武器没有所选方向动作而记录 `free_attack_no_technique`。31 次 CHOP 中 17 次完整播放，4 次被真实 `hit_on_me` 合理打断，但另有 10 次在 0～328 ms 内无受击提前退出；其中多次为 0/15/16 ms，确认不是距离或玩家错觉。
- 竞态位于状态所有权：直接攻击在主循环帧尾建立 CHOP，下一帧外层 `CombatClassAI::update` 有时在 `combatGo_hook` 之前把状态改回 DECISION。已有 go 拦截因此来不及生效；个别样本还在 16 ms 内读取陈旧完成边沿并立即结算 impact。
- 新增 `combatAiUpdate_hook`。仅当受控角色的一次玩家授权 CHOP 正在执行且没有原版受击反应时，跳过 `CombatClassAI::update` 的决策层，改为调用已有的原版非虚 `CombatClass::update → attackState`；仍以每帧序号去重。受击、僵直、格挡及其他角色继续运行完整原版 update。
- 原版攻击 verdict 后新增只读诊断 `attack_native_gethit`，记录受害者 `_getHit` 路径、原版伤害载荷和 stumble。下一场可区分“画面相交但原版攻击区零接触”“HIT_FLESH verdict 后未进入受害者路径”和“确实进入原版伤害/反应路径”。不手工补伤害。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `4D19597912E2604AD503543A89647EC87E21E21D31997310F402E94EB346D14A`；个人 INI 保留，部署前 v78 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v79-20261020-225535.dll`（SHA256 `7A325015872032E82079EC4DDA8D61565ECC63F5A9E8284D0AA130466593F02A`），并生成 `package/IronwindCombat-actions-v79.zip`。随后实机确认 update 所有权修复有效：31/32 刀正常按原版完成；伤害诊断确认 21 次真实 `_getHit`，并暴露 1 次 verdict 假阳性和 5 次无目标永久无伤害空挥，见 v80。

## v78

- v77 实机验证否定“按当前目标距离决定能否起手”：36 次释放仅 8 次进入原版攻击，13 次被 `attack_direction_native_rejected` 拒绝，另有大量输入转为空挥或缓存；箭头还在目标捕获/释放边沿在距离资格与武器能力之间来回切换。该交互违反“随时可以挥空气，打到人只是结果”的核心要求。
- 攻击改为动作优先：方向输入只限定刀路；每次提交先从原版攻击池收集当前武器、技能、单双手和同方向的 technique，再用角色自己的 chooser 在 0/5/12/25 中性参考距离补充，最后随机选择一个并记录 `attack_direction_native_random`。目标距离不参与动作许可，真正没有该方向动作才记录 `attack_direction_unavailable`。
- HUD 恢复为纯武器能力：绿色只表示存在该方向的原版动作，白色表示无动作/未知，不再根据目标距离变色。
- 精确目标仍优先作为原版 AttackState 承载对象；未捕获目标时，前方 80° 内最近敌人可记录为 `attack_incidental_carrier`，仅用于让原版攻击区、接触、格挡、闪避和伤害链运行。它不授权命中、不自动转向、不接近，也不选择中立或玩家队伍。前方没有敌人时继续播放空气挥刀。
- 保留 v77 的 `attack_native_verdict outcome=hit/blocked/dodged` 与 `attack_done contacts/hits/blocks/dodges` 诊断。不恢复手工伤害扇区、AI 走位、等待停步、自动靠近或自动连击。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `7A325015872032E82079EC4DDA8D61565ECC63F5A9E8284D0AA130466593F02A`；个人 INI 保留。部署前 v77 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v78-20261020-224541.dll`（SHA256 `B060FF70929F78CF34D7E5B7D467A50A2978A969E1A1E1CFD0A83A5E46B247BB`），原版攻击池补强前的中间 v78 另备份为 `backup/IronwindCombat-pre-deploy-v78-20261020-224716.dll`（SHA256 `A844E36B3AFE945889A9735D1BCB4B77C2ED54384518E6018A8C17218666F27E`），并生成 `package/IronwindCombat-actions-v78.zip`。随后实机确认动作与距离已解耦，但暴露外层 AI update 抢先结束 CHOP 的竞态，见 v79。

## v77

- v76 实机 29 次释放中，有目标直接初始化 23 次：16 次启动、7 次被当前方向拒绝。16 次启动里 7 刀产生 19 个原版真实命中；3 刀完整播放 672～813 ms 但零接触，目标距离分别为 43.8、46.8、50.5；另有 6 刀在途中收到 `hit_on_me`，属于原版受击打断。
- 三次完整落空全部由 `attack_direction_native_fallback` 强制启动：原版 chooser 已在当前实际间距拒绝该方向，旧代码却用 0/5/12/25 的假距离重新取招式。人群中央时同一原版攻击区可能碰到其他近敌，边缘单挑则只剩完整空刀。
- 删除跨距离 fallback。方向钩子现在最多采样 32 次，但每次都使用 AttackState 给出的同一真实 range；只有原版此刻认可的同方向招式才能进入 CHOP。拒绝记录 `attack_direction_native_rejected`，不会换方向、等待靠近、延迟补刀或交给 AI。
- 按住左键捕获目标后，HUD 以人物中心距减原版 weapon reach 和目标碰撞半径得到与 AttackState 一致的 chooser range，每 150 ms/跨 0.5 单位刷新原版采样；当前距离认可的方向绿色，其余白色。这个预览只着色，实际起手仍由原版初始化再次判定。
- 新增每刀原版接触结果：`attack_native_verdict outcome=hit/blocked/dodged`，并在 `attack_done` 汇总 `contacts/hits/blocks/dodges`。零接触、格挡、闪避、真实伤害和受击打断现在可分别核对。
- 不改变原版攻击区、多人命中、伤害、护甲、XP、敌人格挡/闪避或受击打断；没有恢复 AI 决策、走位、接近和自动连击。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `B060FF70929F78CF34D7E5B7D467A50A2978A969E1A1E1CFD0A83A5E46B247BB`；个人 INI 保留，部署前 v76 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v77-20261020-223732.dll`（SHA256 `FC651C00DA57568D8C1AF0067C81B6713B0938E6D3A833E0D10DD062499B51BF`），并生成 `package/IronwindCombat-actions-v77.zip`。尚未启动 v77，当前距离绿/白、边缘 poke 和敌人 verdict 待实机确认。

## v76

- v75 实机日志否定了“非战斗入场只等待一帧”的方案：27 次释放产生了 710 条 `attack_direct_wait_combat_entry`，移动期间 `combatModeActive` 会被直接控制路径反复清除，使已接受输入等待约 187～1141 ms，甚至被后续输入覆盖；全场只有 12 次直接攻击正常启动。
- 该循环也解释了画面上的异常：移动时人物维持攻击前姿态，停止移动才出刀。这一外观已记录为未来显式蓄势设计的参考，但当前不是受控功能，不能以阻塞自由攻击的方式保留。
- 删除 v75 的跨帧等待与提交窗口续期。有目标攻击恢复为释放帧同步调用原版 `AttackState::initialiseAttack(target)`；不固定动作时长、不延迟补刀、不恢复 AI 任务，也不改变原版攻击区、距离、格挡、伤害或受击打断。
- v74 中 47 次成功启动里仅 1 次出现的 15 ms 战斗入场覆盖仍保留为独立待定位项；在找到不阻塞输入、且不会形成 AI 接管的状态所有权方案前，不再用等待重试规避。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `FC651C00DA57568D8C1AF0067C81B6713B0938E6D3A833E0D10DD062499B51BF`；个人 INI 保留，部署前 v75 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v76-20261020-222301.dll`（SHA256 `D0C24A83217DB5F2B8F63A3759AD14F8DAD6CA60EA376156975AECEF0B03430E`），并生成 `package/IronwindCombat-actions-v76.zip`。尚未启动 v76，即时提交和移动中攻击待实机确认。

## v75

- v74 极限距离拉扯实战中 48 次直接初始化有 47 次成功；23 刀产生真实命中，20 刀完整落空，4 刀被敌人命中打断。距离分组显示：20～30 距离 13 刀中 10 刀命中，而 ≥40 距离 17 刀仅 2 刀命中、15 刀原版落空。保持原版范围，不扩大命中区。
- 三次明显提前结束（203/297/469 ms）都紧跟 `hit_on_me` 并进入原版受击反应，属于敌人先命中的正常打断。另有一次异常攻击在 15 ms 后无受击、无 `finished` 就退出；同帧出现 `combat_entered`，证明原版战斗入场流程覆盖了刚启动的 CHOP。
- 从 `combatModeActive=false` 发起有目标攻击时，现在先武装原版战斗模式并保留同一已接受输入，下一帧再初始化 AttackState；新增 `attack_direct_wait_combat_entry`。这不是固定攻击时长，也不缓存僵直期输入。
- 不改变武器范围、攻击区、格挡/闪避、伤害、护甲、XP 或敌人正常打断。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `D0C24A83217DB5F2B8F63A3759AD14F8DAD6CA60EA376156975AECEF0B03430E`；个人 INI 保留，部署前 v74 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v75-20261020-221506.dll`，并生成 `package/IronwindCombat-actions-v75.zip`。尚未启动 v75，战斗入场延迟一帧的路径待实机确认。

## v74

- v73 饥饿土匪实战中 58 次原版直接初始化有 51 次成功；其中 27 次产生真实命中、19 次完整挥刀但无命中、5 次被敌人攻击打断。落空样本大量显示粘滞准星焦点选中了 25～51 距离的旧目标，场上却有明显更近敌人；典型样本为目标 29.8、最近人物 7.7。
- 新增只用于本次 LMB 初始化的“前方近敌”选择：在原有准星攻击锥和 AimRange 内以距离优先、中心角度辅助选择；只有旧粘滞目标同时比候选远 8 以上且远 35% 以上时才替换。玩家通过 RMB 明确锁定的目标不替换。
- 此改动只决定把哪个敌人交给原版 `AttackState::initialiseAttack`；不调用伤害函数，不扩大攻击区，不改变武器距离、格挡、闪避、伤害、护甲或 XP。挥出后是否命中仍完全由原版 impact 决定。
- “没挥出”中的 7 次真实初始化失败均对应当前武器白色不可用方向；另有 7 次输入发生于原版受击反应窗口而按既定规则丢弃。v74 不把白色方向伪造成错误刀路，也不缓存僵直期输入。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `600FEFFACFBC1A873B3A22153F6DF98111F52158CA9D33548BF2220339D4991C`；个人 INI 保留，部署前 v73 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v74-20261020-220645.dll`，并生成 `package/IronwindCombat-actions-v74.zip`。尚未启动 v74，拥挤近战重选目标待实机确认。

## v73

- 根据 v72 实机画面反馈，仅把四向箭头的统一视觉中心半径再外移 70 px：`188 → 258 px`。64×64 外框、约 3 px 线宽、箭头几何、单选显示以及绿色可攻击/白色不可攻击规则均保持不变。
- 不修改手势选向、攻击方向映射、准星、命中 X 或任何战斗状态逻辑。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `3AD07B296E0237C4DD27C181DAD6A492F1D7B2EF39CCCAEB4BA879BE1830ECB4`；个人 INI 保留，部署前 v72 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v73-20261020-215440.dll`，并生成 `package/IronwindCombat-actions-v73.zip`。尚未启动 v73，258 px 画面距离待确认。

## v72

- v71 实机共 33 次原版直接初始化，27 次成功进入 CHOP；第一刀及中段两刀各正常执行约 0.66 秒并产生真实命中，其余 24 刀却在进入 CHOP 后 15～32 ms 立即退出。日志逐刀显示这些异常攻击在首次更新前已经是 `currentTechniqueHasFinished=1`，不是距离、输入、目标或攻击区问题。
- 根因是 v70 只补了 CHOP → DECISION 收尾，却未复现原版随后 DECISION/WAIT 帧对“上一招已完成”标志的消费。直接路径下一刀初始化时继承静态真值，完成逻辑把上一刀的完成标志误判为本刀完成。
- 每次直接初始化现在先清除陈旧完成标志，并用本刀专属边沿锁存：只有本次 CHOP 已经实际观察到 `finished=0`，之后原版再把它改为 `1`，才允许执行 CHOP → DECISION。初始化失败会恢复旧标志，不污染自由动作降级路径。
- 不增加固定动作时长，不改 impact 范围、扇区、伤害或格挡，也不恢复 AI 决策/走位/连击。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `244B4C23D269129BB9EAF726D969FC97B941B73FE472E2F50D37529180978F29`；个人 INI 保留，部署前 v71 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v72-20261020-214627.dll`，并生成 `package/IronwindCombat-actions-v72.zip`。尚未启动 v72，连续攻击边沿仍需下一场实机日志确认。

## v71

- 继续分析 v69 的 16 次 `attack_direct_native started=0`：方向 HUD/自由动作扫描始终证明该大型武器有方向 3 的两个合法原版招式（`choose=1010`、`directional=2`），失败并非未解锁动作。直接初始化却只按目标当前距离调用一次原版 chooser；返回空后插件立刻改播无伤害自由动画，因而出现“明明很近、挥了但永远不出伤”。
- 直接控制现在把“能否挥出动作”与“impact 是否碰到目标”分开：当前实际距离未选出同方向动作时，仍只调用原版 `CharStats::chooseAttack`，在 HUD 已使用的四档参考距离中补选同武器、技能和方向的原版 technique，然后进入原版 CHOP。
- 此补选不调用 `hitByMeleeAttack`，不扩大攻击区，不添加扇区伤害，也不保证命中。真实挥刀的 impact 仍由原版 `CombatClass::attackState` 计算范围、格挡/闪避、伤害、护甲和 XP；超距或没砍到仍会原版落空。没有该方向合法招式时仍安全失败。
- 保留 v70 基于 `currentTechniqueHasFinished` 的原版 CHOP → DECISION 收尾。新增 `attack_direction_native_fallback` 日志，用于区分“从原版可用招式补选并真实挥刀”与自由动画降级。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `AF613702778D1A6A4738870C4F596DDBDF714770E1A4792C8C8B1A12CA94BD82`；个人 INI 保留，部署前 v70 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v71-20261020-213032.dll`，并生成 `package/IronwindCombat-actions-v71.zip`。尚未启动游戏。

## v70

- v69 实机共提交 54 次攻击：前 16 次同步原版初始化返回失败，按设计降级为空挥；第 17 次在 `dist=11.0` 成功进入 CHOP，原版在 129.872 秒结算命中，并于 130.222 秒把 `currentTechniqueHasFinished` 置 1。
- 该次攻击随后并未离开 CHOP：到退出游戏前又推进约 22 秒，`finished=1` 始终保持，39 次后续输入只能缓存且 8 次过期。确认不是距离、输入漏收、受击打断或 AI 任务接管，而是直接路径只执行了基类 `attackState`，缺少完整 `CombatClassAI::update` 中独立的 CHOP → DECISION 收尾。
- `manualAttackAdvanceNativeState` 现在只在原版 technique 自己报告完成后调用原版 `setCombatState(DECISION)`，由原版清理战斗动画；随后既有生命周期观察器立即恢复手动控制。没有固定毫秒时长，也没有恢复 AI 选目标、走位或自动连击。
- 保留原版 `AttackState::initialiseAttack` 的距离/武器/技能资格以及原版 impact、格挡、伤害与 XP；初始化失败仍是无伤害空挥，不引入强制扇区或手工命中。新增 `attack_native_finish_transition source=technique_finished` 作为精确验收标记。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `9AA23F88A5CEEFEA9D7C1D81D6C6E64FB674FD0DDCAEDCD564E3FA2DF9A848AC`；个人 INI 保留，部署前 v69 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v70-20261020-212910.dll`，并生成 `package/IronwindCombat-actions-v70.zip`。尚未启动游戏；连续第二刀、原版格挡和初始化失败率仍需下一场日志确认。

## v69

- v68 最新实机确认新 helper 每帧运行：1 次直接攻击后出现 105 条 `attack_native_advance`，0 条 invalid；但每条都是 `state=CHOP timer=0.200->0.200 tech=1 target=1 finished=0->0`。最终 0 命中、0 `attack_done`，后续 32 次输入缓存、8 次过期。没有 focused/unprovoked 任务，也不是距离问题。
- 反汇编纠正执行层：`AttackState::update` 是状态对象侧目标/移动逻辑，不是攻击动画段和 impact 的完整执行包装。原版非虚 `CombatClass::update` 在 CHOP 下直接分派 `CombatClass::attackState`，后者才读取动画组合段并执行攻击区、命中、格挡/闪避、伤害与结束。
- `manualAttackAdvanceNativeState` 改为通过 `GetRealAddress(&CombatClass::_NV_update)` 调用基类实现，明确不做 `cc->update()` 虚调用，因此不会进入 `CombatClassAI::update` 或 decisionState。`combatGo_hook`/主循环兜底的同帧去重、原版受击优先以及无固定动作时长原则全部保留。
- 诊断扩展为同时记录 `combat_timer`、状态对象 `attack_timer`、`combo` 和 `finished`；启动记录 `combat_base_update_resolved rva=...`。已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `9466030E9C9AE458606BCF9C20654494BF12CF900E780A05BA621360C77C478F`；个人 INI 保留，部署前 v68 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v69-20261020-211811.dll`（SHA256 `E1B07DD565F8CA26A4213BB9EA3F2F98D0A18AAC75164A76D307DFD0A19E084A`），并生成 `package/IronwindCombat-actions-v69.zip`。尚未启动 v69。

## v68

- v67 最新实机日志中，11 次同步直接初始化只有 1 次返回成功并进入 CHOP；它随后永久停在 CHOP，没有命中、`attack_done` 或控制恢复，后续左键只进入缓存/过期。问题不在距离，也不是重新出现 focused/unprovoked 任务。
- 静态反汇编确认原版 `AttackState::initialiseAttack(target)` 负责选择招式、目标通知与启动动画，但逐帧攻击区、impact、格挡/闪避、伤害及状态退出位于该状态对象的虚函数 `update(frameTime)`。直接路径没有高层战斗任务，不能依赖游戏继续调度完整 `CombatClass::go`。
- 新增 `manualAttackAdvanceNativeState`：攻击期间只执行引擎持有的 CHOP 状态对象，不运行 `CombatClassAI::update`，因此不引入 AI 等待、走位、选招或自动连击。`combatGo_hook` 优先调用，主循环生命周期在 go 缺席时补调用；主循环帧序号保证每帧最多一次，避免计时和命中双推进。
- 原版受击反应优先于直接攻击执行：`manualHitReactionLive` 成立时两个推进入口均不执行 CHOP，而是让完整原版受击状态接管，避免攻击途中被命中仍在同帧继续 impact。
- 动作完成仍以原版状态真实离开 STARTUP/CHOP 为准，没有增加固定收招时间。新增 `attack_native_advance`/`attack_native_advance_invalid` 诊断，可直接区分状态推进、原版退出和对象异常。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL SHA256 均为 `E1B07DD565F8CA26A4213BB9EA3F2F98D0A18AAC75164A76D307DFD0A19E084A`；个人 INI 保留，部署前 v67 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v68-20261020-210904.dll`，并生成 `package/IronwindCombat-actions-v68.zip`。尚未启动游戏；真实命中/格挡、完整退出、连续第二刀、AI 不接管和中立世界后果仍待实机验证。

## v67

- v66 实机日志证明近战实现仍由 `FOCUSED_MELEE_ATTACK` / `UNPROVOKED_FOCUSED_MELEE_ATTACK` 高层 AI 任务驱动：一次输入后的任务可持续 3～5 秒，AI 决定等待、面向、出刀时机和后续战斗决策；`s_attackActive` 只是宽泛放行许可，违背自由控制主旨。
- 删除近战 focused/unprovoked 任务入口。左键释放当帧由 `manualAttackStartDirect` 取得角色自身的 CHOP 状态对象，直接调用原版 `AttackState::initialiseAttack(target)`；成功才进入 CHOP，失败不寻路、不延迟重试，并降级为既有无伤害空挥。
- 新增精确作用域 `s_attackDirectInitialising`。`attackInit_hook`、`setCombatState_hook` 和 `chooseAttack_hook` 不再因整段 `s_attackActive` 而允许 AI 发起攻击；方向选择只在同步初始化调用内生效，找不到匹配方向时返回失败，不再用 AI 选择的其他方向替代。
- `s_attackActive` 仅保留已开始的原版动作层和 20% WASD；`manualAttackTrack` / `manualAttackLifecycleTick` 只观察 STARTUP/CHOP 的真实退出，不再等待 DECISION/WAIT、任务所有权或入场超时。原版继续处理招式资格、攻击区、通知、动画、格挡、闪避、伤害、护甲和 XP。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL 的 SHA256 均为 `B9197F3908718C8D49B12BBA5CC76F3DAEB7FDD18B967853963D1570A770F808`；个人 INI 保留，部署前 v66 DLL 已备份为 `backup/IronwindCombat-pre-deploy-v67-20261020-203915.dll`（SHA256 `88A48523B71FB5EE0B62901F97D226BA4864A56D944E570AB9DDE97475980861`）。尚未启动游戏，直接攻击时序、中立犯罪/骚乱、敌人格挡和无自动连击待实机验证。

## v66

- v65 实机中第一刀正常命中并收尾；第二次 unprovoked 注入先进入 `attack_native_entry_wait`，约 1 秒后原版记录 `combat_exited`，但没有新的 `attack_done`。之后 17 次左键全部进入缓存并过期，甚至已经重新靠近到 `dist=9.2/8.4` 仍不能提交。没有受击或僵直参与。
- 根因是 `manualAttackTrack` 只由 `combatGo_hook` 调用：原版任务放弃入场后不再调用该战斗循环，因此旧授权无法执行入场超时清理。v65 的 1.5 秒保护存在，但失去了运行位置。
- 新增原版主循环后的 `manualAttackLifecycleTick`，在实时更新列表验证后独立观察一次性授权。未见 CHOP 时，原版退出战斗、进入 TARGET_PATHFINDING 或入场窗口失效都会通过统一 `manualAttackFinish` 清任务并恢复输入；日志新增 `attack_done ... reason=...`。
- 见过 CHOP 后不使用入场超时，仍只在原版离开 STARTUP/CHOP 时收尾；没有改变武器范围、攻击区、格挡、伤害或正常动画时长。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL 的 SHA256 均为 `88A48523B71FB5EE0B62901F97D226BA4864A56D944E570AB9DDE97475980861`；个人 INI 保留，部署前 v65 DLL 已生成回滚备份。尚未启动 v66，连续攻击与失败恢复待实机验证。

## v65

- v64 实机并非没有接收后续左键：首个中立攻击完整走过 unprovoked 原版链并命中，之后每次提交也都有 focused order 和 inject，但 `manualAttackTrack` 都在 15～32 ms 后以 `state=DECISION, swung=0` 清掉任务。问题是原版任务尚未安装 AttackState，就被铁风当作攻击已经结束。
- `manualAttackInject` 不再直接写 `combatState=STARTUP_STATE`、`nextMove=CHOP_WEAPON` 和 `stateTimer`；focused/unprovoked 任务现在是 STARTUP/CHOP 状态转换的唯一所有者，符合原版攻击入口。
- 在第一次观察到 CHOP 前，DECISION/CIRCLE/WAIT/HESITATE/BLOCK 只视为有界的任务入场握手，并记录一次 `attack_native_entry_wait`。原版进入 TARGET_PATHFINDING、任务结束或握手窗口失效仍立即清理，禁止 AI 自动靠近；见过 CHOP 后的动作时长继续完全跟随原版状态。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL 的 SHA256 均为 `D21C2A1AA4E80FD4F1ADD23CBF0538650EBBF6A39B4ADE85BB4FA11CAD7D68ED`；个人 INI 保留，部署前 v64 DLL 已生成回滚备份。尚未启动 v65，连续第二刀和无自动追击待实机验证。

## v64

- v63 实机捕获到大量中立目标，但 standalone `CombatClass::isInAttackZone()` 全部返回 false；甚至 impact `dist=7.93`、`weapon_reach=9.50` 仍为 `native_zone=0`。该函数依赖完整原版战斗状态，不能作为自由动画的独立几何判定。
- 删除中立提交时转入自由 direct-impact 路径的分流。明确捕获的中立目标现在提交 `UNPROVOKED_FOCUSED_MELEE_ATTACK`，与敌对 focused 任务共用完整原版 `STARTUP → CHOP_WEAPON`、攻击区、招式、格挡、伤害、犯罪和阵营链。
- 中立注入必须同时匹配当前按键、捕获目标、待清理任务和 unprovoked 标志；空处仍自由空挥。一击离开 STARTUP/CHOP 后立即清任务并恢复控制，禁止 AI 自动走位、追击或连击。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。最终构建、暂存和安装 DLL 的 SHA256 均为 `56D91E3256D318F9C7F11AAEAD5CB37229BD896FA3D53CA0553D97C0F97CEE88`；个人 INI 保留，部署前 v63 DLL 已生成回滚备份。

## v63

- 复核 v62 设计：`CombatClass::weaponReach()` 是原版战斗计算的一个参数，不能证明它等于双方角色根节点之间的最终命中半径。继续用 `9.5+人工容差` 仍然是在猜原版攻击区，因此撤销该方案。
- 中立目标选择只决定“玩家想打谁”：身体投影/25°窄扇区可在有效武器资料长度内捕获目标，但不直接授权伤害。自由招式开始时同步原版 `attackStartPos` 并清除本帧攻击区缓存。
- 实际冲击帧直接调用原版 `CombatClass::isInAttackZone(target)`；只有原版攻击区返回 true 才进入原版 `hitByMeleeAttack`。日志记录 `native_zone`、中心距离、`weapon_reach`、`weapon_length` 和角色半径，便于核对原版判定。
- 敌对攻击链、一次性授权、WASD、受击和格挡逻辑均未修改。已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `8CFE5163972C1682DFDE7586A304DE38315059F12011FC9EF665F0AFB45DF1B8`；个人 INI 保留，部署前 v62 DLL 已生成回滚备份。

## v62

- 分析 v61 最新实战：全场 4 次 `attack_press_target captured=1`、0 次命中。一次明确目标在提交帧仅由 `9.54 > 9.50` 被永久清除，另两次在 impact 帧以 `9.8/9.9 > 9.5` 被插件先行拒绝；这是角色中心点逐帧采样的零容差错误，不是玩家未贴近。
- 中立提交与 impact 宽相位统一使用 `weaponReach()+max(0.75, reach*8%)`。以本场 9.50 reach 为例仅放宽到约 10.26；11.79、13.43 等仍拒绝，不把旧的全场/远距离命中带回来。
- 只修改中立自由挥刀的宽相位。敌对目标仍完整走原版 STARTUP/CHOP；命中候选最终仍调用原版 `hitByMeleeAttack`，格挡、闪避、伤害和阵营后果未被绕过。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。构建、暂存和安装 DLL 的 SHA256 均为 `A979B490C345D2CE6CFCFF860C62675D974D1E0A4E829D603148CADA0D74ED2B`；个人 INI 保留，部署前 v61 DLL 已生成回滚备份。
- 本版的人工 `max(0.75, 8%)` 容差在部署后经设计复核被否定并由 v63 撤销；不能作为当前距离规则。

## v61

- v60 实机确认任务入口已修复：`order=focused` 后成功出现 `attack_inject_native_hostile`，并由原版 hostile verdict 触发命中标记；此前的 `attack_native_entry_failed` 断点已经跨过。
- 新断点是注入后始终没有 `attack_done`：移动/静止保持仍把已注入原版挥刀当普通 WASD 帧清除 `combatModeActive`，令 `s_attackActive` 永不退出，后续输入全部进入队列并过期。
- 只在 `s_attackActive` 已成立时保留移动和静止保持中的战斗动画层；不包含待入场 commit，不恢复 v58 的 pending 桥接，不添加固定动作时间。既有攻击中 20% WASD 保持。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。构建、暂存和安装 DLL 的 SHA256 均为 `8477F770B2E608A7090523EC8258BFD9A06F1E831179F247846A19D2271D88E7`；个人 INI 保留，部署前 v60 DLL 已生成回滚备份。

## v60

- v59 实机否定“单向敌对导致任务类型错误”：首击后多次明确记录 `target_enemy=1 self_enemy=1 target_slave=1`，但普通敌对分支每次 `issued=1` 后仍无 `attack_job_created`、无 `attack_inject_native_hostile`，最终 `attack_native_entry_failed`。
- 确认失败点是敌对分支调用的 KenshiLib `Character::attackTarget` 没有建立原版攻击任务。移除该调用；双方敌对直接提交原版 `FOCUSED_MELEE_ATTACK`，主动攻击中立者继续提交 `UNPROVOKED_FOCUSED_MELEE_ATTACK`，统一通过已经挂钩并可观察的 `Character::addOrder` 入口。
- 不添加任何战斗状态、动画、WASD 或时间窗口补丁；v58 的强制战斗层方案仍保持撤回。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。构建、暂存和安装 DLL 的 SHA256 均为 `3317FEF8D2B1800B69CFC6C408874FF264DE7BAB747350F3AB478F63F3CCE5EB`；个人 INI 保留，部署前 v59 DLL 已生成回滚备份。

## v59

- v58 实机确认中立首击目标为奴隶：`enemy=0>1 target_slave=1>1`。后续每次左键都被接收，但 `attack_native_combat_bridge` 后仍无攻击任务、无 `attack_inject_native_hostile`，最终全部 `attack_native_entry_failed`；直接写 `combatModeActive` 的方案无效且会造成异常姿态。
- 完整撤回 v58 的战斗标志、移动减速和动作保护改动，只保留目标身份/关系诊断；此前受击、自由攻击、WASD、动画结束等路径恢复到 v57 行为。
- 曾判断敌对关系方向误用是主因；v59 实机随后证明双方已经互相敌对，普通 `attackTarget` 仍不创建任务。方向诊断保留，但敌对入口在 v60 改为明确的 `FOCUSED_MELEE_ATTACK` 原版任务。
- `attack_engage_native_target` 新增 `target_enemy`、`self_enemy`、`target_slave`，用于验证任务选择，不再推测或强制状态。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。构建、暂存和安装 DLL 的 SHA256 均为 `0CAA8CB08BD85FC010442AAA9B7912A1EEF1B66E0FA4C2873442760748C689D7`；个人 INI 保留，部署前 v58 DLL 已生成回滚备份。

## v58

- v57 实机确认中立命中链已经成功：`neutral_reticle dist=8.7`，实际 impact 为 `native_result=2 dist=9.2 reach=9.5`；随后目标被原版识别为 `native_hostile`，说明不是首击未生效。
- 曾把后续无法攻击归因于 `combatModeActive` 被清除，并增加战斗层桥接和移动保护；v58 实机证明此判断不完整：桥接后仍无攻击任务、仍然入口失败，且产生异常姿态。该方案已在 v59 完整撤回，不得恢复。
- `swing_impact` 新增 `enemy=前>后` 和 `target_slave=前>后`，下一场可以直接区分目标是否由首击变敌对、被击者是否为奴隶。旧 `dc_slave_state` 只描述受控玩家，不能用于判断受害者。
- 已通过 Visual C++ 2010 x64 编译并安全部署；仅见既有 Ogre/MyGUI C4819/C4091 警告。构建、暂存和安装 DLL 的 SHA256 均为 `023BB2801233CBC1563ADF0EA82F20484E1353642F07F3F05888508634978992`；个人 INI 保留，部署前 v57 DLL 已生成回滚备份。

## v57

- 分析 v56 实机：首次攻击确实以 `mode=neutral_reticle` 选中中立目标，提交距离 9.2、原版范围 9.5；但自由挥刀起手调用 `attackingYou` 后，冲击帧距离已变为 16.4，二次原版范围检查正确拒绝命中。后续四刀附近目标仍在 7.3～8.1，但方向鼠标划动已令目标偏离准星，释放时 `mode=none`。
- 中立意图改为 LMB 按下后的首个游戏线程帧捕获，发生在方向手势转动镜头之前；释放时绑定到本次攻击，缓存下一击时与方向一起保存。释放、更新列表、队伍保护和 impact `weaponReach()` 检查均保留。
- 中立自由挥刀不再在 impact 前调用 `attackingYou`，避免铁风自己提前驱动目标逃出范围。敌对目标仍使用原版 STARTUP/CHOP 的感知、走位、格挡和伤害链；中立伤害与阵营后果仍在真实 impact 调用原版 `hitByMeleeAttack`。
- 新增 `attack_press_target captured=...`、`attack_commit ... captured_target=...`、`attack_queued ... captured_target=...` 和 `free_attack_neutral_notice_deferred_to_impact` 诊断。已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `9776912C6DE60E5F2E6758C4A42E89F2B18D3AA0F898B33A489242500A93A2C0`。实际中立命中与负向边界待 v57 实机复测。

## v56

- 根据实机画面反馈修正左右格挡反向：攻击手势仍按刀刃运动定义（右移 `←` 为 `CUT_LEFT`），但右键格挡在进入原版防守姿势前交换 `CUT_LEFT/CUT_RIGHT`，因此鼠标右移选择右格挡、左移选择左格挡；上下映射保持不变。
- `s_blockDesired` 现在保存原版格挡技术和来袭 verdict 使用的真实防守方向；格挡 HUD 用同一对称转换恢复原手势箭头，保持右移显示右侧 `←`、左移显示左侧 `→`。
- `interact_rmb_block` 日志拆出 `gesture_dir` 与 `guard_dir`，下一次实机可直接核对输入、姿势和方向拦截。
- 已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `EF0F49E99FD5402C86EA164CD5BB46D9349848B25B1D79E067F057686B3F7A5A`。左右姿势和匹配/不匹配来袭仍待 v56 游戏内复测。

## v55

- 敌对攻击恢复原版能力：移除铁风对敌人的 `weaponReach()` 预筛、45° 目标扇区、22°/55° 冲击扇区、突刺 1 人/横扫 4 人限制和 `hostile_sweep` 手工伤害。准星/锁定只负责选择敌对目标，一次攻击随后进入原版 `STARTUP → CHOP_WEAPON`，由 Kenshi 决定攻击区、接触、多人目标、闪避、格挡、伤害、护甲与 XP。
- 中立攻击继续保留 v54 的身体投影、原版 reach 和 25° 窄扇区辅助，并在自由动画冲击帧调用原版 `hitByMeleeAttack`；玩家队伍仍在选择与 impact 两处排除。敌对/中立两条链互斥，避免重复伤害。
- 原版敌对挥刀没有固定动作时长；`manualAttackTrack` 只在原版离开 STARTUP/CHOP 后结束授权。空闲 AI 起手仍被拒绝，防止无输入自动连击。
- 原版敌对 verdict 现在也驱动红色命中 X：只有既非 `HIT_SWORD` 又非 `HIT_MISSED` 的实际结果才触发。
- 右键改为统一的“先格挡、后交互”：按下立即格挡，短按只格挡；仅非攻击/非闪避/非受击状态持续按住满 200 ms 且对准非敌对角色时，才撤销格挡并打开交互或对话。攻击途中按下的右键整次仅格挡，不会在动作结束后延迟弹框。
- 已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `146D718254CC136B906447EFC239F1A0640E85E393EB6B09C775AE8221C395FC`。敌对原版距离/格挡/多人命中、单次授权无自动追击连击，以及右键 0.2 秒边界均待游戏内复测。

## v54

- v53 日志确认距离已恢复正常：重武器 `reach=9.50`、`weapon_length=19.00`，中立人物实际距离 6.5～8.0；仍无法命中的原因是越肩近距投影横向错开 106～168 px，而非范围不足。
- 保留身体投影精确选择，增加原版 reach 内 25° 中立窄扇区兜底；只选正前方最近的非队友中立人物。敌对扇区保持 45°，玩家小队仍完全排除。
- `attack_pick` 新增 `neutral_arc` 模式及第三个候选计数，便于区分精确准星、中立视差兜底和敌对扇区。
- 已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `9E653A8621C47ECFD89D8FED6C760E11EF2846F1CBBC89319C841F70C3F2C885`。中立实际命中、旁观者保护和敌人格挡回归待游戏内复测。

## v53

- 修复 v52 无法命中正常站位中立角色。实机 7 次攻击全部 `hits=0 candidates=0`，且从未出现 `attack_aim_target`，证明原始武器长度不是世界坐标距离。
- 通过已有的受保护成员指针桥，直接调用原版 `CombatClass::weaponReach()`，由 Kenshi 完成武器长度到战斗世界范围的换算。目标选择、超距判断、招式筛选和命中共用该结果。
- `free_attack_start` 新增 `reach` 与 `weapon_length` 并列日志，下一次实战可直接验证换算值、稳定性和边界；ABI 异常安全失败仍保留。
- 保留敌人的原版攻击通知和 `hitByMeleeAttack` 格挡/伤害结算。已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `36F86388E9C69898A8A25E9D058295D84549E66842D58C4AC5BDED07D3F44028`。游戏内命中边界待复测。

## v52

- 修复 v51 的超大近战范围。实机日志证明直接读取的 `longestReachingAttack` 从 2269 增长到 4519，一次横扫甚至命中 563.2 单位外目标；它是当前二进制中失配的成员，不是武器距离。
- `manualOriginalMeleeReach` 改为调用原版 `CharStats::getCurrentWeaponLength()`；准星候选、提交前超距判断、招式筛选和命中扇区统一走该访问器。
- 新增 ABI 故障安全门：访问器返回非正数、非数值或超过 250 时记录 `weapon_reach_invalid` 并禁止本次命中，绝不退回巨大范围。250 不是玩法夹限，正常武器长度不会被裁剪。
- 保留 v51 的敌人原版攻击通知和最终原版格挡结算。已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `5A81F092B4636827EC9C60D98008B1BDE639A7F0CA447A5883070112733A629F`。实际武器长度及格挡行为待游戏内复测。

## v51

- 删除 V 模式实际命中距离的 24～36 人工夹限，但错误地直接读取了当前二进制中已失配的 `CharStats::longestReachingAttack` 成员；实机出现数千单位 reach，本方案已在 v52 撤销。
- 自由攻击起手把合法招式写入攻击者 `CombatClass::currentTechnique`，并对明确目标调用原版 `Character::attackingYou(..., doAwarenessCheck=true)`，使敌人在命中前进入原版感知与格挡决策入口。
- 攻击结束、右键取消或受击打断时，仅清理由铁风写入的当前招式指针。伤害、护甲、闪避、格挡结果与 XP 继续由原版 `hitByMeleeAttack` 处理。
- 已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存和安装 DLL 的 SHA256 均为 `C70E1E8A01343DA99AA095E6057CDFC0746B61115BA4653FC3A8F4CF229B98B9`。原版距离边界、敌人格挡频率/时机及 AI 不接管走位仍待游戏内复测。

## v50

- 保持 v49 的单选绿/白箭头规则、约 3 px 笔画和 64×64 图标尺寸。
- 实机反馈表明 108→148 px 的变化仍不够明显，因此将箭头视觉中心半径再增加 40 px 至 188 px。

## v49

- 澄清“4 个方向都有”：不是四个箭头常驻，而是任一时刻仍只显示当前选中的一个；上、下、左、右四个方向都能出现，不可用方向不能像 v47 那样消失。
- 当前所选方向可攻击时显示绿色，不可攻击或尚未确认时显示白色；格挡同样不使用蓝色。
- 箭头笔画由 v48 的约 2 px 调为约 3 px；统一视觉中心半径从 108 增到 148 px，较准星再外移 40 px。
- 代码与维护文档已同步，并通过 Visual C++ 2010 x64 编译和安全部署；构建、暂存、安装 DLL 的 SHA256 均为 `0AB58C46A0A3201B124FDF9F940FE70125FE82EFE8D27B7C7710A85D766DA23A`。游戏画面仍待实机验证。

## v48

- 方向 HUD 改为四向能力显示：四个向心箭头同时常驻，原生可用性缓存确认可攻击的方向为绿色，其余和未知方向为白色。
- 移除方向 HUD 的蓝色格挡态；格挡期间仍保持相同绿/白攻击能力显示。当前选向、缓存和无招式失败不再临时改色。
- 保持 64×64 外框、箭头长度和 108 px 中心半径，把运行时纹理的抗锯齿笔画从约 4 px 再收细到约 2 px。
- 分析 v47 最新战斗日志：2 次真实命中均由原版请求 `stumble=1` 并排队 `STARTUP → STUMBLE`，但分别约 1.27 s、0.83 s 后出现 `hit_native_reaction_not_entered`，全场没有稳定 `STUMBLE/stagger_start`；其中一次自由攻击已在原版反应前正确中断。由于本轮请求是日志分析且现有证据尚不能区分原版未选到反应招式与剩余钩子覆盖，本版不强制 STUMBLE、不添加固定时长，继续保留为待定位回归。
- 代码与维护文档已同步，并通过 Visual C++ 2010 x64 编译和安全部署；构建、暂存、安装 DLL 的 SHA256 均为 `4FF601CF9E814316B3BC596355A24F689B423F1A7C20F7BC2AE0C4A1A0093CBB`。游戏画面仍待实机验证。

## v47

- 攻击方向提示改为解锁可用性显示：只有 `s_dirMoveAvailable[slot] == 1` 的当前选向才绘制，并统一为白色。
- 当前武器、技能、单双手或距离条件下不可用的方向，以及尚未完成实时判定的方向，都完全隐藏；松开时仍安全失败，不会替换为其他刀路或锁定下一次输入。
- 下一击缓存方向不再用绿色，合法时同样显示白色。方向格挡保留蓝色，因为格挡招式与攻击可用性是两套判定。
- 代码与维护文档已同步，并通过 Visual C++ 2010 x64 编译和安全部署；构建、暂存、安装 DLL 的 SHA256 均为 `1E38573B082967D6074D98102FF7726E1DE349D4FF85E1BB7A8F6E812D6704DF`。游戏画面仍待实机验证。

## v46

- 复核 v45 最新实机：3 次有效受击均由原版判定 `stumble=1`，原版 STUMBLE 队列分别维持约 1.1～4.6 秒；期间 3 次新输入全部记录 `hit_reaction_input_dropped`，用户确认受击动作可以完整播放。没有恢复后补发攻击，v45 的受击所有权修正获得画面反馈和日志证据。
- 四向颜色规则保持不变：黄色普通攻击选向、蓝色格挡、绿色下一击缓存、红色当前武器/技能无对应合法招式；未选方向隐藏。
- 用户确认 v45 箭头大小和细笔画合适，但距离仍近。保持 64×64 外框和几何不变，把统一视觉中心半径从 92 外移到 108 px，并改用单一 `DIRECTION_ARROW_RADIUS_PX` 常量计算四个位置。
- 已通过 Visual C++ 2010 x64 编译并安全部署；构建、暂存、安装 DLL 的 SHA256 均为 `A6FF5634F7F9BC2039DE6192BEAB9FF8A94B233487086D9CDFB44DD09A6B185D`。108 px 距离仍待画面确认。

## v45

- 分析 v44 最新实机：9 次真实命中中 7 次 `_getHit(..., stumble=true)` 均留下 `STARTUP/nextMove=STUMBLE`，但 `combatGo_hook` 随即把所有 STARTUP 当成 AI 偷起手改成 WAIT，原版 `go()` 回到 DECISION，插件同帧记录 `hit_native_reaction_not_entered` 并解除输入锁。修正为反应所有权生效时保留原版 `STARTUP → STUMBLE`；AI 攻击起手拦截仍在非反应路径工作。
- 修复自由攻击途中一次真实命中漏掉 `_getHit`：样本在 `combatModeActive=0` 时只有 `hit_confirmed_pre_gethit`，随后一秒即可再攻击。现在真实命中判定返回原版调用者前恢复战斗标志，使原版受击调用链正常继续；是否硬直、持续多久仍完全由原版 `stumble` 和状态决定，没有固定计时。
- 方向箭头保持 64×64 外框、长度和头部尺寸，笔画约从 7 px 收细到 4 px，视觉中心半径从 78 px 外移到 92 px。
- 修复水平箭头与刀路相反：鼠标右移显示 `←` 时选择 `CUT_LEFT`（从右向左），鼠标左移显示 `→` 时选择 `CUT_RIGHT`（从左向右）；HUD 槽位、招式可用性与红显映射同步交换。
- 已通过 Visual C++ 2010 x64 编译并安全部署；最终构建、暂存、安装 DLL 的 SHA256 均为 `1010DFF413806E0D5C0CF97DA96F371168ABB6433B975967C701DDF978991C45`。启动与画面回归仍待实机。

## v44

- 分析 v43 最新实机：6 次 `HIT_FLESH` 全部开启旧 750 ms 窗口，但 0 次进入稳定 `STUMBLE`；第一次命中后窗口内仍出现 `attack_aim_begin`/`attack_release_commit`。因此短锁主要是铁风固定窗口，不能称为原版僵直时长。
- 删除 `HIT_REACTION_ENTRY_MS` 与 `s_hitReactionGoUntilMs`。新增经 KenshiLib `GetRealAddress` 安装的 `CombatClass::_getHit` 钩子，直接读取原版 `stumble` 参数：轻受击不加铁风时长，真实反应只在原版排队或实际处于 STUMBLE/REACTION_BLOCK 时拥有身体，状态退出当帧恢复。
- 原版请求 `stumble=true` 时只给予一次事件驱动的 `go()` 入场机会；若原版未进入或排队反应则立即交还控制，不使用毫秒兜底决定正常时长。
- 输入线程新增原子反应锁；原版反应期间的攻击、格挡和方向边沿直接记录 `hit_reaction_input_dropped` 并丢弃，不再在恢复帧补发。主线程攻击、格挡、闪避入口也使用同一原版状态门。
- 新增 `hit_confirmed_pre_gethit`、`hit_native_reaction`、`hit_native_reaction_state_end` 和 `_getHit hook OK/FAILED` 诊断，可直接核对原版 `stumble`、前后状态、nextMove 与锁定结果。
- v43 方向图集虽然日志显示创建成功，但实机完全不可见。v44 改为四张独立 64×64 运行时纹理，并采用已被武器光效验证的 `ImageBox::setImageTexture()` 绑定路径；几何和四色规则不变。
- 已通过 Visual C++ 2010 x64 编译；新钩子启动、受击时序与四向纹理画面待实机复测。

## v43

- 复核 v42 最新实机日志：7 次真实命中均有 `hit_reaction_window_open`/`hit_reaction_go`，玩家确认受击时无法 WASD 和攻击；但无稳定 `STUMBLE`。两次攻击中受击虽然取消了后续伤害，铁风自由攻击姿势仍分别延迟 744/761 ms 才清，造成一次“像没被打断”的观感。
- `iHitYouAreYouHit_hook` 现在利用 verdict 早于原版 `_getHit(..., stumble)` 的时序，在真实命中后立即结束铁风自己的自由动作层并清 `animationOverride`，随后才让原版安装 flinch/stumble；新增 `hit_free_action_interrupted before_vanilla_reaction`。
- 方向 HUD 从 `>`、`<`、`v`、`^` 字体字形改为运行时生成的 4×64 RGBA 图集。同一箭头几何旋转四次，四向线长、线宽、箭头头部和距准星半径一致，继续用控件染色表达攻击、格挡、缓存和无招式。
- 自由攻击冻结保护由“总时长 15 秒”补充为“实际时间轴连续 1.5 秒无任何进度变化”；正常慢动作只要持续推进就不会被固定时长截断。v42 的异常样本在 `progress=0.54` 停约 4 秒、总时长 6406 ms。
- 已通过 Visual C++ 2010 x64 编译；画面、受击时序与冻结保护待实机复测。

## v42

- v41 最新实机日志再次证实受击修复未生效：5 次 `hit_on_me`、5 次 `hit_vanilla_verdict result=2`，仍为 0 次 `stagger_start`。
- 找到 v41 遗漏的四个抑制点：空闲保持前后仍清 `combatModeActive`；WASD 在 STUMBLE 尚未进入时仍执行 `halt()+setDirectMovement()`；主循环后自由动作仍重播攻击；`combatGo_hook` 后部的空闲 return 和 movement return 仍会在入场窗口内跳过原版 go()。
- 确认命中入场窗口从 2 秒收窄为 750 ms；`manualHitReactionLive` 现在覆盖入场窗口及真实 STUMBLE/REACTION_BLOCK，移动、位置保持、自由动作以及手动攻击/格挡/闪避注入统一让路。
- 命中时清除自由攻击/闪避/格挡及下一击缓存的 bookkeeping，但不主动结束原生战斗动画层；反应结束后由原清理路径收尾。
- 新增 `hit_reaction_window_open`（含 state/combat/WASD/yielded）和按状态变化输出的 `hit_reaction_go`，下一场可以直接判断原版 go() 是否从命中状态进入 STUMBLE。
- AI 攻击 STARTUP/CHOP 拦截继续在窗口外层执行；待实机确认僵直画面、控制恢复以及 AI 不接管走位/自动连击。

## v41

- 修复 v40 僵直修复未生效（v40 实机日志：`hit_on_me` 13 次、`hit_vanilla_verdict result=2` 13 次、`stagger_start` 仍 0 条）。根因是鸡生蛋死锁：原版状态机进入 STUMBLE 需要 `combatModeActive=true`，而 v40 只在状态**已经是** STUMBLE/REACTION_BLOCK 时才保留该旗标——go() 要旗标才能进僵直，我们却要僵直才给旗标。v41 改为：反应窗口内两处旗标翻转点**无条件强制** `combatModeActive=true`；判定 wrapper 开窗瞬间直接置位（命中判定点原版可能同步做入场检查）。
- v40 结构隐患修正：go() 放行窗口内原本连 STARTUP 起手拦截一起被跳过（整块在 `!reactionWindow` 门内）。v41 把 STARTUP/CHOP → WAIT_MENACINGLY 改写移出门外，窗口内起手攻击仍被拦截，只有抑制 return 被跳过。
- `dir_move_avail` 日志去噪：内容不变时不再每 600 ms 刷一条（v40 场 386 条），改为仅变化时或每 5 秒打印。
- v40 实机证据入库：红显判据与实际提交完全一致（`choose=0110`＝右/下劈可用；dir=3、dir=1 出招，dir=2、dir=4 无招式），v38/v39 红显链路闭环。
- 待实机确认：攻击中/WASD 中被打出僵直（日志 `hit_on_me` 后应出现 `stagger_start`）；窗口内 AI 不偷起手、不接管走位。

## v40

- 修复“攻击或 WASD 移动中被敌人打中不出受击僵直”（实机 2026-10-20 + 日志证实：5 次 `glint_impact` 命中、全场 0 条 `stagger_start`）。根因：原版受击反应从 DECISION 进入 STUMBLE 的状态转移发生在 `CombatClass::go()` 内部，而 V 手动模式每帧抑制 go()；同时 WASD 驱动清掉 `combatModeActive`、两条移动注入路径持续 `halt()+setDirectMovement()`，把反应压死在进入之前。
- 修复方式（四点，全部以“确认命中”为门）：① `iHitYouAreYouHit_hook` 包一层 wrapper，判定结果既非格挡也非闪避时打开 `s_hitReactionGoUntilMs`（2 秒窗口，读档清理归零）；② 窗口内 `combatGo_hook` 放行原版状态机（STARTUP 拦截仍在，AI 仍不会偷起攻击），空闲钉住跳过；③ `combatModeActive` 清除逻辑在窗口内且状态已是 STUMBLE/REACTION_BLOCK 时保持不清，反应动画层不灭；④ 新增 `manualHitReactionLive()` 中央判定，`applyPlayerMovement` 与 charMovUpdate 缓冲分支在反应占用身体期间不注入移动，`halt()` 不再打断僵直；窗口超时自动失效，不会永久锁死移动。
- 自由挥刀的收招清理本就等待 STUMBLE/REACTION_BLOCK 结束，反应可叠加在挥刀上不被清掉。
- 待实机确认：攻击中被击中会出僵直、WASD 移动中被击中会出僵直、僵直期间输入丢弃（原策略）、僵直结束后移动/攻击恢复；日志应出现 `hit_on_me` 后跟随 `stagger_start`。

## v39

- 修复方向箭头闪烁（实机 2026-10-20，v38）：DirectInput 手势窗口每 180 ms 重启时把 `s_combatGestureDirection` 一并清空，空闲预览箭头在持续移动期间按窗口频率闪烁，且窗口刚重启时开始的划动会丢手势（日志表现为每次 `attack_aim_begin gesture=0`、`mouse_input dir=0` 但 `mouse_age` 很新）。现在窗口重启只清累计器，方向保留为下一窗口的种子；死区与对角线迟滞继续吸收抖动。
- 方向箭头再次放大：72×72 控件、字号 54（v38 的 40 仍嫌小），字形中心距准星 68→78 像素。
- v38 实时无招式红显在本场被招式池为空挡住：`attack_pool_unavailable count=0`（RVA 读取的静态攻击表为 0），可用性缓存原以 `s_dcAttackBuilt` 为前提整场未计算。现移除该前提：池为空时四个方向全部走 `chooseAttack` 采样判定——与提交路径实际使用的兜底一致。
- v38 实机证据入库：下劈路径 `attack_aim_begin/attack_release_commit dir=1` → `free_attack_start anim=Heavy big swing/bigchopv2` → `swing_summary` 全链路日志一致；池为空时 chooser 兜底 `attack_eligible unique=3/4 directional=1/2` 可出招。
- 待实机确认：持续右移箭头稳定不闪、54 号字大小合适且不遮挡准星、池为空时红显能出现并与实际可出招一致、按住后第一下划动能注册（`gesture=1`）。

## v38

- 无招式红显改为实时：新增每方向可用性缓存（`s_dirMoveAvailable`，游戏线程，含锚点/武器/持刀键与 600 ms 节流；池扫描 + `chooseAttack` 采样与提交路径同一判据），选向、缓存和空闲预览时该方向即显示红色；实际提交成功/失败会写入真实结果。未查明（-1）不显红，避免误报。
- 撤销 v32 的 `free_attack_no_technique` 后 1200 ms 红色覆盖与“No matching move”文本：失败不再占用 HUD、不再盖住新选向；下一次选向立即生效，输入路径本就未被该状态拦截。
- 方向保持更稳：`combatRecentDirection` 衰减 450→800 ms；按住左键后，本次按压内的第一下手势照常以 12 计数死区选中（`s_attackAimHoldLocked`），其后换向需当前手势窗口累计位移 ≥40 计数，回中/视角微漂不再在松开前翻转箭头。
- 方向箭头放大：控件 32×32→56×56，字号 24→40，字形中心仍距准星 68 像素。命中 X 保持 v36 的 28px/字号 24 不变。
- 待实机确认：无招式方向在按下瞬间变红且松开后无红色残留锁定；轻漂不换向、大幅移动可换向；箭头大小与位置不遮挡准星；`dir_move_avail` 日志与实际可出招方向一致。

## v37

- 准星本身可以变色：中立黄色、友方绿色、空处和红名仍为白色。
- 仍然不显示原版黄/绿箭头指针；有玩家对话包时才换成对话气泡。

## v36

- 菜单选项选中后立即结束 WASD 原地保持，并标为玩家点选，让搜刮/对话/跟随可以走过去。v35 日志已证明点击发出了 `LOOT_TARGET`，但脚被 hold 锁住。
- 菜单打开期间把真实光标位置灌进 MyGUI，避免一直点到屏幕中心的第一项。
- 准星不再被中立黄/绿原版箭头替换。只有 `talk`/`s_talk` 且目标确有 `EV_PLAYER_TALK_TO_ME` 时才显示对话气泡。
- 红色命中 X 字形仍为 28px/字号 24，视觉中心由约 16 像素收到约 8 像素；显示时长由 500 ms 延长到 1000 ms。
- 待实机确认：点菜单后人会走过去、中立保持准星、有对话包才出气泡、红 X 更近但不更大。

## v35

- 方向箭头不再四向常驻：只有鼠标实际选出的那个方向显示 `>` / `<` / `v` / `^`。
- V 模式交互改为右键。对准中立或友方打开原版菜单；红名敌人仍只格挡。
- 有玩家对话包的人右键直接进入原版 `EV_PLAYER_TALK_TO_ME` 对话，不再经过菜单。
- 取消待发攻击的右键仍优先走格挡，不会在瞄准挥刀时误开对话。
- 待实机确认：单箭头显示、中立/友方菜单、红名不交互、可对话者直接进对话。

## v34

- 根据 v33 实机反馈，确认 Kenshi 当前 HUD 字体没有显示 Unicode `→←↓↑`；改用已验证可显示的向心 ASCII 字形：左 `>`、右 `<`、上 `v`、下 `^`。
- 红色命中 X 的四个控件由距中心约 22 像素收近到约 16 像素，中央空隙由约 16 像素缩到约 4 像素。
- 命中 X 的显示/淡出时长由 220 ms 延长到 500 ms。
- 命中触发条件不变，仍只接受实际伤害结果；格挡、闪避、空挥和超距不亮。
- 待重新实机确认四个方向字形均出现，以及命中 X 的新距离和持续时间。

## v33

- 四向 HUD 改为全部朝向准星：左 `→`、右 `←`、上 `↓`、下 `↑`。
- 方向箭头中心距由约 33 像素增至约 68 像素，字号改为 24，未选方向提高亮度。
- 真正造成伤害时，在准星四个对角显示红色短斜线组成的断开 X，并在 220 ms 内淡出。
- 命中标记由 `hitByMeleeAttack` 返回值驱动；格挡 `HIT_SWORD`、闪避 `HIT_MISSED`、空挥和超距不会触发。
- 新增 `hit_marker_hud_created`、`hit_marker_hud_create_failed` 和 `hit_marker_trigger` 诊断；加载清理会取消未结束的提示。
- 已通过 VC++ 2010 x64 编译，箭头字形、位置、亮度及命中标记画面仍待实机验证。

## v32

- 方向模式改为按住左键持续选向、松开左键提交攻击；本版不加入人物蓄势动画。
- 选向期间按右键会取消待发攻击，并直接按当前方向进入格挡。
- 鼠标手势改为 180ms 短窗口、径向死区及对角线迟滞，降低镜头细微移动造成的方向闪烁。
- 招式选择不再静默退回错误方向；当前武器/技能没有对应动作时安全失败并显示红色提示。
- 当前挥刀或收招期间可以缓存一次下一击；缓存超过 1 秒、遭受硬直、退出 V 或加载时清除。
- 准星新增四向标记：黄色攻击、蓝色格挡、绿色缓存、红色无合法招式。
- 新增 `attack_aim_begin`、`attack_release_commit`、`attack_aim_cancel_to_guard`、`attack_queued`、`attack_queue_commit` 与 `attack_queue_expired` 诊断。
- 已通过 VC++ 2010 x64 编译，游戏内手感和画面仍待验证。

## v31

- 新增 UTF-8 `Settings.zh-CN.tsv`，汉化游戏设置页、悬停说明、按键列表、重置按钮及模组内状态提示。
- INI 注释改为中文，程序读取的英文节名和键名保持不变。
- 第一人称 FOV 的重置默认值改为 90°；用户本机配置可继续覆盖默认值。
- 启动日志同时显示铁风版本与上游基础版本，避免把 v31 误认成上游 1.5.0。
- 补齐维护文档、版本文件、测试矩阵、已知问题和安全部署脚本。

## v30

- 撤回无实际位移的 `attack_manual_move` 分支，恢复攻击期间 20% 的直接移动。
- 动作结束并原地保持时清理残留移动动画覆盖和战斗移动标记。
- 第一人称 FOV 配置由 75° 调至 90° 作为广角试用值。

## v29

- V 模式库存打开时释放鼠标，同时保留 WASD 控制和越肩距离。
- 第一人称视点前移量从 1.0 调到 0.0。
- 曾尝试攻击中手动位移；日志证明无效，已在 v30 撤回。

## v28

- V 模式打开库存时保留直接控制与越肩镜头；关闭库存后恢复原滚轮距离。
- 增加 `inventory_control_retained` 和 `inventory_control_restored` 诊断。

## v27

- 手动动作结束后清理模组动作层，避免攻击后留下异常战斗站姿。
- 真实受击硬直与招架反应仍优先完整播放。

## v26

- 按住格挡且没有 WASD 输入时固定位置；有输入时以 20% 上限移动。
- 攻击移动上限由 30% 调为 20%，新增 `combat_move` 与 `guard_stationary_hold`。

## v25

- 扩大 E 交互的角色身体投影和距离容差。
- 保留挥刀动画层；允许在命中判定后取消后摇并切入格挡。

## v24

- 识别原版 MyGUI 交互菜单按钮并调用选项处理函数。
- E 松开时若未指向选项，菜单保持打开供鼠标点击。
- 攻击移动上限由 15% 恢复为 30%。

## v23

- 在两个 WASD 入口及原生移动更新后限制攻击移动速度与水平动量。
- 维持方向格挡所需的战斗标志，增加 `attack_move` 诊断。

## v22

- 攻击移动上限一度降至 15%。
- 放宽方向格挡动画在战斗状态中的启动条件。
- 室内越肩距离临时限制为最多 35，跳过当前建筑已隐藏屋顶的碰撞。

## v21

- 攻击期间加入受限 WASD 移动；初始上限为 30%。
- Alt 的鼠标释放行为收窄到库存边走边操作场景。

## v20

- 越肩滚轮最远距离扩展到 120。
- 最近端自动进入第一人称，向外滚一次返回越肩视角。
- 加入室内墙体碰撞收近和离墙平滑拉远。

## v19

- 玩家队伍保护改为角色标记与花名册双重判断。
- 允许准星精确指向非队伍盟友时主动攻击。
- 初始越肩高度由 21 调为 18。

## v18

- 使用非独占 DirectInput 鼠标线程读取滚轮。
- E 菜单打开期间维持原版右键状态，并在释放时发送对应事件。

## v17

- 新增准星指向角色后长按 E 打开原版交互菜单的初始实现。

## v16

- 目标选择与伤害结算双重排除玩家队伍。
- V 模式压制战斗 AI 自主决策，但保留受击与格挡反应。
- 横扫支持近似轨迹多目标结算：突刺最多 1 人，其他挥砍最多 4 人。

## v15

- 修复读取空 `mainState` 导致的崩溃。
- 撤销直接修改 Ogre 时间轴的闪避尝试。
- 确认自由闪避当前缺少有效动画数据，记录 `free_dodge_no_animation_data`。

## v14

- 尝试维持翻滚片段，并在片段缺失时快速解除移动锁。
- 增加 `roll_clip_missing_cancel`；后续实测暴露空指针问题并由 v15 修复。

## v13

- 准星角色身体区域延伸到脚部并加入边缘容差。
- 伤害限制到武器近战触及范围。
- 移除翻滚固定时间收尾，增加动作时间轴诊断。

## v12

- 瞄准范围恢复为 40 游戏单位，并记录附近角色和最近距离。
- 尝试使用原版闪避播放函数，避免 Shift+WASD 回落为普通走路。

## v11

- 区分准星蓄意攻击、敌对扇区候选和空挥。
- 空挥不会自动选取中立或队友。

## v10

- 使用近距离镜头朝向锥选取角色，在命中帧直接调用受击接口。
- 翻滚结束判断改为参考动画长度和播放速度。
- 攻击招式在游戏筛选出的片段中轮换。

## v9

- V 入口直接请求拔刀，钩住人类角色收刀覆盖函数。
- 以 `weaponInHands` 判断真实持刀状态，避免未持刀时播放假空挥。

## v8

- 收窄目标到准星中心、无遮挡角色；超距时不发追击命令。
- 攻击结束清理模组下达的战斗命令，V 入口预设战斗姿态。

## v1–v7

早期原型阶段没有可靠、可逐版核对的发布记录。相关代码和日志只能作为历史参考，不能据此宣称功能已验证。

