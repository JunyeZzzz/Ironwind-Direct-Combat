# 铁风执刃测试规范

测试目标不是“日志没有报错”，而是同时验证代码路径、画面行为和原版状态没有被破坏。

## 证据等级

| 等级 | 含义 | 可以声称什么 |
| --- | --- | --- |
| 静态 | 源码、配置、依赖、哈希检查 | 代码存在、配置一致 |
| 启动 | DLL 加载及所有预期钩子成功 | 插件在当前版本可挂载 |
| 日志 | 对应运行路径和关键数值出现 | 路径被执行 |
| 画面 | 玩家实际看到动作、镜头或 UI 正确 | 单项功能在该场景有效 |
| 回归 | 相关场景完整复测且无旧问题 | 可以标为已确认 |

只达到“静态／启动／日志”时，不得写“已修复”或“已完成”。

## 1. 修改前基线

1. 确认 Kenshi 未运行。
2. 记录 `git status --short`，不要覆盖不属于本次的修改。
3. 记录安装 DLL 的 SHA-256。
4. 保存当前 `RE_Kenshi_log.txt` 或确认它属于哪次会话。
5. 阅读 `KNOWN_ISSUES.md`，并按相关版本查询 `CHANGELOG.md`，确定本次不能顺带宣称解决的项目。

## 2. 静态检查

- `VERSION` 与源码 `IRONWIND_VERSION` 相同。
- `WASDCombatPlugin.ini` 中的节名和英文键名未变。
- `Settings.zh-CN.tsv` 每个非注释行恰有两列，英文键无重复、中文值非空。
- `RE_Kenshi.json` 只加载 `WASDCombatPlugin.dll`。
- `runtime/IronwindCombat.mod` 仍为无 FCS 记录的启动器占位真源；开发包只含 `IronwindCombatDev.mod`，正式包只含 `IronwindCombat.mod`。
- `data/mods.cfg` 只启用一个铁风通道，未启用 `WASDCombatPlugin.mod`；开发设置页标题带 `[DEV]`，正式包不带。
- `mods/IronwindCombatDev/` 是唯一的本地开发安装；正式版由 Workshop `3824431439` 提供，不保留同身份本地副本。
- 没有重新安装 `playerMove`、`showTradeWindow` 裸 RVA 钩子。
- 新增跨帧状态已在 `clearAllState`、V 退出和读档路径中处理。
- 新增缓存 Character 指针的使用点包含世界存活／更新列表验证。
- 攻击缓存目标、瞄准捕获目标和攻击活动标志仍各只有一个直接清理所有者；`tools/ironwind.ps1 Check` 的运行时状态所有权检查通过。

## 3. 构建与部署检查

1. 日常实测运行 `./tools/ironwind.ps1 DevFinish`；准备上传的正式候选才运行 `Finish`。
2. 编译必须无错误；警告需记录并判断是否新增。
3. DLL、PDB 和 `BUILD-STAMP.json` 必须同次生成且哈希互相匹配。
4. 开发通道的构建、开发暂存和开发安装 DLL SHA-256 必须一致；正式通道只要求构建与正式包一致。
5. `BUILD-MANIFEST.json` 必须记录版本、源码、DLL、PDB、构建指纹、Git HEAD 和 dirty 状态。
6. 同版本发布 ZIP 已含不同 DLL 时应在安装前拒绝覆盖。
7. 开发安装被替换时，`backup/` 应有不同旧 DLL 的回滚副本（相同哈希时可不重复备份）。
8. 实机前运行 `SessionStart`，退出后运行 `AnalyzeLog`；确认报告范围确实只覆盖本轮测试。
9. 动态报告里的“未知事件”不是错误；风险、未闭合链和时长离群必须结合上下文与画面解释。
10. 版本 ZIP 必须包含 DLL、INI、TSV、shader、RE_Kenshi.json、`IronwindCombat.mod`、`CHANNEL.json` 和文档，且不得包含 `IronwindCombatDev.mod`。

## 4. 启动检查

启动 Kenshi 后检查 `RE_Kenshi_log.txt`：

- 出现兼容启动标记 `Ironwind Combat vNN`；这是 DLL 内的诊断字符串，不是当前发布名称。
- 没有 `ANOTHER COPY of Direct Control`。
- 所有预期钩子为 `hook OK`。
- 新增的 `WASDCombatPlugin: _getHit hook OK` 必须出现；失败时不得验证受击时序。
- `WASDCombatPlugin: CombatClassAI update hook OK` 必须出现；失败时不得验证玩家直控 CHOP 的完整性。
- 没有 `hook FAILED`、`RVA hook SKIPPED` 或未捕获异常。
- 全场没有 `invariant_violation`；一旦出现，本轮不能宣称状态重构通过。
- 出现 `poll thread OK`。
- 退出游戏后出现 `WASDCombatPlugin: unloaded`。

任一核心钩子失败，本轮不得继续宣称玩法验证成功。

## 5. 游戏内回归矩阵

### A. 生命周期与控制

| 场景 | 画面预期 | 关键日志 |
| --- | --- | --- |
| V 开启 | 当前角色被跟踪、进入 OTS、可 WASD、持刀 | `dc_user_intent_on`、`manual_attack_on_with_dc`、`v_combat_stance_prearmed` |
| V 关闭 | 恢复原版镜头、输入和任务 | `dc_user_intent_off_manual`、镜头退出日志 |
| V 内外按 Z／X | 始终分别执行原版 1×／5×游戏速度；不切换手动战斗，不循环角色移动档位 | 不得出现铁风 Z/X 输入事件；原版速度 UI 正确变化 |
| G／双击切换角色 | 控制转移到新的玩家角色，无瞬移或残留移动 | 控制锚点切换日志 |
| 切换到非玩家角色 | 不得接管 | 无有效锚点切换 |
| 正常退出 | 不崩溃，不残留线程错误 | `unloaded` |
| Space 暂停／Esc 菜单 | 光标立即可见并自由移动；不再回中或隐藏。恢复游戏后只回中一次，控制正常 | 暂停期间 camera trace 应为 `ui=1`，恢复后为 `ui=0` |
| 四向方向保持 | 任一方向短划后，真实选择与箭头共同保持约 1.2 秒；保持期间改划其他方向应立即切换，不等待旧方向到期 | 箭头与 `attack_aim_begin`／实际刀路一致 |
| 中立目标真实命中 | 点击、失败初始化、挥空和格挡都不触发还手；肉体接触必须继续进入原版 `_getHit`、伤害、红 X 和受害者反击 | `neutral_origin=1`；必要时 `outgoing_hit_victim_combat_restored`；随后 `attack_victim_provoked source=confirmed_gethit` 与 `attack_native_gethit` |

### B. 移动

| 场景 | 画面预期 | 关键检查 |
| --- | --- | --- |
| 四方向和斜向移动 | 始终相对镜头，斜向不超速 | 速度与方向日志 |
| 步行／慢跑／奔跑 | 使用原版 `cycle_run_speed` 绑定循环，本机当前为 NUM6；铁风受控角色同步到同一档位 | 实际速度档位与原版面板一致 |
| 受伤或负重 | 不得绕过原版减速 | 与原版速度比较 |
| 镣铐 | 只能慢速移动 | `wasdMoveLimit` 结果 |
| 松开 WASD | 立即停步，无滑行和任务恢复 | `wasd_release_vector_zeroed` |
| 攻击中移动 | 约正常速度 20%，攻击动画仍可见 | `combat_move attack=1`、非零 `frame_dist` |
| 格挡中移动 | 约正常速度 20% | `combat_move block=1` |
| 格挡时松键 | 原地不走 | `guard_stationary_hold` |
| 攻击结束静止 | 自动恢复正确站姿，不需再按 WASD | `free_action_idle_pose_restored` 加画面确认 |

### B2. 手动／自动格挡模式

| 场景 | 画面预期 | 关键日志 |
| --- | --- | --- |
| 关闭“自动格挡” | 右键按鼠标方向出现对应姿势；请求已接受且仍按住时，来自可防御前方的匹配来袭立即可挡，即使姿势尚未完全抬起；不匹配不得误挡 | `block_mode mode=manual_directional`、`free_block_play`；匹配时只有 `directional_guard_block ... pose=0/1`，不得出现旧 `perfect_block` 或奖励蓝光 |
| 开启“自动格挡”并按住右键 | 右键立即成为防御请求，方向由原版 AI 按技能/概率选择，也允许失败受击；松开立即停止 | `block_press_accepted mode=automatic_ai`，来袭时可有 `ai_block_choice`；不得出现本次按压对应的 `free_block_play` 或 `directional_guard_block` |
| 新安装／本页恢复默认 | “自动格挡”复选框默认关闭，右键使用手动方向格挡；已有安装升级时不覆盖玩家已经保存的选择 | 默认 INI 为 `AiBlocksInManual=false`，设置行默认值为 0；手动请求出现 `block_mode mode=manual_directional` |
| 自动格挡空地按压 | 当前自然姿势完全不变，不因按压进入防御站姿／combat idle；仍保持原地且等待真实来袭 | `ai_guard_request armed=1 pose=unchanged`；不得出现 `ai_guard_ready_pose` 或 `block_hold vanilla_order=applied` |
| 自动模式不按右键 | 即使敌人来袭也不得自主格挡；受击和僵直仍走原版 | 可见 `manual_ai_block_denied` 或普通 `hit_on_me`，不得有 `ai_block_choice` |
| 自动格挡时移动／绕圈 | 仅“右键按住＋真实来袭”时 AI 可作原版防御；无请求时 WASD 仍归玩家，AI 不追击、不自动挥刀、不连击 | 可见原版 block/dodge verdict；不得出现未授权攻击提交或自动 CHOP |
| 跑步中按住右键格挡并继续按 WASD | 整个已接受格挡请求期间角色、镜头锚点与碰撞位置一起停住；可视人物不得沿原跑步方向脱离镜头；原版格挡／失败受击动作继续推进；松开右键后移动立即恢复 | 按住期间有节流后的 `guard_wasd_blocked` 与 `guard_stationary_hold`，不得有 `combat_move block=1`；不得出现镜头停住而人物继续跑；松开后下一次 WASD 正常注入 |
| BLOCK 被破／防御反应结束后仍按住右键 | 原版 REACTION_BLOCK/STUMBLE 完整；结束后回到普通持械防御待机，不停在格挡尾帧或怪姿势 | 反应期间输入仍按原版锁定；随后 `ai_guard_ready_pose neutral_combat_idle`，不得固定毫秒强退 |
| 自动格挡按住右键并 WASD（无真实 BLOCK） | 仍保持原地防御待机，不因 WASD 清掉下次自动格挡资格 | 有 `guard_wasd_blocked`；下一次来袭仍至多一次 `ai_block_attempt` |
| 自动格挡且敌人攻击 | 敌人手部／武器上不出现铁风自制发光；按住右键后的自动防御仍工作 | `attackingYou_hook` 仍建立独立来袭事件，可见 `incoming_attack_impact`、`ai_block_attempt` 或原版失败受击；不得出现 `dc_glow_tex`、`dc_glint`、`glint_created`、`glint_impact`。原版 `HIT_SWORD` 碰撞火花不属于本条删除范围 |
| 自动模式长按右键 | 无论时长都只维持自动防御请求，不打开菜单或对话 | `interact_rmb_block mode=automatic_ai ... social_armed=0`；不得有旧 `interact_rmb_hold_trigger` |

### C. 攻击与目标

| 场景 | 预期 |
| --- | --- |
| 空地左键 | 原地空挥，不追击旁人 |
| 空手四向空挥 | 不装备武器，以四向各提交多次；合法武术方向应见 `weapon_ready unarmed`、`dir_move_avail ... unarmed=1`、`attack_eligible ... unarmed=1` 和完整 `free_attack_start/done`，不得出现 `weapon_reach_invalid combat=0.00`。原版当前未提供的方向保持白色且安全不出招 |
| 训练设施左键 | 若非 Character 实体则空挥，不误选 NPC |
| 敌人准星攻击 | 记录一次原版攻击授权，由原版攻击区决定命中；不经铁风敌对范围/扇区手工伤害 |
| 中立准星命中 | 可蓄意攻击并可能引发派系敌对 |
| 中立首击后继续攻击 | 每次左键释放各记录一次 `attack_direct_native`；不得创建 focused/unprovoked 攻击任务。首击后无论敌对关系如何变化，下一击仍只能由下一次释放启动 |
| 首次攻击普通中立 | 起手应记录 `neutral_precontact_notice_deferred` 与 `attack_direct_native ... enemy=0`，此时不得有 `attack_victim_provoked`；只有真实 `_getHit` 后才记录 `attack_victim_provoked ... deferred=1`，本帧原版循环返回后记录 `attack_victim_response`。任务只能存在于受害者，玩家不得自动移动／连击 |
| 中立挥空／被格挡与盟友支援 | 挥空、闪避或 `HIT_SWORD` 后目标不得因起手通知单独敌对或还手；真实命中后 campaign/awareness 与受害者响应必须同一次提交。让一名实际同队或同阵营盟友留在近处，命中后由原版关系、感知、距离和 AI 决定是否支援；铁风不得直接给盟友下攻击任务 |
| 首击奴隶／守卫等特殊身份 | campaign 与 response 路径仍执行，但原版可以因身份规则拒绝或改为逃跑／呼救；不得用临时敌对记忆强制所有人攻击 | 依据 `victim_enemy/attacker_enemy/target_set` 与画面解释，不把 `target_set=0` 一律视为插件失败 |
| 断腿爬行／装死敌人 | 可被准星选中并尝试原版攻击；日志 `prone` 为 `PS_CRIPPLED`/`PS_PLAYING_DEAD` 时不得因 `unconscious=1` 被铁风过滤 |
| 真正 KO／死亡敌人 | 不作为攻击载体，不因本修复重新授权伤害 |
| 原版攻击同步入场 | 有效目标和方向应在释放帧记录 `attack_direct_native started=1 state_after=CHOP_WEAPON`；不得经历 DECISION/WAIT、TARGET_PATHFINDING 或 AI 入场计时。`started=0` 只允许无伤害空挥，不得延迟补刀 |
| 原版放弃入场后的恢复 | 若 inject 后原版未进入 CHOP并退出战斗，应出现 `attack_done ... swung=0 reason=combat_inactive_main` 和任务清理；下一次左键必须可重新提交，不能持续 `attack_queued/expired`。此失败回收不产生攻击恢复时间 |
| 敌对原版挥刀完成 | 启动先见 `combat_base_update_resolved`。`attack_direct_native started=1` 后应周期出现 `attack_native_advance`，其中 `combat_timer`、`combo` 或 `finished` 必须随原版动作变化，并按原版动作时长出现 `attack_done` 和 `attack_direct_control_restored`；动作中保持 20% WASD，动作结束后下一击可提交，不能长期 `attack_queued/expired`。每一主循环帧最多推进一次，不得运行 AI 走位/连击；攻击途中受击后必须立即停止攻击推进并让位给原版反应 |
| 非队伍盟友准星命中 | 只有明确对准时可攻击 |
| 玩家队友正前方 | 永不选中、永不结算伤害 |
| 模组招募队友 | 即使 `isPlayerCharacter` 异常也由花名册保护 |
| 敌对目标超距 | 原版拒绝命中；授权结束后不继续自动追击或补刀 |
| 中立目标超出原版攻击条件 | 可以播放一次原版攻击尝试但不得造成伤害；任务随后清除，不允许自动接近 |
| 敌对多人结果 | 完全服从原版招式与攻击区，不附加铁风最多人数或扇区 |
| 连续点击 | 每次有效点击一招，恢复期内不重复注入 |
| 多种武器 | 招式符合武器和技能资格，不播放防御动作当攻击 |
| 四向能力 HUD | 任一时刻只显示当前选中的一个向心箭头；上、下、左、右四个方向都能显示。绿色只表示武器/技能具备该方向动作，与有无目标和目标距离无关；其余/未知为白色，不得出现蓝色格挡态 |
| 按住左键移动鼠标 | 按住期间不攻击；所选箭头不因对角线轻微抖动闪烁；四向必须是同一几何旋转，线长、约 3 px 笔画、头部和 258 px 中心距一致 |
| 四向各重复 10 次 | 上、下、左、右的明确短划都应稳定选中对应箭头；v89 的 Y×1.25 只校正操作难度，不随机方向。统计 `attack_aim_begin` 核对定向测试命中率，不要求自然战斗分布机械地各 25% |
| 水平选向与刀路 | 鼠标右移选择右侧 `←`，实际从右向左；鼠标左移选择左侧 `→`，实际从左向右。无对应原版招式时仍显示白色箭头、攻击安全失败且不可反向替代 |
| 松开左键 | 只提交一次当前高亮方向的攻击 |
| 边缘绕圈 poke | 绿色方向在任意距离释放都应选择 `attack_direction_native_random` 并挥刀；远处目标允许 `contacts=0`，靠近后由原版产生 hit_candidate/blocked/dodged。未精确捕获但附近有敌人时应见 `attack_incidental_carrier` 或 `attack_incidental_carrier_anywhere`；不得出现目标距离拒绝、自动转向或 AI 自动靠近 |
| 出手后切换准星目标 | 姓名/血条可以切换到新的 UI 焦点；在途 AttackState 不动态换载体。刀路实际碰到新角色时仍可由原版产生 `_getHit`，未碰到则正常落空 |
| 中文／英文原版医疗面板 | 姓名下方只出现两行紧凑细长条：上排“血／头／胸／腹”，下排“左臂／右臂／左腿／右腿”。单条约 `38×5 px`，应明显长于 v118 的 `22×6 px` 色块，同时两行总高度更小、标签与条之间无多余空隙。有机健康必须为原版绿，骨人全身及机械义肢健康段为原版蓝；制造钝伤、未处理切割伤、包扎／机械修理、夹板和机械磨损，分别确认灰、红、黄、白覆盖和原版磨损段来自同一条内的真实比例，而不是整条按阈值换色 | 首次聚焦记录 `aim_health_part ... flesh=... stun=... bandage=... rig=... wear=... max=... robot=...`；28/29/30/31/32/100/101 ID 应解析正确。切换语言不影响识别，缺失部位不显示伪造空条，不得读取本地化显示名、合成总 HP 或重新引入健康阈值色 |
| 全关系 NPC 精确观察面板 | 依次用准星直接覆盖敌对、中立、友军、玩家队友及倒地角色的身体，均显示姓名与同一套两行八部位面板；重叠时只显示距离最近者，移开准星立即隐藏。姓名应为敌对红、友方绿、中立黄，医疗条保持原版伤势颜色。关闭“显示 NPC 名称与生命状态”后全部隐藏 | 进入／离开覆盖时出现 `bars_observed_target set/none`；全场始终只有一套面板，不得因已有锁敌／AI 目标而在准星移开后继续显示，也不得改变锁敌、E 交互候选、攻击目标、CombatClass 或敌对关系 |
| 原版 CHOP 完整性 | `attack_direct_native started=1` 后应以 `attack_native_finish_transition` 正常结束，或紧邻真实 `hit_on_me` 并以 `reason=hit_reaction*` 收权。若见 `attack_native_state_recovered`，必须至多一次、带 `source=combat_track/main_lifecycle` 且 progress<0.98，随后仍由原版完成；不得按固定时间重播。`attack_native_early_exit` 需保留为失败证据 |
| impact 后视觉收招 | `currentTechniqueHasFinished` 后先记录 `attack_native_impact_finished progress<0.98`；此后只允许 `attack_native_recovery_clip`，不得新增 verdict/gethit。最终必须以 `attack_native_finish_transition source=clip_finished` 和 `attack_done` 结束，不能永久停在 CHOP |
| impact 前按住右键 | 只记录一次 `attack_guard_wait_for_impact`，攻击的原版命中阶段继续；impact 后才允许开始交接 |
| impact 后按住右键 | 依次记录 `attack_guard_handoff accepted=1`、`attack_done`，随后下一主循环才出现 `free_block_play`；不得在 `s_attackActive=1` 时创建自由格挡，不得崩溃 |
| 命中与落空动画一致性 | 同一招式、相近速度下，`contacts>0` 与 `contacts=0` 都应完整播放同一 clip；除真实受击打断外，不能只有命中时动作完整 |
| 伤害路径 | `attack_native_verdict outcome=hit_candidate` 只是候选；只有后续 `attack_native_gethit` 才增加 `hits` 并出现 `hit_marker_trigger source=native_gethit`。伤害载荷应为有限非负值；无 `_getHit` 的候选不得亮红 X，也不得手工补伤害 |
| 选向时按右键 | 不出刀，立即取消并进入当前方向格挡 |
| 攻击／收招中再次选向并松开 | 只缓存一击；四向能力颜色不切换为缓存专用颜色，恢复结束后按缓存方向出刀 |
| 武器缺少所选方向 | 不播放其他方向，该方向保持白色且不结算伤害；下一次有效选向不受锁定 |
| 实际伤害命中 | 紧贴准星的四个对角出现红色短斜线，组成断开的 X，字形不放大，并约在 1000 ms 内淡出 |
| 空挥／超距 | 不显示红色命中 X |
| 被格挡／被闪避 | 不显示红色命中 X；格挡火花和闪避仍走原生结果 |
| 敌对原版范围 | 徒手、短刀、长柄和大型武器分别在边界内外攻击；目标攻击记录 `attack_direct_native`，命中资格由原版 AttackState 判断，不得出现铁风 `attack_target_out_of_reach` 或 `hostile_sweep` |
| 中立原版攻击区 | 中立目标在边界内外攻击；必须通过直接初始化的原版 AttackState 进入 CHOP，不得使用 AI 任务、direct impact、中心距离或 standalone `isInAttackZone()` |
| 距离 ABI 安全门 | 正常持械近战不得记录 `weapon_reach_invalid`；除 `SKILL_UNARMED` 合法零值外，一旦 `weaponReach()` 返回非正数、NaN 或超过 250，辅助选择必须安全关闭，绝不能退回巨大范围 |
| 空手中立目标 | 只有准星覆盖身体才可建立意图并尝试直接原版 AttackState；近处真实命中才触发红 X/敌对，超距必须由原版判空且不触发敌对。不得用 25° 中立扇区或自定义武术距离 |
| 敌人普通格挡 | 有敌对目标的挥击记录 `attack_direct_native started=1`；敌人按原版 AttackState 通知、技能、方向和时机决定格挡，不得自定义必挡 |
| 防御回归 | 连续攻击高低技能敌人，确认既有成功格挡也有正常命中；攻击通知不得导致玩家自动走位、自动连击或目标切换 |
| 越肩／第一人称精确瞄准中立目标 | 40 单位 AimRange 内准星覆盖身体时必须用 `neutral_reticle` 并见 `attack_press_target captured=1`，即使距离超过插件读取的 weapon reach；方向划动后提交 `captured_target=1` 并进入 `attack_direct_native`。是否接触、格挡、闪避或伤害仍只看后续原版 verdict；未覆盖身体时，正前 25° 的 `neutral_arc` 仍受选择 reach 限制 |
| 中立起手反应 | 记录 `neutral_precontact_notice_deferred` 与 `attack_direct_native started=1 enemy=0`；起手不得改变敌对关系。犯罪、敌对转换和目标反应只由 confirmed `_getHit` 后的完整世界提交产生，不得出现 focused 任务、手工 `free_attack_neutral_notice` 或 direct impact |
| 中立兜底安全边界 | 侧面超过 25°、超出原版 reach、倒地/死亡角色和玩家小队不得被中立扇区选中；不得因附近 NPC 多而误伤旁观者 |
| 原版多人命中 | 结果完全服从原版招式；每个真实未格挡 verdict 可刷新 X，但不得同时出现自由路径的第二次伤害 |

日志至少核对 `attack_aim_begin`、`attack_release_commit`、`attack_aim_cancel_to_guard`、`attack_queued`、`attack_queue_commit`、`attack_pick`、`attack_incidental_carrier_anywhere`、`attack_direct_native`、`attack_native_advance`、`attack_native_gethit`、`attack_done`、`attack_direct_control_restored`、`attack_eligible`、`free_attack_start`、`hit_marker_trigger` 和 `free_attack_done`。有目标链不得出现 focused/unprovoked 任务或 `swing_impact source=hostile_sweep`；`attack_native_advance_invalid` 不得持续出现。

### D. 格挡

| 场景 | 预期 |
| --- | --- |
| 四方向右键 | 鼠标右移显示右侧 `←` 并播放右格挡，左移显示左侧 `→` 并播放左格挡；上移为上挡、下移为突刺/下方挡。日志的 `gesture_dir` 与 `guard_dir` 只在左右方向互换 |
| 格挡期间方向 HUD | 当前所选箭头仍只按攻击可用性显示绿/白，不出现蓝色箭头 |
| 匹配方向来袭 | 持续格挡返回原生武器格挡 |
| 不匹配方向来袭 | 不得错误判为武器格挡 |
| 姿势入场期间的匹配来袭 | 只要有效 RMB 请求仍按住且方向匹配，即使日志为 `directional_guard_block ... pose=0` 也应走原版声音、火花和经验分支；不等待固定毫秒窗 |
| 连续多段来袭 | 每次命中都按当时仍有效的 RMB 请求、前方资格和方向匹配独立判定；不依赖额外固定时间窗 |
| 挥刀命中前右键 | 不取消攻击，不产生假命中 |
| 挥刀命中后右键 | 取消后摇并进入方向格挡 |
| 真实命中红 X | 每次 `_getHit` 只触发一次；四段位置不变，笔画较 v83 略粗、初始有轻微透明度并在约 1000 ms 内淡出，不遮住准星目标 |
| 中立目标近身 | v64 由原版 unprovoked STARTUP/CHOP 决定命中；铁风不记录或判断 `native_zone` |
| 中立目标位于原版攻击区外 | 不得隔空命中，也不得由任务接管移动自动靠近；本次尝试结束后立即恢复直接控制 |
| 攻击途中持续右键 | 只按命中前后规则请求格挡，绝不打开交互/对话；不得出现 `interact_menu_open` |
| 受击硬直期间新按或持续按住某方向右键 | 僵直期间不允许进入格挡；保存一次仍在物理按住的方向，原版 STUMBLE/REACTION_BLOCK 结束第一帧立即恢复格挡，不需要松开重按。期间若松开，恢复后不得补发旧格挡 | 依次出现 `reaction_guard_hold_preserved`、原版 `stagger_end`、`reaction_guard_rearmed` 和正常 `block_press_accepted`／`block_hold`；松开分支出现 `reaction_guard_hold_cancelled`，之后不得 rearmed |
| 空闲时被真实命中 | 日志依次出现 `hit_on_me`、`hit_confirmed_pre_gethit`、`hit_native_reaction`；`stumble=0` 只保留原版轻受击，`stumble=1` 才允许进入 STUMBLE |
| WASD 移动中被真实命中 | `stumble=1` 时立即停止直接移动并完整播放原版僵直；原版状态结束当帧恢复。`stumble=0` 不得出现固定 750 ms 锁脚 |
| 自由攻击中被真实命中 | 当前挥刀在原版受击动作安装前立即结束；即使命中前 `combatModeActive=0`，也应先见 `hit_reaction_combat_restored`，随后出现 `hit_confirmed_pre_gethit ... yielded=1` 和 `hit_native_reaction` |
| 原版反应中敌人持续施压 | 玩家 AI 不自行走位、不自动出刀、不连击；攻击键只记 `hit_reaction_input_dropped`，不得在恢复后补发 `attack_release_commit/free_attack_start`；只有仍按住的 RMB 可按上一行恢复 |
| 与切磋／第三方战斗控制器共同受击 | 若第三方没有继续调度玩家 `go()`，应只出现一次 `hit_reaction_fallback_advance`，随后进入原版 STUMBLE/REACTION_BLOCK 并按真实状态结束；正常原版调度应只有 `hit_reaction_go ... source=engine`，不得出现兜底、AI 走位或自动攻击 |
| 切磋中进入并退出 V | V 内受控角色只响应铁风手动输入，自动攻击保持抑制；退出 V 后由切磋插件在一次 `true → false` 边沿恢复自己的聚焦任务，角色重新自动切磋。铁风不恢复未知外部任务，整场不得出现周期性补单 |
| 受击倒地导致 V 自动退出后重新进入 | 退出只清铁风自己的 pending/输入锁，不截断原版倒地；角色可控后重新按 V，若已离开 STUMBLE/REACTION_BLOCK，首次左键必须正常进入 `attack_aim_begin`，不得继续 `hit_reaction_input_dropped` | `hit_reaction_lock_cleared_v_exit`；已恢复时可有 `hit_reaction_stale_lock_cleared_v_enter`，但不得残留丢键 |
| 格挡或闪避成功 | 不出现 `hit_confirmed_pre_gethit`，不产生假 `stagger_start` |

日志核对 `free_block_play`、`free_block_clip`、`directional_guard_block`、`attack_guard_cancel`、`hit_free_action_interrupted`、`hit_reaction_combat_restored`、`hit_confirmed_pre_gethit`、`hit_native_reaction`、`hit_reaction_fallback_advance`、`hit_reaction_input_dropped`、`reaction_guard_hold_preserved`、`reaction_guard_hold_cancelled`、`reaction_guard_rearmed`、`stagger_start` 和 `stagger_end`。新会话不得再出现 `perfect_block`、`block_press_reward` 或奖励蓝光。`stumble=1` 的 `after=3/8` 之后不得再立刻出现 `hit_native_reaction_not_entered`；应进入稳定 STUMBLE。`stumble=0` 时不得由铁风制造固定时长的输入锁。无第三方调度异常的普通战斗不得出现 fallback；同一 serial／同一帧不得补推进两次。

### D2. 动画冻结保护

- 正常长攻击只要 `free_attack_clip progress` 持续增加，就必须完整播放，不受 1.5 秒保护影响。
- 若进度在 0.05～0.98 之间连续 1.5 秒完全不变，应记录 `free_animation_watchdog stalled`，结束该动作并恢复输入。
- 暂停、打开菜单或读档不得制造错误伤害；恢复后若保护结束旧动作，不得补发迟到的 `swing_impact`。

### E. 主动闪避删除回归

v116 的预期是“不存在铁风主动闪避路径”：

- Shift+W/A/S/D 必须与普通 W/A/S/D 一样移动，不触发动作、冷却或输入锁。
- Q 不再注册为铁风闪避键；设置页与默认 INI 不出现 Dodge 项。
- 新会话不得产生 `dodge_*`、`free_dodge_*` 或 `roll_clip_missing_cancel` 等旧事件。
- 敌人仍可由原版返回 `HIT_MISSED`；自动格挡模式按住 RMB 时，原版 chooser 若自行选择 `isDodge`，仍可完成该次原生防御。二者都不得被误报为铁风主动闪避。

### F. 镜头

| 场景 | 预期 |
| --- | --- |
| OTS 鼠标观察 | 平滑转向，无翻转、跳变或系统光标 |
| H 换肩 | 横向偏移换侧，不改变距离 |
| 滚轮 3～120 | 距离正确保存 |
| 滚到最近 | 自动进入第一人称 |
| 第一人称向外滚 | 无论此前由滚轮还是 F 进入第一人称，都返回至少 6 的 OTS，并记录 `ots_zoom_exit_first_person` |
| F 切换 | OTS／第一人称可来回切换且不触发原版 `focus_char` 回中；F 进入后向外滚也能返回 OTS；P 不产生铁风输入，Q 仍可在键盘战斗配置下格挡；退出 V 后原版 F 恢复 |
| V 模式按 Ctrl | 不改变镜头、准星、WASD 锚点或战斗状态；不得退回原版俯视镜头。菜单中的 Ctrl+点击仍由原版处理 |
| 室内 | 目标距离最多 35，真实墙体仍阻挡 |
| 隐藏屋顶 | 不把镜头压到人物身边 |
| 离开墙体或室内 | 平滑恢复用户距离 |
| OTS 近／中／最远三档声音 | 同一角色脚步、挥刀、命中不应随视觉镜头拉远而消失；近处环境声仍按原版，不应被整体拉近；左右／前后仍按当前视角定位 | 启动同时出现 `audio listener anchor hook OK` 与 `character audio range hook OK`；V 开启有 `dc_audio_listener_anchor_on source=character_audio`，角色事件低频出现 `dc_audio_source_range_bypass` |
| 第一人称／普通 V 音频 | 两种 V 视角都以受控角色为听觉位置，切换角色后只跟随新角色，不产生爆音、旧位置残留或把旁人声音强行拉近 | 每次有效重新进入可见新的 listener 坐标；bypass 只来自当前受控角色 |
| 退出 V／读档／角色失效 | 下一次原版提交立即恢复镜头监听，菜单和读档期间不访问旧角色 | `dc_audio_listener_anchor_off`；无崩溃、无持续角色锚定 |
| 跑步切长按格挡／同时受击 | 远距镜头不得单帧拉到 4.5 再弹回。角色身边最小镜头泡内的向上表面接触可见 `camera ... floor_near=1`，但 `target` 应保持用户距离；真实墙体仍必须拉近 |
| 极端向下看 | 不应异常贴到头顶或让角色／准星关系突变；用 `camera ... dist/target/zoom/hit/normal_y/pitch` 区分轨道几何与碰撞拉近 |
| 第一人称身体 | 无头部遮挡，身体内侧可见，头盔／头发恢复正确 |
| 倒地／硬直 | 近裁剪不造成严重穿模或黑屏 |

### G. UI、库存与交互

| 场景 | 预期 |
| --- | --- |
| Tab 自有库存 | 自动释放鼠标，仍可 WASD；无输入时原地 |
| 关闭库存 | 恢复准星并保留镜头距离 |
| 玩家主动暂停后开库存 | 插件不得擅自解除主动暂停 |
| 搜刮／交易 | 窗口距离与关闭行为保持原版合理性 |
| 战斗层短按或长按右键 | 无论目标和时长都只格挡；不得打开菜单或对话 |
| 准星覆盖 30 单位内的中立／友方角色身体 | 胸口侧边出现带深色底、跟随位置的黄色 `E`；顶部战斗文字保持方向／格挡信息。同屏更近但未被准星覆盖的人不得抢提示，30 单位外不得提示 | `interact_target selected=1 type=1 ... miss_px=0.0` 后出现 `interaction_hint_visible type=1 x=... y=...` |
| 靠近活着的红名敌人 | 不显示 E 交互提示，不影响方向攻击；敌人 KO 或死亡后允许成为搜刮候选 |
| 准星覆盖 30 单位内的门、床、工作台／炮台投影范围 | 对象中心侧边出现同样的深色底黄色 `E`；普通墙体、装饰和无原版任务对象不提示 | `interact_target selected=1 type=0 ...` 与 `interaction_hint_visible type=0 ...`；不得生成虚构选项 |
| Kenshi 精确识别近距地面物品 | 允许显示 `E`；只有原版 `mouseRightTarget` 实际给出该物品时成立，不做全场物品扫描 | `interact_target selected=1 type=4 ...` |
| 有候选时按 E | 只打开一次该目标的原版上下文菜单，菜单稳定停留且世界暂停、光标释放；V 模式不退出 | `interact_menu_requested ...` 后 `interact_menu_open visible=1 orders=... pause_owned=1`；不得在约一帧内自动执行默认项 |
| 点击菜单中的对话／急救／搬运／搜刮／使用／开门等选项 | 由原版 MyGUI 回调执行；插件不得直接调用 `optionSelected`。普通世界任务选完后恢复时间、准星和战斗；若进入对话／库存／地图，暂停所有权交给原版 UI | 普通项为 `interact_menu_closed ...` 后 `interact_pause_released`；嵌套 UI 为 `interact_pause_transferred_to_native_ui`，随后原版任务按选项出现 |
| 玩家原本已经暂停 | E 不得认领或解除玩家自己的暂停；菜单关闭后仍保持原暂停 | 不得错误出现 `pause_owned=1` 或擅自 `interact_pause_released` |
| 候选没有任何原版菜单项 | 不暂停、不执行默认动作、不改变 V 模式 | `interact_menu_open_failed reason=no_native_options` |
| 无候选时按 E | 不弹框、不吸附远处人物、不改变战斗状态 | `interact_no_target` |
| 攻击、格挡、受击或已有 UI 时按 E | 不夺取动作或鼠标所有权 | `interact_mode_denied reason=committed_action/ui/context` |
| E 菜单可见时受到真实攻击 | 菜单关闭并完整进入原版受击反应 | `interact_menu_closed reason=hit`，随后正常 `hit_native_reaction` |
| 空格暂停中的 OTS／第一人称 | 世界人物和战斗保持暂停，系统光标释放；不得继续捕获鼠标、移动、攻击或推进任务。恢复后准星只回中一次 | 暂停时能正常使用菜单光标；恢复后无鼠标锁死或连续回中 |
| Esc、库存、地图、对话或 E 菜单 | 光标释放，鼠标移动和滚轮不得同时转动 V 镜头；关闭 UI 后恢复 |
| 中立无对话包 | 黄准星，不出现原版黄箭头 |
| 友方无对话包 | 绿准星，不出现原版绿箭头 |
| 空处或红名 | 白准星 |
| 有玩家对话包 | 自动候选的准星可换成对话气泡；按 E 打开其原版菜单后选择对话 |
| Esc／点击外部 | 关闭菜单 |
| 设置页 | 只显示 11 个铁风按键和 11 个常用设置；保留准星目标夹角、统一 NPC 姓名／生命状态、越肩高度、第一人称眼位高低／前后。不得重新出现独立友军状态开关，不得重复原版移动档位，也不得显示攻击／格挡毫秒、敌人武器发光、实验颜色或动作眼位等已删除／INI-only 参数；中文无乱码、无截断，“恢复本页默认设置”不覆盖高级 INI 值 |

### H. 读档与异常状态

| 场景 | 预期 |
| --- | --- |
| 区块流式加载 | 控制意图保留，不访问旧指针 |
| 快速读档 | 不崩溃；旧锚点清空后安全重获 |
| 普通读档 | 同上 |
| 新游戏 | 角色创建界面无残留 OTS／准星 |
| 导入存档 | 不访问上一世界角色 |
| 控制角色死亡／昏迷 | 停止非法移动和攻击，镜头不崩溃 |
| 大量敌人 | 无明显每帧日志洪水或移动钩子性能回退 |

## 6. 发布判定

发布前必须满足：

- 静态、构建、部署和启动检查全部通过。
- 本次修改涉及的场景达到“画面”。
- 相关邻接场景至少做一次回归。
- 自动发布报告保存构建／部署事实；本次结论明确标记证据等级。
- `KNOWN_ISSUES.md` 没有被无证据删除的问题。
- `CHANGELOG.md` 有本版本条目。
- 安装 DLL 与版本 ZIP 中 DLL 哈希一致。

如果无法启动游戏，只能交付“已编译／待游戏验证”，不能交付“已修复”。
