# XboxLiveOnBDS

[English](README.md) | **繁體中文** | [简体中文](README.zh-CN.md)

> [!WARNING]
> **使用風險自負。** 這不是 Microsoft 或 Mojang 提供的官方功能。
>
> - 模組是以遊戲本身的身分登入 Xbox Live，主機帳號有可能被限制或封鎖。**請使用專用帳號，不要用自己的主帳號。**
> - `mods/xbox-live-on-bds/data/auth.json` 存有主機帳號的登入憑證，而且沒有加密。拿到這個檔案的人就能使用該帳號。不要外流，分享或備份伺服器資料夾前請先移除它。

讓基岩版專用伺服器（BDS）出現在玩家遊戲的「好友」分頁，點一下就能加入。

**不需要公網 IP，也不用設定路由器。**

伺服器會以一個 Xbox 帳號（主機帳號）的身分開房，這個帳號的好友就能看到並加入伺服器。

## 需求

- Windows 上的 BDS 26.60.6 與 LeviLamina 26.60.7
- 一個 Xbox 帳號當主機帳號，建議另外申請一個專用的

## 安裝

### 用 lip 安裝

先關閉伺服器，在伺服器資料夾裡執行：

```
lip install github.com/MorningLightTW/XboxLiveOnBDS
```

lip 會把模組放進 `mods/`，沒有 LeviLamina 的話也會一起裝。之後用 `lip update` 更新。需要 lip 0.40.0 或更新的版本（`lip --version`）；舊版 lip 會說這個套件的 `lip.toml` 無效。

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

玩家和主機帳號互加好友後，打開遊戲的「好友」分頁就會看到伺服器。

加好友要你自己用主機帳號操作，模組不會自動加。

## 查看連線狀態

每個玩家都能用 `/xbl ping` 查看自己的連線：是不是從 Xbox Live 好友分頁進來、是 P2P 直連還是經微軟中繼，以及延遲、抖動和封包遺失。不會顯示 IP 位址。管理員（和伺服器主控台）可以用 `/xbl ping <玩家>` 查看別人。只有管理員能用這個用法，也只有管理員會在指令清單裡看到它。文字和指令清單裡的指令說明都會跟著玩家的遊戲語言顯示（英文、繁體中文、簡體中文；其他語言顯示英文）。翻譯檔在 `mods/xbox-live-on-bds/lang/`。

## 設定

不改也能用。設定檔在 `mods/xbox-live-on-bds/config/config.json`：

| 欄位 | 預設 | 說明 |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | 誰看得到伺服器。`"friendsOfFriends"`：好友，以及伺服器內玩家的好友。`"friends"`：只有好友 |

要限制誰能進來，請用伺服器的白名單（`allow-list`）。

## 常見問題

**玩家看不到伺服器**
確認主控台有 `Xbox Live session is up`，而且玩家已和主機帳號互加好友。剛加好友後可能要重開遊戲。

**主機帳號在別處玩時，伺服器從好友分頁消失**
主機帳號在好友分頁只能顯示一個世界。它在別台裝置進了其他世界時，伺服器會被藏起來，主控台也會出現警告。那台裝置離開世界後，大約一分鐘內伺服器會自己回來。最好不要拿主機帳號去玩。

**玩家看得到但進不來**
看啟動時 `Friends' joins:` 開頭的警告，通常是白名單開著。也請確認玩家的遊戲版本和伺服器相同。

**玩家卡在「連線到多人遊戲」**
同一台電腦開兩台伺服器時，每台的 `server.properties` 都要有自己的 `server-port`、`server-portv6` 和 `server-udp-ports`。UDP port 被佔用時，伺服器連不到玩家，卻不會說什麼；本模組會在啟動時發出 `Friends' joins: server-udp-ports=... is already in use` 警告。

**想換主機帳號**
關閉伺服器，刪除 `mods/xbox-live-on-bds/data/auth.json`，再啟動並重新登入。

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

第一次會下載不少檔案，需要等一下。看到 `build ok` 後，成品在 `bin\xbox-live-on-bds\`，把這個資料夾複製到伺服器的 `mods/` 即可。

更新舊版時，請保留 `mods/xbox-live-on-bds/data/`，就不用重新登入。

## 參考來源

Xbox Live 與 NetherNet 的運作方式微軟沒有公開文件。以下幾個部分參考了這些專案：

- [df-mc/go-xsapi](https://github.com/df-mc/go-xsapi)：與 Xbox Live 即時服務（RTA）斷線後重新連線、但不撤下 session 的做法，讓伺服器持續留在好友分頁。
- [df-mc/go-nethernet](https://github.com/df-mc/go-nethernet)：signaling 服務發放的中繼（TURN）憑證與其有效期限、等憑證到了才回應加入，以及好友加入時只使用這些中繼伺服器（[gophertunnel](https://github.com/Sandertv/gophertunnel) 也是如此）。
- [MCXboxBroadcast](https://github.com/MCXboxBroadcast/Broadcaster)：世界在 Xbox Live 上架時所用的 session 格式，以及把加入請求交給伺服器 HTTP signaling 的方式（先查狀態、只帶 IPv4 candidate）；這條路徑在本模組只是後備。它的設計與本模組相反：它先讓伺服器出現在列表，再把玩家轉送過去；這裡則是好友直接加入 BDS 本身。

## 授權

[MIT](LICENSE)
