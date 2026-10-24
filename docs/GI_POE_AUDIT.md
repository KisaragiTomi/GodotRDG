# GI-POE 实现审计与截图解读

2026-09-24。检查对象：`D:\MyProject\GI-POE\project`，以及对应 Godot 4.6.1 源码。

结论：用户怀疑“写得有问题”有明确代码依据。这里既有实现错误，也有将二维屏幕近似当作三维传播的问题；不能用旧项目表现判断 Radiance Cascades 的理论或工程上限。

本次只读审计，未更改旧工程，也没有运行截图对应状态。下面明确区分已确认代码事实、条件判断和未复现的视觉影响。

## 1. 优先确认截图是否实际执行 RC

用户提供的是带灯光图标和网格的编辑器透视视图。它显示高饱和彩色光斑和大面积近黑区域，但这些现象不能单独证明 RC 在运行或失败。

- [dungeon_test.tscn:19](D:/MyProject/GI-POE/project/scenes/dungeon_test.tscn:19)：WorldEnvironment 仅设置 Environment。
- [dungeon_test.tscn:27](D:/MyProject/GI-POE/project/scenes/dungeon_test.tscn:27)：RC Compositor 只挂在场景 Camera3D。
- [编辑器源码:6124](D:/MyProject/GI-POE/godot-source/editor/scene/3d/node_3d_editor_plugin.cpp:6124)：自由视口有独立 Camera3D。
- [编辑器源码:4501](D:/MyProject/GI-POE/godot-source/editor/scene/3d/node_3d_editor_plugin.cpp:4501)：相机预览直接挂接场景相机 RID；关闭预览恢复自由相机，不是把场景 compositor 复制给自由相机。
- [渲染选择逻辑:3524](D:/MyProject/GI-POE/godot-source/servers/rendering/renderer_scene_cull.cpp:3524)：优先使用当前相机 compositor，否则使用场景 compositor。

**条件结论：** 若当时是普通编辑器自由视口，没有 Camera/Cinematic Preview、未保存修改或额外工具挂载，RC 不会应用到该视口。截图不能百分之百确认预览状态，必须在运行视口或明确启用相机预览后做开关验证。[官方 WorldEnvironment compositor 说明](https://docs.godotengine.org/en/4.6/classes/class_worldenvironment.html#class-worldenvironment-property-compositor)。

## 2. 已确认：级联合并没有声明 GPU 先写后读依赖

[rc_effect.gd:346](D:/MyProject/GI-POE/project/scripts/rc_effect.gd:346) 在同一个 compute list 中连续 dispatch，按 `C3←C4、C2←C3、C1←C2、C0←C1` 更新同一 atlas；[370 行](D:/MyProject/GI-POE/project/scripts/rc_effect.gd:370) 后没有 `compute_list_add_barrier()`。

[rc_merge.glsl:57](D:/MyProject/GI-POE/project/shaders/rc_merge.glsl:57) 读远级联，[79 行](D:/MyProject/GI-POE/project/shaders/rc_merge.glsl:79) 写近级联。后一个 dispatch 需要看到前一个 dispatch 的完整写入，CPU 循环次序不足以声明这个依赖。

**核验依据：** Godot [RenderingDevice:5785](D:/MyProject/GI-POE/godot-source/servers/rendering/rendering_device.cpp:5785) 的 `compute_list_add_barrier()` 会拆分当前 list 并恢复绑定；Vulkan dispatch 路径只是派发命令，没有隐式补足此内部依赖。官方示例也明确说明未加 barrier 的 dispatch 可并行执行。

这是同步正确性缺陷，Vulkan 路径尤其明确。部分 D3D12 legacy barrier 路径可能隐式补 UAV barrier，不能泛称所有后端表现相同。尚未通过运行证明它造成截图中的哪一个现象。

修正方向：在依赖级联 dispatch 之间调用正确的 compute-list barrier，或每一级独立 list；不应把已废弃的全局 `barrier()` 当成替代。先验证这一小改动，再判断剩余画质问题。

## 3. 已确认：接收面的材质未参与间接光合成

[rc_build.glsl:109](D:/MyProject/GI-POE/project/shaders/rc_build.glsl:109) 从已着色画面读颜色；[rc_apply.glsl:130](D:/MyProject/GI-POE/project/shaders/rc_apply.glsl:130) 最后直接执行 `scene_color.rgb + gi_color`。

这条路径没有接收面的 albedo、金属度或真实法线输入，因此不能正确表达不同材质对漫反射入射光的响应。读取的场景颜色还可能含有镜面成分，不能简单视为方向无关的反弹辐射。

**结论：** 直接提高 `gi_intensity` 不能修复材质响应，容易放大不自然染色。需要区分光照量、表面反射率、发光和镜面分量，并在正确的线性着色阶段接入。这里没有证据证明无限跨帧累加，不作此断言。

## 4. 已确认：深度拒绝权重的下限会失效

[rc_apply.glsl:95](D:/MyProject/GI-POE/project/shaders/rc_apply.glsl:95) 使用 `bilinear_w * max(depth_w, 0.05)`，最后又用权重总和归一化。

当四个邻居都不匹配而触发相同 0.05 下限时，这个公共系数会在归一化时抵消，结果仍是这些错误邻居的普通加权平均。它不是“只保留 5% 污染”，而可能完全依赖错误表面。

[rc_merge.glsl:48](D:/MyProject/GI-POE/project/shaders/rc_merge.glsl:48) 的跨级联插值本身也没有深度或可见性拒绝。薄墙、门边和墙地交界存在明确的颜色污染风险，程度需测试。

修正方向：真正拒绝不匹配邻居，给“所有邻居无效”定义明确回退；不能用无条件下限消除零权重而同时宣称保留遮挡。

## 5. 已确认：探针采样位置与重建假设不一致

[rc_build.glsl:51](D:/MyProject/GI-POE/project/shaders/rc_build.glsl:51) 的抖动 seed 含方向编号。同一探针的不同方向从不同 XY、甚至不同表面深度开始。

但 [rc_apply.glsl:89](D:/MyProject/GI-POE/project/shaders/rc_apply.glsl:89) 用未抖动中心的单一深度来代表整个探针，[rc_merge.glsl:31](D:/MyProject/GI-POE/project/shaders/rc_merge.glsl:31) 也假定固定中心。深度断层附近，采到的表面和验证的表面可能不是同一个。

抖动可以合法使用，但需要和存储、重建及可见性模型一致。当前实现缺少这种一致性，不能视为仅调随机噪声强度即可解决。

## 6. 三维传播被二维屏幕近似替代

[rc_build.glsl:64](D:/MyProject/GI-POE/project/shaders/rc_build.glsl:64) 只有二维 `ray_dir`，[68 行](D:/MyProject/GI-POE/project/shaders/rc_build.glsl:68) 读一次 probe depth；射线之后只推进 XY，并一直以固定 `probe_z` 和厚度启发式判断命中。

它没有沿三维射线变化的位置与深度；采样方向是一圈二维方向，而不是表面上半球。不能完整表示墙面向地面、高低差或桥下的传播，也读不到屏幕外和遮挡后的信息。

这不说明所有屏幕空间 RC 都错误，也不否定原论文；它说明此具体简化与完整 3D 需求不匹配。同步修复后仍需替换几何追踪与重建部分，不能承诺一行改动就获得自然 GI。

## 7. 正交兼容性是潜在缺陷，不是当前截图已查明原因

[rc_build.glsl:31](D:/MyProject/GI-POE/project/shaders/rc_build.glsl:31) 和 [rc_apply.glsl:27](D:/MyProject/GI-POE/project/shaders/rc_apply.glsl:27) 写死透视反向 Z 公式；未使用实际逆投影矩阵。若改成正交相机，会错误重建深度。法线也是深度差加人为尺度的近似。

**但**当前场景相机是默认透视，near=0.1、far=100，和 RC 参数一致，所以不能把正交 bug 当成此截图的既定根因。未来验证正交时，应使用真实投影参数、逆矩阵重建位置和正确空间的法线。

## 8. 场景设置也会放大不自然感

当前主场景的材质是很暗的常量颜色，缺少材质纹理变化；环境光很低，SDFGI 关闭，SSIL/SSAO/glow/雾同时存在。这些可以作为风格选择，但不适合作为孤立验证新 GI 的唯一基准。

主场景内容直接保存在 `dungeon_test.tscn` 中，没有挂载 `dungeon_generator.gd`；不能仅修改生成脚本的参数就声称当前场景已经改变。

正确 GI 不会凭空让吸光材质变亮。应该先用中性灰漫反射房间、一面有色墙和一个光源建立基准，再引入深色石材与彩色效果。

## 推荐验证顺序

1. 在新实验工程保留旧实现作为对照，不修改原目录。确认运行相机执行了 RC，使用现有 INDIRECT_ONLY / CASCADE_ATLAS 调试模式。
2. 单独修复级联合并同步，比较同一视角及纯间接光输出，记录后端。
3. 统一 probe 位置并修复深度拒绝回退，检查薄墙和深度断层。
4. 使用真实三维几何/法线与正确材质输入，避免把屏幕颜色直接叠加视为完整 GI。
5. 用 LightmapGI 的固定场景参考、VoxelGI 现成实时方案，以及可信 3D RC 实现对比。
6. 若仍受屏幕缺失限制，再决定世界空间 RC、DDGI 或硬件 RT 路线。

本阶段完成调研和审计，不代表上述修复已实现或画质已经改善。
