# XboxLiveOnBDS

[English](README.md) | [繁體中文](README.zh-TW.md) | **简体中文**

> [!WARNING]
> **使用风险自负。** 这不是 Microsoft 或 Mojang 提供的官方功能。
>
> - 模组是以游戏本身的身份登录 Xbox Live，主机账号有可能被限制或封禁。**请使用专用账号，不要用自己的主账号。**
> - `mods/xbox-live-on-bds/data/auth.json` 存有主机账号的登录凭证，而且没有加密。拿到这个文件的人就能使用该账号。不要外泄，分享或备份服务器文件夹前请先移除它。

为基岩版专用服务器（BDS）加上“好友主持世界”的功能：服务器会像好友主持的世界一样出现在“好友”页面，好友点一下就能加入。

## 需求

- Windows 上的 BDS 26.60.6 与 LeviLamina 26.60.8
- 一个 Xbox 账号作为主机账号，建议另外注册一个专用的

## 安装

### 用 lip 安装

先关闭服务器，在服务器文件夹里执行：

```
lip install github.com/MorningLightTW/XboxLiveOnBDS
```

需要 lip 0.40.0 以上。没有 LeviLamina 的话，lip 会一起装。用 `lip update` 更新。

### 手动安装

1. 关闭服务器。
2. 从 [Releases](https://github.com/MorningLightTW/XboxLiveOnBDS/releases) 下载 `XboxLiveOnBDS-server-windows-x64.zip`。
3. 把压缩包里的 `xbox-live-on-bds` 文件夹放进服务器的 `mods/`。
4. 启动服务器。

想自己构建模组文件，请看最下面的[自行构建](#自行构建)。

## 登录

第一次启动时，控制台会显示一个网址和一组代码：

```
Sign in the Xbox account this server will host as:
  open https://www.microsoft.com/link and enter the code ABCD1234
```

用浏览器打开网址，输入代码，以主机账号登录。看到下面这行就完成了：

```
Xbox Live session is up: ...
```

之后重启不用再登录。

代码约 15 分钟内有效。连续两组没用到之后，要在服务器控制台输入 `xbl login` 获取新代码。

## 让玩家加入

用主机账号和玩家互加好友（模组不会自动加），服务器就会出现在对方的“好友”页面。

## 查看连接状态

`/xbl ping` 显示自己的连接（P2P 或中继、延迟、丢包），不显示 IP；管理员可以加上 `<玩家>` 查看别人。

## 设置

不改也能用。配置文件在 `mods/xbox-live-on-bds/config/config.json`：

| 字段 | 默认 | 说明 |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | 谁看得到服务器<br>`"friends"`：只有好友<br>`"friendsOfFriends"`：再加上服务器内玩家的好友 |
| `playerCount` | `"includeHost"` | 好友页的人数<br>`"includeHost"`：玩家数 + 1（主机）<br>`"playersOnly"`：只算玩家（至少 1） |

要限制谁能进来，请用服务器的白名单（`allow-list`）。

## 常见问题

**玩家看不到服务器**：控制台要有 `Xbox Live session is up`，玩家也要是主机账号的好友。出现 `does not match the NetherNet internals` 或 `lacks NetherNet functions` 时，请安装对应你 LeviLamina 版本的发行文件（消息会列出版本）。

**服务器从好友页面消失**：主机账号一次只能显示一个世界。主机账号在别的设备上离开世界后，大约一分钟内服务器会回来。

**玩家看得到但进不来**：看启动时 `Friends' joins:` 开头的警告（通常是白名单），并确认游戏版本相同。

**玩家卡在“连接到多人游戏”**：同一台电脑开两台服务器时，`server-port`、`server-portv6` 和 `server-udp-ports` 都要不同。

**想换主机账号**：关闭服务器，删除 `mods/xbox-live-on-bds/data/auth.json`，再启动并重新登录。

**报告问题**：创建空文件 `mods/xbox-live-on-bds/data/debug.flag`，重启并让问题再发生一次，然后创建一个[问题报告](https://github.com/MorningLightTW/XboxLiveOnBDS/issues/new?template=bug.yml)。

## 自行构建

需要 Windows，并安装以下工具：

- [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/)（勾选“使用 C++ 的桌面开发”）
- [LLVM](https://github.com/llvm/llvm-project/releases)
- [xmake](https://xmake.io)
- [Git](https://git-scm.com)

在项目文件夹打开终端，执行：

```powershell
xmake f -y -p windows -a x64 -m release
xmake -y
```

看到 `build ok` 后，把 `bin\xbox-live-on-bds\` 复制到服务器的 `mods/`。更新时保留 `mods/xbox-live-on-bds/data/`，登录数据在那里。

## 参考来源

微软没有公开 Xbox Live 与 NetherNet 的文档。参考了：

- [df-mc/go-xsapi](https://github.com/df-mc/go-xsapi)：RTA 断线后重新连接，但不撤下 session。
- [df-mc/go-nethernet](https://github.com/df-mc/go-nethernet)：中继（TURN）凭证，以及好友加入时只用这些中继（[gophertunnel](https://github.com/Sandertv/gophertunnel) 也是）。
- [MCXboxBroadcast](https://github.com/MCXboxBroadcast/Broadcaster)：Xbox Live session 格式和 HTTP signaling 后备。它把玩家转送到别的服务器；这里是好友直接加入 BDS。

## 许可证

[MIT](LICENSE)
