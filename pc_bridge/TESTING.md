# Windows 验收记录

2026-11-23，Windows x64，Python 3.11.9，Hearth 3.9.0。服务端为 Raspberry Pi 上的 Python 3.13.5。

## 自动化与集成

`python -m unittest discover -s backend -v` 的 3 个测试和 `python -m unittest -v pc_bridge.test_sync` 的 12 个测试全部通过。测试在临时 SQLite 和 Vault 中运行。覆盖首次导入、任务 CRUD 和双向完成、随记双向 CRUD、锁屏选择与快照分页、日期及原时分、跨目录移动、其他 YAML 属性、空闲文件读写、离线重启、批量提交、创建/编辑/删除的响应丢失、版本冲突、旧库迁移和 dry-run。

在独立 Obsidian 测试 Vault 中使用实际安装的 Hearth 插件，Tasks 卡片识别出标题、计划日和截止日。点击勾选后原文件出现 `[x]` 和 ✅ 日期，取消后恢复 `[ ]`。两次状态都经过 PC bridge、SQLite 和实际的设备同步处理函数验证。Bases 核心插件成功打开生成的 .base 文件并显示随记和布尔属性列。

正式 PC 使用单独的证书信任和 Bearer 令牌连接树莓派。错误证书信任及错误令牌分别被拒绝。正式 Vault 在首次同步后包含 2 项任务、1 条随记和 3 个来源页面。Obsidian 自身的索引也确认了 2 项任务和 1 条随记。Windows 登录计划任务已注册、启动并经过一次停止/重新启动，状态返回零待办、零错误、零冲突。实际 Windows 注销和重新登录未进行。

一次应用刷写后，COM5 和真实卡快照均通过隐藏 → 展示 → 隐藏验证，21 个原有 WAV 的 SHA-256 全部一致。详情见 [掌机实测记录](../firmware/TESTING.md)。本次通过串口与卡文件核验设备状态，未观察物理屏幕，也未在分页写入期间对设备断电。

后续 UI 回归使用 Hearth 3.9.0 的任务详情垃圾桶删除独立 Vault 中的一项任务，确认后文件仅剩 Properties 和详情。宽限期后检查了归档和设备接口中的移除结果。通过「锁屏随记」页内嵌的 Bases 复选框取消选择，也验证了随记仍在 SQLite、设备摘录已移除。新增自动化用例覆盖删除行、撤销、格式错误保护、重启和归档。

Favorites 展示回归覆盖短标题同名、重命名后的 ID、摘录更新和空闲重跑。Hearth 独立 Vault 中选中数依次为 1、2、1、0，清空后标题入口仍可用。正式主页已核对无工具栏和下划线、标题跳转、短标题及摘录；阅读模式中已核对隐藏的 YAML 键和可见的锁屏勾选项。

正式 Vault 的左侧「同步到掌机」按钮已通过实测。掌机离线时，按钮提交了临时任务的编辑，Obsidian 显示树莓派回执，存储卡保持原列表。掌机主动同步后，卡中出现更新的任务。随后在 Hearth 详情确认删除，点击按钮约 0.6 秒后收到成功回执，检查树莓派已移除该任务，Vault 中有归档。掌机再次主动同步后串口报告 HTTP 200，卡上恢复为原有的一项任务。临时归档已移至回收站。本轮 14 个 PC 测试及 3 个服务端测试通过。

## 性能

使用 README 中的 Windows benchmark 命令，Pi API 在独立进程的本机回环 HTTPS 上运行。单独的客户端进程加载已同步 3,000 条记录的 Vault，处理 100 个本地 checkbox 修改。脚本在最后验证服务端恰好有 100 项已完成任务。

| 指标 | 实测 |
| --- | ---: |
| 空 Vault 首次生成 1,000 项任务和 2,000 条随记 | 55.95 秒 |
| 100 项本地修改的增量同步 | 0.99 秒 |
| 增量同步进程峰值工作集 | 27.22 MiB |
| 已有 Markdown 在该次上传中的写入 | 0 次 |
| 空闲观测时长 | 302.4 秒 |
| 空闲工作集范围，30 次采样 | 27.35–27.54 MiB |
| 最后一次空闲工作集采样 | 27.54 MiB |
| 空闲 CPU，按一个核心计，每 10 秒执行同步 | 0.625% |
| 空闲期间 Markdown 写入 | 0 次 |
| 正式小 Vault 登录任务的空闲工作集，一次采样 | 25.21 MiB |

空闲测量包含周期扫描、TLS 游标检查与 SQLite 状态更新。Markdown 写入次数只统计客户端的 Vault 文件写入。5 分钟样本内工作集在很窄范围内波动。更长的观测可调整 `--idle` 参数。回环耗时没有包含校园网络延迟。

## 交付文件

| 路径 | 内容 |
| --- | --- |
| `pc_bridge/__init__.py`、`pc_bridge/__main__.py` | 模块入口、命令行、状态、单实例、退避、轮转日志 |
| `pc_bridge/sync.py`、`pc_bridge/markdown.py`、`pc_bridge/windows.py`、`pc_bridge/transport.py` | 增量与合并、Markdown、Windows 文件身份、HTTPS |
| `pc_bridge/config.example.json`、`pc_bridge/install-task.ps1` | 配置示例和当前用户登录任务 |
| `pc_bridge/templates/task.md`、`pc_bridge/templates/memo.md` | 新建记录模板 |
| `pc_bridge/templates/Dashboard.md`、`pc_bridge/templates/Murmur Memos.base`、`pc_bridge/templates/锁屏随记.md`、`pc_bridge/templates/Favorites.md`、`pc_bridge/obsidian/murmur-reading.css`、`pc_bridge/templates/README.md` | Vault 入口、随记和锁屏视图、操作说明 |
| `pc_bridge/test_sync.py`、`pc_bridge/benchmark.py` | 同步回归和 Windows 资源测量 |
| `pc_bridge/README.md`、`pc_bridge/TESTING.md` | 安装、Hearth 卡片配置和验收 |
| `backend/bridge_api.py`、`backend/server.py`、`backend/README.md` | 兼容迁移、修订游标、PC 接口和独立认证、设备过滤及编辑去重 |
| `firmware/src/pi_sync.cpp` | 持久化设备编辑的字段标记和操作编号 |
| `firmware/README.md`、`firmware/TESTING.md`、`docs/lockscreen-api.md` | 固件说明、实测日志和已部署协议 |

SQLite 备份、实际配置、证书私钥和令牌均保存在 Git 工作区之外。
