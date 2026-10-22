# 当前状态（兼容入口）

本文件不再手工复制逐版本哈希和测试状态，以免和实际产物漂移。

- 当前版本：`VERSION`
- 当前未解决项：`KNOWN_ISSUES.md`
- 历史变化：`CHANGELOG.md`
- 构建事实：`build/BUILD-STAMP.json`
- 部署事实：`package/IronwindCombat/BUILD-MANIFEST.json`
- 发布报告：`reports/releases/`

查看实时状态：

```powershell
.\tools\ironwind.ps1 Status
.\tools\ironwind.ps1 Verify
```

截至 v120，近战主链已经进入细节打磨阶段；弩与主动闪避仍属于后续独立设计，不得从旧代码恢复半成品。
