# Murmur 与 Obsidian 同步

使用 Python 3.11 或更高版本。PC 只需标准库。Obsidian 可以关闭。首次同步从树莓派导入历史任务和随记，以后根据游标拉取变更。服务只遍历配置的 Murmur 目录，跳过隐藏目录、模板、来源、归档和冲突副本。

## 连接树莓派

已有掌机继续通过 8443 的 TLS-PSK 连接。PC 使用另一个 HTTPS 监听端口（默认 8444），在同一个服务进程内访问同一份 SQLite。部署时把 `server.py`、`bridge_api.py`、`cloud.py` 一起放到 Pi 的 backend 目录。升级前用 SQLite backup 备份数据库，保留旧程序。服务首次启动会迁移旧表，保留掌机整数 ID、原始录音、时间、任务状态，和已有的锁屏选择。新随记及没有选择记录的旧随记默认不入选。

给 PC 监听器提供与配置地址 SAN 匹配的证书和独立的随机令牌。例如在 Pi 上执行以下命令，将主机名和 IP 换成实际地址。已有部署应保留现有密钥，在需要轮换时再替换。

```sh
umask 077
cd ~/.config/murmur
openssl req -x509 -newkey rsa:3072 -nodes -days 730 -subj /CN=raspberrypi.local \
  -addext 'subjectAltName=DNS:raspberrypi.local,IP:192.168.1.20' \
  -keyout bridge.key -out bridge.pem
(printf 'MURMUR_PC_TOKEN='; openssl rand -hex 32
 printf 'MURMUR_PC_CERT=%s/bridge.pem\nMURMUR_PC_KEY=%s/bridge.key\n' "$PWD" "$PWD") > bridge.env
mkdir -p ~/.config/systemd/user/murmur.service.d
printf '[Service]\nEnvironmentFile=%s/bridge.env\n' "$PWD" > ~/.config/systemd/user/murmur.service.d/bridge.conf
systemctl --user daemon-reload
systemctl --user restart murmur
```

通过已验证主机身份的 SSH 安全复制公开证书和独立 PC 令牌到本机用户配置目录。服务器私钥留在 Pi。Windows 可在 `%LOCALAPPDATA%\Murmur` 中保存配置和仅当前用户可读的令牌文件。令牌文件只放令牌值。可使用系统信任的服务器证书并省略 `ca_file`。替换证书前同步更新 PC 信任的证书。

## Windows 启动

复制 config.example.json 到用户配置目录。填写 Vault 绝对路径、Vault 下的数据目录、Pi 的 HTTPS 地址、受信证书，以及 `token_env` 指定的用户环境变量。也可以删除 `token_env` 并填写 `"token_file": "C:/.../pc.token"`。状态默认保存在本机 LocalAppData 下按目录及服务器区分的子目录，可通过 `state_dir` 指定 Vault 外的本地目录。

在仓库根目录运行以下命令。将示例配置路径改为实际路径。

```powershell
$cfg = "$env:LOCALAPPDATA/Murmur/RaspberryPi/bridge.json"
python -m pc_bridge --config $cfg --dry-run
python -m pc_bridge --config $cfg --once
python -m pc_bridge --config $cfg --status
.\pc_bridge\install-task.ps1 -Config $cfg
Start-ScheduledTask -TaskName 'Murmur Obsidian Sync'
```

持续运行也可直接使用 `--daemon`。计划任务以当前用户身份在登录时启动，使用 pythonw.exe。用 `Stop-ScheduledTask` 暂停，`Unregister-ScheduledTask` 取消自启。变更配置后重启任务。进程锁阻止同一状态目录出现两个写入者。CLI 返回码 0 表示同步成功，1 表示错误或有待处理的冲突。第一次观测到文件消失只开始计时，即使宽限期为零，也需要再扫描一次。

默认每 60 秒扫描一次，网络失败后逐步延长到 15 分钟，然后自动重试。状态包含最近成功时间、最近错误、上次扫描的待发送数量、待应用的下载、异常路径和冲突副本。日志在状态目录轮转，每份上限 256 KiB，保留两份历史。备份 Vault 时也建议备份对应的状态目录。

## Hearth 3.9 设置

Windows 桌面端可将 `pc_bridge/obsidian/murmur-sync` 复制到 Vault 的 `.obsidian/plugins/`，并在第三方插件中启用 Murmur。左侧循环箭头 **同步到掌机** 会立即通知本机后台提交修改到树莓派，并显示回执。掌机下次联网同步时获取更新。手动同步会再次核对 Hearth 中已移除的任务行，归档并提交删除；对于删除的整篇随记，Windows 服务通过文件 ID 核实它已进入本 Vault 的 .trash 或系统回收站后，也会在这次手动同步时提交删除。去向不明的文件仍保留宽限期。

**删除与锁屏选择。** 在 Tasks 卡片中点任务文字打开快速预览，点底部垃圾桶「删除任务」并确认。Hearth 会移除原任务行；服务在删除宽限期后归档任务并回写树莓派。恢复原任务行可在宽限期内撤销。为避免归档和模板也出现任务，范围可以选 Tasks 文件夹，或排除 Murmur 下的 Archive、Conflicts、Templates。

主页的 Favorites 只列出已选随记的复选框和标题。取消勾选会将该项移出锁屏。点 **Favorites** 可以在「锁屏随记」页重新勾选，点随记标题可以读原文。两处复选框都直接编辑 `show_on_device`。要在另一个 Hearth 主页配置同样的入口，添加 **Embed** 卡片，目标选 `Murmur/Favorites.md`，把卡片标题留空，使用笔记内的可点击标题。

将仓库 `pc_bridge/obsidian/murmur-reading.css` 复制到 Vault 的 `.obsidian/snippets/`，在「设置 → 外观 → CSS 代码片段」启用 murmur-reading。从 Obsidian 社区安装并启用 [Banners Reloaded](https://github.com/dgcreations00/obsidian-banners-reloaded)，将同目录的「Favorites 页头.png」复制到 Vault 的 `Murmur/Assets/`。选择页使用这张从 Hearth 主页提取的装饰图，复选框由 Bases 提供。新录音的随记使用 DeepSeek 根据整条原话拟定的标题作为初始文件名。导入没有标题的旧记录时取正文开头，重名时附加序号。可直接重命名为自己喜欢的标题。同步 ID 和自动摘录保存在 YAML，平时阅读时隐藏，可在源码模式查看。

1. 打开 Hearth 主页，进入编辑模式，添加 **Tasks** 卡片。来源选择 **Markdown checkboxes**，范围选择 **Only these folders / 仅这些文件夹**，填写 `Murmur/Tasks`。启用显示已完成任务，按任务数量调整卡片显示条数。在列表直接勾选或取消即可写回原 Markdown。移动到其他目录的任务也需加入卡片范围。
2. 添加 **Note** 或 **Embed** 卡片，选择 `Murmur/Today.md`。列表包含已由掌机移入今天的任务，以及计划日、截止日任一个已到的未完成任务。条目链接到原任务，跨日由后台进程自动更新。也可直接嵌入 `Murmur/Dashboard.md`，同时查看今天和随记。
3. 在 Obsidian 核心插件中启用 **Bases**，新增 Hearth 的 **Embedded base** 卡片，选择 `Murmur/Murmur Memos.base`。在“全部随记”中打开文件阅读或切换 `show_on_device`，在“锁屏候选”视图查看已选项。若 Obsidian 未自动识别属性类型，在 Properties 将它设为 Checkbox。
4. 任务、随记的 `recording` 属性链接到 Recordings 下的来源笔记，包含原始录音名、录音时间和已有转写。手工记录不填来源。Templates 下有可复制到对应文件夹的空白起点。创建多个任务时每项使用一个文件。
5. 自定义数据目录时把上述路径换成配置值。服务写入首次使用的入口页及 Bases 文件。Hearth 卡片的添加和布局保存在用户选定的 Dashboard 中，由用户自行操作。

Hearth 的计划标记是 ⏳，到期标记是 📅。在测试 Vault 的原生 Tasks 卡片中核对了两个日期并实际勾选和取消。勾选时 3.9.0 会追加 ✅ 完成日期，解析器会将它从标题中排除。PC 保留服务器原始时分与偏移，编辑日期只替换日历日。使用 Windows 编辑器保存的任务会规范化为 LF，使 Hearth 可以读取。

## 文件、删除与冲突

- 文件带有服务端公开 ID。离线新建会先写入随机客户端 ID，服务端接受后沿用同一标识并发布 `murmur_id`。文件名可以重命名。手工复制新记录时清除两个 ID。用户新增的 YAML Properties 可以是列表或多行文本，服务只维护自己的顶层标量。
- 图片、附件和音频留在原处。录音来源页面首次生成后由用户保管。任务标题来自首行 checkbox，详情来自后续段落，随记取自正文。
- 自动确认删除需要两次完整扫描和 `delete_grace` 秒（默认 120 秒）。同一管理目录内的移动由 ID 匹配。本机 NTFS 还会按文件 ID 查找同卷移出的笔记，暂停删除并在状态中提示。Obsidian 的 .trash 或系统回收站属于删除。跨卷、部分网络文件系统以及第三方云盘的复制后删除操作只能按宽限期判断，跨目录整理时可先暂停服务。在删除宽限期内恢复文件即可撤销。
- 服务器删除的文件移入 Archive。恢复时复制回任务或随记目录并删除 ID。关闭锁屏展示只改变选择，原随记仍然存在。
- 不同字段的并发编辑自动合并。同字段冲突或删除与修改相遇时，Conflicts 保存本地内容或删除意图，正常路径反映服务器版本。核对后将需要保留的部分编辑回正常文件并删除冲突副本。服务器已删除的记录可按新记录恢复。
- 最后确认的版本、待发送的值、下载收件箱和游标存入本机 SQLite。待发送数据先落盘。Pi 使用单个事务检查基础修订并写入一批编辑，响应丢失时相同内容可安全重试。旧版本的独立修改会重新合并。Windows 原子替换时保留被替换文件直至核对完成，编辑器在替换瞬间抢先保存的内容可在 Conflicts 找回。
- PC 同步只处理 UTF-8 Markdown。每条出站编辑上限约 250 KB，超限、错误的 checkbox 或重复 ID 会出现在 status 中。修正文件后会自动重试。意外失去缓存目录或检测到服务端数据库回滚时保留数据并报错。

## 协议与测试

PC 端点均以 HTTPS POST 传输 JSON，使用独立 Bearer 凭据。changes 接受 `after`，返回服务器身份、最新版本和最多 64 条当前记录或墓碑。edit 接受带有 kind、uid、base 和 values 的编辑，或最多 32 条编辑的 edits 数组。values 为 null 表示删除。recording 按原录音名称读取转写。设备继续调用原来的 /v1/sync，并使用原有的摘录快照版本、每页 20 条和 TF 卡缓存。新固件额外提交本次修改的状态字段及操作编号，后台可以区分移动与完成，并去重设备重试。

```powershell
python -m unittest discover -s backend -v
python -m unittest -v pc_bridge.test_sync
python -m pc_bridge.benchmark --openssl 'C:/Program Files/Git/ucrt64/bin/openssl.exe' --idle 300
```

基准在 Windows 创建独立临时数据库、Vault 和环回 HTTPS 服务，生成 1,000 项任务和 2,000 条随记，再在新客户端进程上传 100 次勾选。每 10 秒执行一次空闲同步并采样工作集，CPU 按单核时间计算。服务端和数据生成在单独进程，测量不包含它们。具体实测结果见 TESTING.md。
