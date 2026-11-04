# Mem Reduct 白名单版

[Mem Reduct](https://github.com/henrypp/memreduct) 的二改版本：按目录白名单定向清理内存，白名单内的程序（游戏、常驻工具）工作集不动，其余照常清理。

> 本项目是对原作者 [Henry++](https://github.com/henrypp) 的 [memreduct](https://github.com/henrypp/memreduct) 的二次修改，非官方版本。原项目版权与许可（GPL-3.0）完整保留，详见 `LICENSE`。

## 下载

不用编译，直接取现成 exe：[Releases](https://github.com/AIN-sch/memreduct-whitelist/releases/latest)

- `memreduct-whitelist-3.5.3.exe`：单文件，免安装，无运行库依赖
- `whitelist.txt`：下载后重命名为 `白名单.txt`，与 exe 放同一目录

## 改动点

原版清理工作集是全局一次 `NtSetSystemInformation(SystemMemoryListInformation, MemoryEmptyWorkingSets)`，全系统进程一起压，游戏纹理被驱逐、切回就卡。

本版改动：

- 改为逐进程处理：枚举进程 → 命中白名单直接跳过 → 其余进程压工作集
- 白名单来自 exe 同目录的 `白名单.txt`，支持目录前缀、完整 exe 路径、环境变量、子进程继承
- 清理优先走 `ProcessWorkingSetControl` + COMPRESS|STORE（压进内存压缩存储，不写盘），失败回退 `ProcessQuotaLimits`
- 白名单进程加保护：页优先级提到 5、standby 只清低优先级、临时设硬工作集下限
- 白名单文件缺失或为空时拒绝清理；运行期锁定该文件（可编辑，不可删改）
- 界面精简为一个清理按钮 + 自动清理阈值；常驻托盘，图标显示占用数字（绿/黄/红三档）
- 每次清理写一份报告到 `%TEMP%\memreduct.log`，相同条目自动折叠

## 用法

1. 运行 `memreduct.exe`，UAC 弹窗选「是」（需要管理员权限）
2. 编辑同目录 `白名单.txt`，每行一个路径；`#` 开头为注释，去掉 `#` 即生效：

```
# 目录：该目录及其子目录下所有程序都豁免
#D:\Steam\steamapps\common\PUBG
# 单个程序：只豁免这一个 exe
#D:\Tools\obs64.exe
```

3. 点「立即清理」；勾选「内存占用超过 N% 时自动清理」可常驻后台
4. 关闭窗口 = 最小化到托盘；托盘右键退出，左键双击恢复窗口

## 白名单匹配

- 目录条目覆盖其下全部子目录
- 完整 exe 路径条目只覆盖该程序
- 被豁免进程启动的子进程同样豁免（exe 位于别处也算）
- 按目录分隔符判定边界，`App` 不会误伤 `App2`

## 配置

`memreduct.ini` 与 exe 同目录，首次运行自动生成：

| key | 默认 | 说明 |
|---|---|---|
| `AutoClean` | 0 | 自动清理开关 |
| `AutoCleanPercent` | 85 | 自动清理阈值，范围 20-99；低于 50 界面会提示可能反复清理 |
| `CleanSystemCache` | 1 | 是否顺带清系统文件缓存 |
| `AllowEmptyWhitelist` | 0 | 置 1 才允许空白名单下清理 |
| `LogEnable` | 1 | 是否写 `%TEMP%\memreduct.log` |
| `TrayWarningPercent` | 70 | 托盘黄档起点 |
| `TrayDangerPercent` | 85 | 托盘红档起点 |

自动清理节奏：越过阈值即清理，两次之间至少 30 秒；若清理后仍高于阈值，每 5 分钟最多再清一次（阈值低于 50 时为 10 分钟）。

## 构建

需要 VS2022（v143）与 Windows SDK：

```
msbuild memreduct.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

`routine` 依赖按 `.gitmodules` 放在上一级目录（`../routine`），与 `memreduct` 同级。

## 实测

**场景**：后台常驻 5 个应用 + 运行 PUBG（PUBG 安装目录写入白名单），自动清理阈值 85%。

**表现**：自动清理触发瞬间游戏无可感知卡顿；游戏纹理未被驱逐，切场景不出现读盘卡顿，1% low 帧有可测提升。

**记录方法**：

1. RTSS 打开 OSD，显示 frametime 与 1% low
2. 固定同一场景各跑 5 分钟：A 组关闭自动清理，B 组开启自动清理
3. 对比两组的 1% low 与最大 frametime

**对照组**：原版在同样场景下清理会压掉游戏工作集，切场景时出现明显读盘卡顿。

## 已知限制

- standby list、系统文件缓存、modified page list 是全局接口，没有按进程参数，只能间接规避
- 受保护进程（PPL、部分 SYSTEM 服务，例如 `AppXSvc` 所在的 svchost）打不开，会跳过并写进日志
- 清理过程中进程被强制结束时，白名单进程的硬工作集下限会保留到它重启

## 许可

GPL-3.0。原项目：https://github.com/henrypp/memreduct

## 交流

公开技术探讨 Q 群：**1102821216**（问题反馈、使用交流）
