# Cell

A small, portable Windows battery widget, built in native C++.

Double-click **CellBattery.exe**. No installation or extra runtime is required. Drag the title area to move it. Cell is one card that never changes size: approximately **276 × 444 pixels** at 100% Windows scaling (Medium); **… → Size** offers Small and Large. It follows Windows' light or dark app mode.

## Controls

- **Tabs:** Battery, Apps, Now and Week. Left and right arrow keys switch tabs.
- **Taskbar pill:** Cell shows its battery, percentage and time left in the taskbar, left of the tray (two lines on taller taskbars, one line on slim ones). Hover for the battery card; click to show or hide Cell; right-click for the menu. Turn it off with **… → Show in taskbar**. While the pill is shown, the tray keeps the plain Cell icon. If the taskbar can't host it (vertical taskbar, Explorer restarting), Cell retries every few seconds and the tray icon shows the percentage meanwhile.
- **Battery tab:** the ring shows the level and time left; beside it, the live draw with a 5-minute sparkline. Below: a time-left range from how much the draw moved recently, Health, change since unplugging (or plugging in), how long a full charge lasts at your usual use, and the 24-hour curve.
- **Week tab:** measured battery use per day, your usual draw on battery, and the 7-day level curve.
- **Alerts (… → Alerts):** off by default. Unplug reminder when charging passes 80%, low battery at 20%, and unusually high drain (more than twice your usual draw). They fire once on crossing, as quiet Windows notifications.
- **Pin beside …:** attach Cell to the desktop alongside your icons, behind normal app windows. Click again to return to a floating widget. The highlighted pin indicates that desktop attachment is active, and the position is locked while pinned so it can't be nudged by accident. Unpin to move it, then pin again.
- **On battery / Plugged in / All:** choose which Windows energy records to show. The chart and list follow the choice.
- **Now:** live view that needs no refresh: battery draw over the last 5 minutes and the apps using the most CPU over the last 30 seconds. It samples every 4 seconds only while the tab is open, and stores nothing.
- **Hourly bars:** hover to see an hour's share of the day; click to filter the app list to that hour. Click it again, or the small ×, to return to all 24 clock hours.
- **App rows:** each shows the app's energy in Wh and its share. Click an app to highlight its hourly energy on the chart and see how much of a full charge it used and where it went (CPU, screen, network…). Click it again, or the ×, to clear it.
- **Mouse wheel or page arrows:** browse the remaining apps, five at a time. The footer shows the page.
- **24-hour battery curve:** hover to read the time, level and power state of the nearest recorded sample.
- **↻ or F5:** refresh Windows' saved app-energy history. With hourly setup enabled, this requests the scheduled job without a prompt. Otherwise Windows asks for administrator consent for a temporary exporter; the widget itself runs as a normal user.
- **… or right-click:** pin to desktop, keep a floating widget above other windows, optionally start with Windows, open Windows battery settings, hide to tray, or exit. The tray menu also lets you unpin while other apps cover the desktop.
- **Tray icon:** shows the live battery percentage over a small level bar (red at 20% or below on battery) and follows the light or dark taskbar theme. Click it to restore the widget. Alt+F4 hides it; use **Exit Cell** to stop it completely.
- **Tab / Shift+Tab / Space:** navigate and activate native button controls. Escape hides the widget.

Desktop pinning, startup, and always-on-top are initially off on a fresh setup. Desktop pinning is remembered across runs; the widget returns to its floating mode when you unpin it. If Explorer is still loading, attachment retries briefly. If the desktop host is unavailable, Cell remains usable as a floating window. Rounded corners are retained in both modes; Windows supplies a shadow for the floating window.

Enabling startup adds only this executable to your own Windows startup registry entry. Disable it before moving or deleting the executable. Updating this executable in place preserves the existing desktop shortcut and startup entry.

## What the numbers mean

Battery percentage and charging state come from Windows. Visible readings refresh about every **two seconds**, with wattage shown to two decimal places. The battery firmware still controls how often its underlying readings change.

On battery, Cell collects a bounded, in-memory trend from the battery's remaining energy. After at least a minute and enough distinct capacity changes, wattage becomes a **recent 1–2 minute capacity-loss average**. This is useful on this HP laptop, where the Windows-reported rate disagreed substantially with the observed loss of stored charge. It is an estimate from the battery gauge, not an instantaneous electrical measurement. Allow 1–2 minutes for a workload or brightness change to settle into the average.

Remaining time prefers **Windows' own ETA** whenever available. Otherwise it uses recent drain, labelled **From the last 1–2 min of drain**. During warmup it falls back to Windows' reported discharge rate, labelled **From reported rate · refining…**; on devices that cannot provide reliable capacity observations the label is **From Windows' reported discharge rate**. Without either input, it says **Estimating**. The wattage sign shows direction: **+** while charging, **−** on battery.

While charging, **Full in …** is a linear estimate from the current charge rate and the remaining capacity; charging normally slows near full, so treat it as a lower bound. Plugged in below 100% without charging is shown as **Charging paused** (typically a charge limit or smart charging). **… → Battery reading details** explains the active readings and shows the Windows-reported rate separately.

The three tiles show **Health** (full-charge capacity as a share of design capacity), **Stored** energy in Wh, and **Cycles** (or full-charge capacity when the battery does not report a cycle count). Battery details are read briefly at start, on power-source changes and hourly; no handle stays open.

The recent trend resets on charging, rising capacity, missing readings, or a sampling gap. It is not persisted or reused after sleep. Direct capacity estimation requires one readable system battery reporting absolute energy units; other configurations retain the Windows reading. None of the estimates is a guarantee of remaining runtime.

The 24-hour battery curve records observations while Cell is running. Sleep, screen-off and closed-app gaps up to 6 hours are bridged by a faint dashed line so the trend stays readable; no samples are invented. **Green = on battery, amber = charging, blue = plugged in without charging.** History starts when you first run Cell. Sleep, screen-off, locked-session, and closed-app gaps stay blank; they are not assigned to an app or interpolated. Battery level is sampled at five-minute intervals, plus power-source changes, and retained for seven days.

The expanded panel uses the Energy Estimation data Windows already collects, exported by `powercfg /srumutil`. It shows the latest 24 clock-hour buckets in that export. Hour labels use your local Windows time; the current hour can be incomplete. These are the times Windows recorded each aggregate, not exact app start and stop times. Daylight-saving changes may repeat or skip a displayed clock hour.

App percentages are **shares of the reported estimated app energy for the selected source and time range**. They are not percentage points of battery charge lost. Windows' estimates do not account for every hardware drain and need not add up to the battery's actual capacity loss. Windows' unattributed energy is kept outside app shares and shown separately in the footer as **… not assigned to an app**; its percentage uses app plus unattributed estimates as its denominator. Hardware meter domains (`EMI_*`) are excluded because they are not apps and can overlap. Plugged-in app energy is separate from energy stored in the battery while charging. The hourly chart follows the selected filter: green for on battery, blue for plugged in, and, under **All**, grey for records whose power source is unknown.

**How much of your drain Windows explains.** Windows' per-app estimates often cover only part of what the battery really loses: the screen, idle platform power and sleep are largely unattributed. Cell therefore measures the real loss itself, by integrating the drop in the battery's remaining-energy counter while on battery, and keeps it per clock hour for 8 days (`drain.bin`). On the **On battery** chart each hour's outline is that measured loss and the filled bar is what Windows attributed; the line under the chart reads, for example, *Battery lost 7.1 Wh · Windows estimates cover 23% of it*. Measurement starts the first time 1.3 runs; loss during sleep or with the screen off is spread across the hours it spanned, and charging restarts the baseline instead of being counted.

App energy is shown in Wh (Windows reports millijoules; on the development laptop one on-battery stretch totalled 3.8 Wh in Windows' records against a measured drop of about 4.2 Wh, while another hour explained only 0.5 Wh of about 6.6 Wh). "% of charge" divides by the battery's current full-charge capacity. Several executables of one product, such as `steam.exe` and `steamwebhelper.exe`, are merged under one name. The footer shows the newest Windows record ("data through 16:00"); Windows writes these in hourly batches, so the current hour is usually missing until the next refresh.

The **Now** tab is a different measurement: share of CPU time, read from one system-wide snapshot (the same source Task Manager uses). CPU time is a good guide to what is keeping the processor busy, but it is not energy: the screen, GPU and radios are not included.

App icons come from the Windows shell, using the path or packaged-app id Windows records for each app, and are kept in a small `apps.bin` beside the cache. Apps without a resolvable icon get a coloured initial. Packaged names such as `Mozilla.Firefox` are shown as `Firefox`. Icons appear after the first refresh with this version; with hourly setup, re-run **Enable-hourly-history.cmd** once so the scheduled helper is updated too. Identical executable filenames in different paths remain separate identities.

App data refreshes **hourly after optional one-time setup**, and can also be requested on demand. Without hourly setup, refreshes are manual and require administrator consent. Windows may publish its aggregates late. The footer shows when the cache was refreshed. If an export is denied, unavailable, or malformed, Cell keeps the previous cache and explains the issue in **… → Show refresh details**. Battery display and local battery history continue to work. **Open Windows battery settings** is available when Windows cannot provide an export.

## Hourly app history

Double-click **Enable-hourly-history.cmd** once and approve the Windows administrator prompt. Setup copies a native helper into an administrator-protected Program Files folder and creates an hourly task for your Windows account. No password is stored. The task runs only while you are signed in, permits operation on battery, and does not wake the laptop. It catches up after missed runs when Windows allows it. The helper exports and compacts Windows history, then exits; there is no resident service.

Cell detects completed caches automatically, normally within a minute while visible or when it resumes. **↻** can request the same job without another administrator prompt. The initial export and a normal-user on-demand request were both verified on the development laptop.

The helper and task action are protected against modification by an unelevated process. The compact cache lives under ProgramData with read access for your account; the widget keeps its usual local copy. Raw exports are deleted after parsing. Re-run setup after updating the exporter version.

Use **Disable-hourly-history.cmd** to remove the task; that setup change also requires administrator approval. It retains the existing cache and helper files. To remove everything, also delete the CellBattery folders in Program Files and ProgramData with administrator permission after removing the task.

## Resource design

- One native UI process using Win32 and GDI+, with static native libraries and system DLLs.
- Desktop pinning uses the existing desktop shell and a hidden command window in the same process. It adds no service or worker thread. A bounded retry timer runs only while restoring a pin whose desktop host is not ready.
- A blocking Windows message loop; no render loop, animation, browser engine, managed runtime, process scans, WMI polling, or continuous tracing.
- Native battery/source/display notifications. A coalesced two-second timer refreshes visible readings; hiding to tray reduces this to five minutes.
- Sampling timers stop with the display off, session locked, or system suspended. A notification can still record a power-source change.
- Only changed wattage, ETA, or history regions repaint during steady operation. Fonts, the drawing buffer, and the icon are reused. The larger buffer is released when its dimensions change.
- The capacity estimator uses one cached battery-device handle and at most 80 in-memory observations. Device enumeration happens at startup or bounded retries, not every update. It uses no extra worker thread or child process.
- Windows efficiency throttling and low memory priority are requested. No artificial memory trimming or high-resolution timer requests.
- History is bounded, buffered, and saved at most once per 15 minutes during steady operation, plus source transitions, pause, and exit. Writes replace files atomically.
- An app-history refresh temporarily launches an exporter and Windows' `powercfg.exe`. They exit after completion. CSV parsing streams through a 16 KiB input buffer, aggregates at most 16,384 app/hour/source records, and deletes the raw export.
- The release never makes network requests. No accounts, telemetry, online assets, or resident background service. The optional hourly task uses Windows Task Scheduler.

See **VERIFICATION.md** for measured results and the limits of testing on this machine.

## Files and removal

Your observations and preferences stay in the **data** folder beside the executable. `history.bin` holds battery observations; `energy.bin` holds the compact Windows app-energy cache; `preferences.bin` holds position and display preferences; `refresh.status` holds the latest export error, if any.

To remove Cell, disable hourly history if enabled, turn off **Start with Windows** if enabled, choose **Exit Cell**, and delete its folder. The source and previews are optional; only the executable and its writable adjacent data folder are needed to run it. Keep the executable in a writable user folder.

## Build and verify

Source is in **source/**. The delivered 1.0.3 build uses the installed **MSVC C++ Build Tools and Windows SDK**, with the native runtime linked statically. No runtime installation is needed to run Cell.

```powershell
.\source\build-msvc.ps1 -BuildDirectory 'C:\path\to\scratch\Cell-build' -WithIcon
```

The alternative build script supports the official portable [Zig 0.15.2 compiler](https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip). The original release used this toolchain; the updated build was verified with MSVC. The compiler is only a build tool. Runtime notices are in **THIRD-PARTY-NOTICES.md** and **licenses/**.

```powershell
.\source\build.ps1 -ZigPath 'C:\path\to\zig.exe' -BuildDirectory 'C:\path\to\scratch\Cell-build'
```

The builds use size optimization, static native libraries, no exceptions or RTTI, and unused-code elimination. They embed an as-invoker and Per-Monitor-V2 DPI manifest. To embed the app icon after generating previews, rebuild with `-WithIcon`.

Optional developer modes (each exits after finishing):

```powershell
Start-Process .\CellBattery.exe -ArgumentList '--self-test "C:\path\to\test-output"' -Wait
Start-Process .\CellBattery.exe -ArgumentList '--desktop-check "C:\path\to\test-output"' -Wait
Start-Process .\CellBattery.exe -ArgumentList '--render "C:\path\to\previews"' -Wait
Start-Process .\CellBattery.exe -ArgumentList '--render-demo "C:\path\to\previews"' -Wait
Start-Process .\CellBattery.exe -ArgumentList '--import "C:\path\to\existing-srum-export.csv"' -Wait
Start-Process .\CellBattery.exe -ArgumentList '--quit' -Wait
```

Live previews render actual current data. **sample-** previews are explicitly labelled examples, generated only in the developer demo mode; production startup never loads sample data. Previews are rendered at twice the logical drawing size for readability. `--desktop-check` briefly attaches a hidden test window to the desktop, verifies its controller and detachment, and saves diagnostics without modifying your preferences.

## Windows references

- [System power status and change notifications](https://learn.microsoft.com/en-us/windows/win32/power/system-power-status)
- [Windows battery percentage and remaining-time fields](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status)
- [Remaining capacity, signed power draw, and battery ETA](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-system_battery_state)
- [Battery energy units and device capabilities](https://learn.microsoft.com/en-us/windows/win32/power/battery-information-str)
- [Reading battery-device status](https://learn.microsoft.com/en-us/windows/win32/power/ioctl-battery-query-status)
- [Coalescable timers](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcoalescabletimer)
- [Windows energy-history export](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/powercfg-command-line-options#srumutil)
- [Microsoft's explanation of Energy Estimation data and its limitations](https://devblogs.microsoft.com/sustainable-software/measuring-your-application-power-and-carbon-impact-part-1/)
