# 3D top-down 间接照明：网上方案对比

调研日期：2026-09-24。用户确认 3D，要求明显改善旧 GI-POE 的自然度，并怀疑旧实现错误。以下是文档、作者仓库与本地源码评估，不是本机渲染性能实测。

## 结论

现阶段应优先建立可信的图像对照，再定最终算法。旧 RC 存在实现和观察路径问题，不能把旧截图当成 Radiance Cascades 的质量上限。

建议顺序：高质量静态 LightmapGI 参考 → VoxelGI 实时对照 → 正确实现的 3D RC / 世界空间 DDGI。若接受硬件光追，加入 NVIDIA Godot 路径追踪分支作质量参照。最终路线取决于动态几何、性能和平台约束，而不只是俯视视角。

## 可用路线

### 1. LightmapGI：静态关卡首先建立的质量基线

优点是稳定、运行时成本低、细节和遮挡比低分辨率体素方案更容易做好。需要 UV2 和离线烘焙；动态角色通过探针接收间接光。可以保留实时直接光及阴影，但已烘焙反弹不会跟着移动灯、开门或拆墙正确改变。

**判断：** 如果地图基本静态，它可能就是最终主方案；即使最终做动态 GI，也适合作为固定场景的近似质量参考。不能把烘焙结果称为无误差真值。

来源：[Godot 4.6 LightmapGI](https://docs.godotengine.org/en/4.6/tutorials/3d/global_illumination/using_lightmap_gi.html)。

### 2. VoxelGI：小型 3D 场景的现成实时对照

静态结构先体素化，动态灯可更新，Dynamic 模式物体可参与 GI。静态结构改动仍需重烘焙，动态对象增加成本；薄墙和小尺度几何会受到体素精度限制。

**判断：** 先在同一个小房间跑出可信基线，比直接自研完整渲染器更快检验“当前效果差是否主要来自实现”。它不是自动消除漏光的方案。

来源：[Godot 4.6 VoxelGI](https://docs.godotengine.org/en/4.6/tutorials/3d/global_illumination/using_voxel_gi.html)。

### 3. SDFGI：大范围地图候选

支持动态灯和大范围覆盖，动态物体可以接收间接光，但动态遮挡物及动态发光表面不能完整参与传播。级联更新、空间分辨率和相机移动带来质量与耗时取舍。低垂直度关卡可研究 Y Scale 分布。

**判断：** 适合大地图对照，不应因名字里“实时”就认定它能正确处理任意开门和拆墙。

来源：[Godot 4.6 SDFGI](https://docs.godotengine.org/en/4.6/tutorials/3d/global_illumination/using_sdfgi.html)。

### 4. SSIL / 屏幕空间 SSGI：补充近处细节

官方 SSIL 定位是其他 GI 的补充，可增加可见动态表面的反弹。屏幕外及被不透明物体遮住的信息不在输入中。俯视镜头平移、缩放、遮屋顶时，这种缺失仍然存在。

**判断：** 可用来补细节，不适合独立承担稳定的空间间接光。高饱和模糊光斑不等于正确的光能传播。

来源：[Godot SSIL](https://docs.godotengine.org/en/4.6/tutorials/3d/environment_and_post_processing.html#screen-space-indirect-lighting-ssil)。

### 5. 3D Radiance Cascades：保留，但要求更严谨的实现

[gomezzz/godot-3d-radiance-cascades](https://github.com/gomezzz/godot-3d-radiance-cascades) 是真实的 Godot 4.7 / Forward+ 世界空间实验，使用软件 BVH 追踪。它是值得下载验证的参照，不是已经证明成熟的最终产品方案。

限制包括固定探针体积、静态网格重建、动态解析代理数量限制、无骨骼网格和纹理级反弹；粗采样、薄墙、小发光体及最终表面取样仍可能产生误差。多次反弹依赖迭代反馈，并非一次级联合并就自动完成。

**判断：** 保留 RC 研究，但先修正旧实现的同步、三维重建、可见性和材质响应，再比较图像。不能把一个新 demo 的宣传帧率移用到本项目。

[原作者论文源码](https://github.com/Raikiri/RadianceCascadesPaper/blob/main/RadianceCascades.tex) 描述了 PoE2 使用的屏幕空间实现，但不能据此推断其完整渲染器只有屏幕 GI。级联数据结构的复杂度性质也不等于场景追踪和更新成本完全与几何无关。

### 6. DDGI：动态 3D 漫反射研发候选

世界空间探针保存辐照度和距离信息，结合可见性、重定位及分类来降低穿墙与无效探针问题。屏幕外几何可以参与，适合真实三维空间。

代价是低频近似、时域响应延迟、探针数量和场景更新成本。RTXGI-DDGI SDK 需要宿主提供光线追踪、加速结构及材质命中着色，不是直接安装到 Godot 的成品插件。

**判断：** 若确有动态几何和离屏光照要求，优先列入研发候选；不能承诺天然无漏光或必然快于 RC。

来源：[NVIDIA 算法与局限](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Algorithms.md)、[接入要求](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Integration.md)。

### 7. NVIDIA Godot 路径追踪分支：质量参照及高端路线

[NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot/tree/nvidia-pt-dlss-dev) 有真实分支，已通过远程 Git 和源码核验。Environment 中有路径追踪开关、每像素采样数、最大反弹次数和降噪选项。

**判断：** 本机 RTX 5070 值得用于该路线的后续验证。当前未编译、未运行该 fork，不能承诺材质、骨骼、正交、驱动或平台适配已满足要求。它属于引擎分支接入工作，不能当作给 4.6.1 打开一个开关。

核验版本：`nvidia-pt-dlss-dev` HEAD `e00193295868ee85575ac17b7b53e2dd75acc7de`；`nvidia-pt-dlss` HEAD `135dff3887a3d2ccf45cafbd9bdffa7b718e1e76`。分支会继续变化，应在测试时固定提交。

来源：[分支 Environment 源码](https://github.com/NVIDIA-RTX/godot/blob/e00193295868ee85575ac17b7b53e2dd75acc7de/scene/resources/environment.cpp)。

## 不直接作为本项目底座的例子

- [Sohojoe Godot RC](https://github.com/Sohojoe/radiance-cascades-godot)：本机旧参考库，主要是 2D 移植，不能直接代表完整 3D GI。
- [2.5D RC 伪代码](https://github.com/CorylusDonuts/2.5-d-Radiance-Cascade-Pseudocode)：不是完整插件，追踪等函数有占位实现。
- [Split Radiance Cascades 论文](https://arxiv.org/abs/2607.20384) 与 [chandra 参考代码](https://github.com/entropylost/chandra)：新的稀疏三维研究方向；非 Godot，依赖、构建和许可需额外核实，本次未审核完整论文实验。
- 单层俯视高度场：可以是节省成本的工程近似，但桥下、楼层、屋檐需要多层或真正 3D 表示。此项为工程判断，不是某个现成插件的承诺。

## top-down 专项验收

1. 固定相机、材质、曝光、灯强，以无 GI / 仅 GI / 合成结果三种视图保存图像。
2. 中性房间加一面有色墙，检查地面颜色反弹；在黑、灰、白接收面检查材质响应。
3. 薄墙、窄门、墙后亮灯，检查可见性与漏光。
4. 光源移出屏幕，镜头平移/缩放，观察照明是否突然消失。
5. 斜坡、阶梯、墙地交界，以及桥上/桥下，检查三维传播与层间隔离。
6. 开门、移灯、增加墙体，记录反应时间和历史残留。
7. 分别验证透视和正交。Godot 仍有历史正交问题报告，例如 [VoxelGI #69784](https://github.com/godotengine/godot/issues/69784)、[SDFGI #60522](https://github.com/godotengine/godot/issues/60522)。这些不等于已在本机当前版本复现。
8. 记录 GPU pass 时间、总帧时间和分辨率。帧率对比前统一 VSync、动态分辨率、抗锯齿及场景内容。

先关闭 glow/雾和额外 SSIL/SSAO 进行诊断，再逐项恢复美术效果。正确的间接光仍受材质反射率影响：全黑材质不会因为更换 GI 算法就自然产生明亮反弹。

## 版本和平台

原生 VoxelGI、SDFGI、SSIL 在所核验的 4.6 文档中要求 Forward+。LightmapGI 烘焙结果可供 Forward+、Mobile 和 Compatibility 使用，烘焙本身有 RenderingDevice 硬件要求。[渲染器文档](https://docs.godotengine.org/en/4.6/tutorials/rendering/renderers.html)。

[当前官方 Windows 下载](https://godotengine.org/download/windows/) 为 4.7.2；本目录复制 4.6.1 以保留旧工程复现基础，不把两个版本的功能混在一起。
