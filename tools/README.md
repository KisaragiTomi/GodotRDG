# 快速运行与验证

`./tools/run-city.ps1` 运行默认城市大气场景。模型已附带，无需下载 Sponza 或生成 BVH。`-Capture` 保存真实 Vulkan 航拍、街道有雾/无雾、广场和控制面板截图到 `artifacts/city/`；`-Rebuild` 根据固定随机种子重新生成可编辑的 `scenes/city.tscn`，会覆盖对该场景文件的手工布局修改。场景范围和来源见 [城市场景说明](../docs/CITY_SCENE.md)。

`F1/F2/F3` 在城市、Sponza TG GI + 雾、拱廊之间切换。统一面板显示原生雾体积计算和 TG GI 的独立 GPU 时间戳耗时。`validate-viewer.gd` 用实际按键事件覆盖场景往返、GI 资源重载、关闭效果后清除计时，并输出到 `artifacts/viewer/`；运行命令见城市场景说明。该检查需要已准备的 Sponza 和真实 Vulkan，不能使用 headless。

`./tools/run-atmosphere.ps1` 运行独立阳光体积雾场景，无需 Sponza。`-Capture` 将 Vulkan 截图和自动结果写入 `artifacts/atmosphere/`。`./tools/run-tg.ps1 -Atmosphere` 将雾接入现有 Sponza；附加 `-Capture` 验证同场景对照和 GI 历史隔离，结果在 `artifacts/atmosphere/sponza/`。实现依据与控制见 [大气雾说明](../docs/TG_ATMOSPHERE.md)。

雾接口补丁已并入 `engine/rdg-engine.patch`，用 `tools\build_engine.bat` 构建（见 README 的“编译”）；`resolve-godot.ps1` 在没有显式 `GODOT_EXE` 时使用 `%GODOT_SOURCE%\..\godot-rdg\bin\` 下的 rdg 构建。Sponza 大气材质先输出无雾照明，缓存后再由 `TGSurfaceFog` 合成原生积分雾。

`./tools/validate-tg-fog-history.ps1 -Python <python.exe>` 用同一个 rdg 引擎构建（`resolve-godot.ps1`）分别跑原生雾、独立表面缓存和全程无雾控制序列，覆盖 MSAA、浓雾、天空边缘、GI 开关、移动相机、遮挡、点灯、关灯归零与缩放。检查雾合成图像误差和 GI/辐射缓存逐值隔离，输出 `artifacts/tg-fog-history/`。测试专用 `--tg-native-fog` 强制旧路径。统一面板的 Fog GPU 计入新增合成 pass，GI GPU 仍包含表面历史复制。

`./tools/validate-tg-sky.ps1 -Python <python.exe>` 检查 Sponza 共享天空源的真实 Vulkan 候选辐射、方向、150 米远处遮挡、半强度/暖色响应、三帧归零、屏幕反馈、自适应预算、缩放和 `8` 键开关。结果在 `artifacts/tg-sky/`；需要 numpy、Pillow 与已准备的 Sponza。`run-tg.ps1 -Atmosphere` 默认启用天空，`-NoSky` 从关闭天空开始；普通无雾入口保持原照明。

`./tools/validate-tg-world-cache.ps1 -Python <python.exe>` 检查可选的世界空间间接光缓存：先在真实 GPU 上验证 16 个薄墙/法线/哈希碰撞/过期案例及长时间反馈稳定性，再串行运行关闭缓存、开启缓存与开雾三组相同输入。比较 13 个状态的离屏复用、过期回退、灯光/遮挡失效、屏幕历史、基础滤波、缩放及 `9` 键开关，并要求开雾/关雾的 GI 和缓存字节一致。需要 numpy 与已准备的 Sponza，结果在 `artifacts/tg-world-cache/`。交互使用 `run-tg.ps1 -WorldCache`；默认关闭，首次启用额外分配约 4.25 MiB。

`./tools/validate-atmosphere-quality.ps1 -Python python` 对照旧/新体积雾参数与更高采样参考，并验证相机移动重投影、视角恢复及关灯后的历史清理。需要已准备的 Sponza、NumPy 和 Pillow；结果与三组同机位截图位于 `artifacts/atmosphere-quality/`。

`./tools/validate-city-fog.ps1` 验证城市完整雾覆盖、两个质量预算、密度/开关、运动和环境恢复，并追加约 566–1301 米远距离的城市雾信号回归。城市 `V` 或启动参数 `-FullFog` 切换采样质量，两档均自动覆盖城市，不缩短远景范围。截图与数值分别在 `artifacts/city-fog/` 和 `artifacts/city-coverage/`。

`./tools/validate-bvh.ps1` 运行 BVH 的暴力求交对照测试与四次顺序 GPU A/B。新准备流程默认 SAH，`prepare-sponza.ps1 -Bvh median` 保留旧方案。已有缓存可用 `python tools/triangle_bvh.py --method sah --output artifacts/bvh/sah` 单独构建备用加速数据，再用 `run-tg.ps1 -BvhDirectory artifacts/bvh/sah` 运行。计时、固定射线 SH/截图等价性与结果在 `artifacts/bvh/`。

所有交互演示的相机按键与通用飞行相机（`%GODOT_SOURCE%\misc\shared_scripts\fly_camera.gd`）一致：按住右键转向，WASD 移动，E/Space 上升、Q 下降，Shift 加速，滚轮调移动速度；P 恢复观察预设。释放右键或失焦释放鼠标，捕获时 Esc 先释放、再按退出。盒体演示的正交俯视预设里滚轮缩放视野，右键自动切入透视。

自由相机输入与 TG 非俯视角 GPU 集成检查：

```powershell
$godot = & ./tools/resolve-godot.ps1
& $godot --path . --rendering-driver vulkan --max-fps 60 --script tools/validate-free-camera.gd -- --tg
```

键盘飞行只在窗口有系统焦点时生效（`FreeFlyCamera._process` 检查 `has_focus()`）：从后台进程启动、另一个程序占着前台时，Windows 的前台锁不给新窗口焦点，键盘飞行各项会失败，重跑或启动后点一下窗口即可。检查右键捕获、转向、飞行、升降、加速、滚轮调速、移灯按键不动相机、焦点与 Esc 释放，以及预设恢复；旋转、平视、仰视和反向观察时验证 SH 有限与实际追踪。结果和截图在 `artifacts/free-camera/`，失败返回非零退出码。

`./tools/run-tg.ps1` 打开 TG 风格的 Sponza 实时 GI。`-Capture` 输出 `artifacts/tg/` 下的截图、SH 调试数据、结果 JSON 与 `run.log`，检查屏幕/BVH 两种追踪、历史降噪、移灯、动态遮挡板、关灯归零、相机重投影和窗口缩放；功能断言或 Godot 错误都会使脚本失败。该路径只读取已准备的三角形 BVH 与材质，不预计算任何光照或传输。详见 `docs/TG_SCREEN_GI.md`。

`./tools/validate-tg-noise.ps1` 串行运行 TG 初版和改进后的降噪链路，比较静止、连续移灯和停下后的帧间波动，同时要求关灯归零。结果与前后截图写入 `artifacts/tg-noise/`；默认交互版使用改进后的低差异采样、有效 tap 历史插值、几何法线引导与 TAA 颜色历史。

`./tools/validate-tg-camera.ps1 -Python <python.exe>`（需要 numpy）使用相同相机轨迹和基础滤波，对比稀疏预算、自适应质量预算和每接收点 32 条射线的参考。导出冷启动、平移各阶段的 GI/几何缓冲与截图，按相同世界位置的像素比较间接光误差，另外检查屏幕/BVH 集成、预算切换和关灯归零。结果在 `artifacts/tg-camera/`。这是相同渲染器的高采样参考，不是无偏路径追踪真值。交互版默认稀疏预算加后期降噪，`6` 依次切换稀疏、局部自适应和质量三档，`7` 对比后期降噪与旧基础滤波。

`./tools/run-tg.ps1 -Sampling Adaptive` 启动局部自适应稀疏档，每块额外候选射线最多增加 50%，静止后按历史年龄与方差回落。`./tools/validate-tg-adaptive.ps1 -Python <python.exe>` 用相同后期滤波串行捕获稀疏、固定补样、自适应及新生成的 32 射线参考，检查逐块预算、采样帧号/历史年龄一致性及全图/年轻历史/边缘误差；随后验证移动灯、非整倍数尺寸、天空、三档切换、完整 GI 和开雾。结果在 `artifacts/tg-adaptive/`。

`./tools/validate-tg-post.ps1 -Python <python.exe>` 固定每 4×4 像素一条射线，比较基础滤波和四轮方差引导后期降噪。验证静止、移灯、相机运动、关灯归零，并与 `artifacts/tg-camera/reference/` 的 32 射线结果比较全图和几何边缘误差；参考不存在时生成。变更场景/追踪算法后应先重跑 `validate-tg-camera.ps1` 更新参考。结果在 `artifacts/tg-post/`，包含预算/历史年龄一致性检查、误差与耗时。`run-tg.ps1 -BasicFilter` 可交互观察对照。

`./tools/prepare-sponza.ps1 -Python <python.exe>` 重新生成 TG 用的三角形 BVH 和材质数据（需要 numpy），不烘焙任何光照；Sponza 网格或材质改动后才需要重跑。

在项目根目录运行 `./tools/run-lab.ps1` 打开盒体交互演示，`4` 在运行时烘焙原生 VoxelGI 作对照（`scripts/native_baselines.gd`）。`./tools/validate.ps1` 依次导入、运行封闭薄墙 GPU 回归、实际材质 shader 回归及完整场景截图/性能采样（含 VoxelGI 对照）。全部通过后返回 0；日志含 Godot 脚本/引擎错误也会判定失败。

默认 256 rays，支持 `-Rays 16..512`。`-Backend vulkan` 为本轮验证后端。`-Backend d3d12` 只提供启动选项，未进行本轮质量/性能验收。`--headless` 仅用于导入和语法检查，不能代替 GPU 检查。

`./tools/run-emissive.ps1` 打开发光面演示；`./tools/validate.ps1 -Emissive` 追加自发光辐射量、近场重建及开关灯验证。演示捕获包括固定俯视全景和 `emissive_detail.png` 近景。直接启动项目时传入 `--coarse-emitter-receivers` 可关闭近发光体墙面的局部细化，用于同参数 A/B。

`./tools/run-ball.ps1` 打开可控制的自发光球：方向键移动、PgUp/PgDn 升降、Shift 加速、R 复位小球、C 改色、T 开关、+/- 强度；相机使用上述自由观察控制。`./tools/validate.ps1 -Ball` 增加球形辐射/遮挡 GPU 测试与输入、移动、碰撞、变色和开关回归。球模式的原生 VoxelGI / LightmapGI 快捷键禁用，避免静态对照误用于动态光源。
