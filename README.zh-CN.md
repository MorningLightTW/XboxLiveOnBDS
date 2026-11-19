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

lip 会把模组放进 `mods/`，没有 LeviLamina 的话也会一起装。之后用 `lip update` 更新。需要 lip 0.40.0 或更新的版本（`lip --version`）；旧版 lip 会说这个包的 `lip.toml` 无效。

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

## 让玩家加入

玩家和主机账号互加好友后，打开游戏的“好友”页面就会看到服务器。

加好友要你自己用主机账号操作，模组不会自动加。

## 查看连接状态

`/xbl ping` 显示自己的连接（P2P 或中继、延迟、丢包），不显示 IP；管理员可以加上 `<玩家>` 查看别人。

## 设置

不改也能用。配置文件在 `mods/xbox-live-on-bds/config/config.json`：

| 字段 | 默认 | 说明 |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | 谁看得到服务器。`"friendsOfFriends"`：好友，以及服务器内玩家的好友。`"friends"`：只有好友 |

要限制谁能进来，请用服务器的白名单（`allow-list`）。

## 常见问题

**玩家看不到服务器**：控制台要有 `Xbox Live session is up`，玩家也要和主机账号是好友。如果出现 `does not match the NetherNet internals`，请安装对应你 LeviLamina 版本的发行文件。

**服务器从好友页面消失**：主机账号一次只能显示一个世界。主机账号在别的设备上离开世界后，大约一分钟内服务器会回来。

**玩家看得到但进不来**：看启动时 `Friends' joins:` 开头的警告（通常是白名单），并确认游戏版本相同。

**玩家卡在“连接到多人游戏”**：同一台电脑开两台服务器时，`server-port`、`server-portv6` 和 `server-udp-ports` 都要不同。

**想换主机账号**：关闭服务器，删除 `mods/xbox-live-on-bds/data/auth.json`，再启动并重新登录。

**报告问题**：创建空文件 `mods/xbox-live-on-bds/data/debug.flag`，重启并让问题再发生一次，然后创建一个[问题报告](https://github.com/MorningLightTW/XboxLiveOnBDS/issues/new?template=bug.yml)，表单会说明要附什么。

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

第一次会下载不少文件，需要等一下。看到 `build ok` 后，成品在 `bin\xbox-live-on-bds\`，把这个文件夹复制到服务器的 `mods/` 即可。

更新旧版时，请保留 `mods/xbox-live-on-bds/data/`，就不用重新登录。

## 参考来源

Xbox Live 与 NetherNet 的运作方式微软没有公开文档。以下几个部分参考了这些项目：

- [df-mc/go-xsapi](https://github.com/df-mc/go-xsapi)：与 Xbox Live 实时服务（RTA）断线后重新连接、但不撤下 session 的做法，让服务器持续留在好友页。
- [df-mc/go-nethernet](https://github.com/df-mc/go-nethernet)：signaling 服务发放的中继（TURN）凭证及其有效期、等凭证到了才回应加入，以及好友加入时只使用这些中继服务器（[gophertunnel](https://github.com/Sandertv/gophertunnel) 也是如此）。
- [MCXboxBroadcast](https://github.com/MCXboxBroadcast/Broadcaster)：世界在 Xbox Live 上架时所用的 session 格式，以及把加入请求交给服务器 HTTP signaling 的方式（先查状态、只带 IPv4 candidate）；这条路径在本模组只是后备。它的设计与本模组相反：它先让服务器出现在列表，再把玩家转送过去；这里则是好友直接加入 BDS 本身。

## 许可证

[MIT](LICENSE)
