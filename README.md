# ESP-01 Relay Controller

這個 Arduino sketch 會讓 ESP-01 開啟 Wi-Fi AP：

- Wi-Fi 名稱：`HLC_ESP01`
- 手機連上後開瀏覽器：`http://192.168.4.1`
- 網頁可設定 ESP-01 要連線的外部 Wi-Fi SSID/密碼
- 網頁可手動開啟/關閉 relay
- 每次 relay 開啟最多 2 秒，時間到會自動關閉
- 可設定每天固定時間開啟 relay 1 到 2 秒
- 排程設定會保存，斷電後不會消失
- ESP-01 正常執行時，板載 LED 會一閃一閃

## 燒錄需求

Arduino IDE 需先安裝 ESP8266 board package，然後選：

- Board：`Generic ESP8266 Module`
- Flash Size：依 ESP-01 常見設定可先用 `1MB`
- Upload Speed：`115200`

## 使用方式

1. 將 `ESP01.ino` 用 Arduino IDE 開啟並燒錄到 ESP-01。
2. ESP-01 插回 relay 模組並上電。
3. 手機連上 Wi-Fi `HLC_ESP01`。
4. 瀏覽器開啟 `http://192.168.4.1`。
5. 在「Wi-Fi 設定」輸入現場 Wi-Fi 的 SSID 和密碼，按「儲存並連線 Wi-Fi」。
6. 頁面會顯示 ESP-01 是否已連上外部 Wi-Fi，以及取得的 IP。
7. 第一次打開頁面時會自動同步手機時間，之後可設定每天固定開啟時間。

ESP-01 連上外部 Wi-Fi 後，`HLC_ESP01` AP 仍會保留，方便之後再用 `192.168.4.1` 修改設定。

## 掃不到 HLC_ESP01 時

請依序檢查：

1. 燒錄完成後，USB 燒錄器如果有 `PROG/UART`、`FLASH/RUN` 或類似開關，請切回 `UART` / `RUN`。
2. 拔掉 ESP-01 電源再重新上電一次。若 GPIO0 還接到 GND，ESP-01 會停在燒錄模式，不會開 Wi-Fi。
3. 手機 Wi-Fi 頁面等 10 到 20 秒再重新整理，SSID 是 `HLC_ESP01`。
4. ESP-01 需要穩定 3.3V 供電，電流建議至少 500mA；供電不足時常見症狀就是 Wi-Fi 掃不到或一直重開機。
5. 若用 Arduino IDE 開 Serial Monitor，請設 `115200` baud。正常啟動會看到 `Starting HLC ESP01 Relay Controller`、`AP SSID: HLC_ESP01`、`AP IP: 192.168.4.1`。

## Relay 腳位設定

程式預設 ESP-01 relay 模組使用 `GPIO0` 控制 relay：

```cpp
const uint8_t RELAY_PIN = 0;
const bool RELAY_ACTIVE_LOW = true;
```

如果按「開啟」反而關閉，或按「關閉」反而開啟，請把：

```cpp
const bool RELAY_ACTIVE_LOW = true;
```

改成：

```cpp
const bool RELAY_ACTIVE_LOW = false;
```

## LED 閃爍設定

程式預設 ESP-01S 板載藍色 LED 是 `GPIO2`：

```cpp
const uint8_t LED_PIN = 2;
const bool LED_ACTIVE_LOW = true;
```

如果燒錄後程式正常但 LED 沒有閃，可能你的 ESP-01 板載 LED 是接在 `GPIO1`，可把 `LED_PIN` 改成 `1` 再試。

## 注意

ESP-01 沒有內建 RTC 時鐘電池。Wi-Fi 與排程設定會保留，但若重新開機，請用手機重新打開控制頁面一次，讓 ESP-01 重新同步目前時間，排程才會準確。
