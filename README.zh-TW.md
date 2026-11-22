# XboxLiveOnBDS

[English](README.md) | **繁體中文** | [简体中文](README.zh-CN.md)

> [!WARNING]
> **使用風險自負。** 這不是 Microsoft 或 Mojang 提供的官方功能。
>
> - 模組是以遊戲本身的身分登入 Xbox Live，主機帳號有可能被限制或封鎖。**請使用專用帳號，不要用自己的主帳號。**
> - `mods/xbox-live-on-bds/data/auth.json` 存有主機帳號的登入憑證，而且沒有加密。拿到這個檔案的人就能使用該帳號。不要外流，分享或備份伺服器資料夾前請先移除它。

為基岩版專用伺服器（BDS）加上「好友主持世界」的功能：伺服器會像好友主持的世界一樣出現在「好友」分頁，好友點一下就能加入。

## 需求

- Windows 上的 BDS 26.60.6 與 LeviLamina 26.60.8
- 一個 Xbox 帳號當主機帳號，建議另外申請一個專用的

## 安裝

### 用 lip 安裝

先關閉伺服器，在伺服器資料夾裡執行：

```
lip install github.com/MorningLightTW/XboxLiveOnBDS
```

需要 lip 0.40.0 以上。沒有 LeviLamina 的話，lip 會一起裝。用 `lip update` 更新。

### 手動安裝

1. 關閉伺服器。
2. 從 [Releases](https://github.com/MorningLightTW/XboxLiveOnBDS/releases) 下載 `XboxLiveOnBDS-server-windows-x64.zip`。
3. 把壓縮檔裡的 `xbox-live-on-bds` 資料夾放進伺服器的 `mods/`。
4. 啟動伺服器。

想自己建置模組檔案，請看最下面的[自行建置](#自行建置)。

## 登入

第一次啟動時，主控台會顯示一個網址和一組代碼：

```
Sign in the Xbox account this server will host as:
  open https://www.microsoft.com/link and enter the code ABCD1234
```

用瀏覽器打開網址，輸入代碼，以主機帳號登入。看到下面這行就完成了：

```
Xbox Live session is up: ...
```

之後重啟不用再登入。

## 讓玩家加入

用主機帳號和玩家互加好友（模組不會自動加），伺服器就會出現在對方的「好友」分頁。

## 查看連線狀態

`/xbl ping` 顯示自己的連線（P2P 或中繼、延遲、封包遺失），不顯示 IP；管理員可以加上 `<玩家>` 查看別人。

## 設定

不改也能用。設定檔在 `mods/xbox-live-on-bds/config/config.json`：

| 欄位 | 預設 | 說明 |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | 誰看得到伺服器<br>`"friends"`：只有好友<br>`"friendsOfFriends"`：再加上伺服器內玩家的好友 |
| `playerCount` | `"includeHost"` | 好友分頁的人數<br>`"includeHost"`：玩家數 + 1（主機）<br>`"playersOnly"`：只算玩家（至少 1） |

要限制誰能進來，請用伺服器的白名單（`allow-list`）。

## 常見問題

**玩家看不到伺服器**：主控台要有 `Xbox Live session is up`，玩家也要是主機帳號的好友。出現 `does not match the NetherNet internals` 或 `lacks NetherNet functions` 時，請安裝對應你 LeviLamina 版本的發行檔（訊息會列出版本）。

**伺服器從好友分頁消失**：主機帳號一次只能顯示一個世界。主機帳號在別台裝置離開世界後，大約一分鐘內伺服器會回來。

**玩家看得到但進不來**：看啟動時 `Friends' joins:` 開頭的警告（通常是白名單），並確認遊戲版本相同。

**玩家卡在「連線到多人遊戲」**：同一台電腦開兩台伺服器時，`server-port`、`server-portv6` 和 `server-udp-ports` 都要不同。

**想換主機帳號**：關閉伺服器，刪除 `mods/xbox-live-on-bds/data/auth.json`，再啟動並重新登入。

**回報問題**：建立空檔案 `mods/xbox-live-on-bds/data/debug.flag`，重啟並讓問題再發生一次，然後開一個[問題回報](https://github.com/MorningLightTW/XboxLiveOnBDS/issues/new?template=bug.yml)。

## 自行建置

需要 Windows，並安裝以下工具：

- [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/)（勾選「使用 C++ 的桌面開發」）
- [LLVM](https://github.com/llvm/llvm-project/releases)
- [xmake](https://xmake.io)
- [Git](https://git-scm.com)

在專案資料夾開啟終端機，執行：

```powershell
xmake f -y -p windows -a x64 -m release
xmake -y
```

看到 `build ok` 後，把 `bin\xbox-live-on-bds\` 複製到伺服器的 `mods/`。更新時保留 `mods/xbox-live-on-bds/data/`，登入資料在那裡。

## 參考來源

微軟沒有公開 Xbox Live 與 NetherNet 的文件。參考了：

- [df-mc/go-xsapi](https://github.com/df-mc/go-xsapi)：RTA 斷線後重新連線，但不撤下 session。
- [df-mc/go-nethernet](https://github.com/df-mc/go-nethernet)：中繼（TURN）憑證，以及好友加入時只用這些中繼（[gophertunnel](https://github.com/Sandertv/gophertunnel) 也是）。
- [MCXboxBroadcast](https://github.com/MCXboxBroadcast/Broadcaster)：Xbox Live session 格式和 HTTP signaling 後備。它把玩家轉送到別的伺服器；這裡是好友直接加入 BDS。

## 授權

[MIT](LICENSE)
