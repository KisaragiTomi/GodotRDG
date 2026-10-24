# Classic Crytek Sponza

用户截图中的经典 Sponza 中庭，来自 Khronos glTF Sample Assets；采用完整 glTF 2.0/PBR 版本。

- 模型入口：`res://assets/sponza/glTF/Sponza.gltf`
- 上游：[Khronos Sponza](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza)
- 下载日期：2026-09-24。GitHub 提交查询不可用，下载自 `main`；每个实际下载文件的 URL、字节数和 SHA-256 保存在 `download-manifest.json`，不声称固定到了某个提交。
- 完整资源：1 个 glTF、1 个二进制缓冲区、69 张贴图，以及 4 个上游说明/授权文件，共 52,691,478 字节（50.25 MiB）。Godot 导入生成的缓存不计入这个大小。
- 上游提供 25 个材质；保留原始节点缩放、PBR 参数、法线和透明裁切贴图。
- `README.md`、`LICENSE.md`、`metadata.json` 是未经修改的上游文件。它们的相对文档链接沿用上游仓库结构；本地授权引用另存于 [licenses/LicenseRef-CRYENGINE-Agreement.txt](licenses/LicenseRef-CRYENGINE-Agreement.txt)。模型授权声明与说明文件授权声明分别以 `LICENSE.md` 为准。

已使用随项目提供的 Godot 4.6.1 执行 headless editor import，成功导入完整场景，日志：`artifacts/sponza-import.log`（项目仓库根目录）。这是资源导入验证，不是 GPU GI 效果验证。

在 Godot FileSystem 中将 `Sponza.gltf` 拖入 3D 场景即可实例化。截图中的灯光、发光球及 Lumina GI 效果不属于这个模型。2026-09-28 已加入独立的自研网格 GI 测试场景（`res://scenes/sponza.tscn`），使用三角形 BVH 和 UV2 表面缓存；在仓库根目录运行 `./tools/run-sponza.ps1`。实现、验证和限制见仓库的 `docs/SPONZA_TEST.md`。

需要重新下载时，在仓库根目录执行 `python tools/download-sponza.py`。脚本保留 glTF 的资源引用，并重新生成下载清单。
