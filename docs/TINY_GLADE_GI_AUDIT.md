# Tiny Glade 现有反编译材料的 GI 审计

调查日期：2026-09-24。输入目录：`D:\MyProject\Tiny Glade`。本轮只读检查原目录，未运行游戏、未知二进制或修改任何原始资源。唯一新增文件是本报告。未发现目录内额外 AGENTS.md；当前工具集中没有 MemPalace，因此将可复用结论保存在这里。

## 结论

现有证据支持的是：**稀疏屏幕射线 + 软件三角形 BVH / 地形高度图回退 + 低阶方向光照表示 + 时空重建 + 艺术化照明**。这批 GI 内核没有呈现体素光照传播、规则世界空间探针级联或当前 Godot 方案那样的逐面 surfel atlas。

它的平滑效果有具体成本和取舍：减少实际追踪数量、共享周围的射线结果、用每颜色四个球谐系数表示入射方向分布、累积最多 48 帧，并根据深度/法线/遮蔽引导滤波。最终在真实像素法线上重新求照明，因此不会直接把一个粗糙世界空间光照格子画到墙上。但是屏幕依赖、历史延迟和局部细节丢失仍然存在；静态审计不能证明运行时完全没有漏光或拖影。

两个容易误读的命名需要纠正：

1. `restir_*` 资源确实存在，但这批内核中的 update 是候选复制与固定权重初始化，eval 是邻域加权投影；**不能据此称为完整 ReSTIR GI**。
2. `_ssgi_ssgi` 当前内核只读深度和法线，输出单通道 R16F。它在此链中提供的是**几何遮蔽/可见性引导**，不是 RGB 间接光能运输。真正的彩色间接光由 `gi_*` 链产生。

这些结论针对磁盘上已检查的编译变体。没有帧捕获和 CPU render graph 源码，不能断言所有画质设置、所有游戏版本均使用同一配置。

## 材料与证据等级

- **已证实**：现有 GLSL 可直接读到的运算，或本轮对指定 SPIR-V 静态解码后可直接读到的运算。
- **强推断**：资源读写、公式和调用符号共同支持，但仍缺运行时绑定/调度证据。
- **未确认**：符号名、旧报告或经验猜测不足以证明。

原有材料包含 `compiled-shaders/*.bin`、292 个 `tmp/shaders/` 文件、`tmp/pdb_symbols.txt`、`tmp/exe_symbols.txt` 和项目结构报告。**核心 GI/SSGI 原来没有对应的 GLSL 文件**；现有 GLSL 主要覆盖地形、植被和材质等。旧 [PROJECT_ANALYSIS.md](</D:/MyProject/Tiny Glade/PROJECT_ANALYSIS.md>) 仅作为线索，不作为算法结论的证明。

本轮先解析 SPIR-V 的 OpName/OpMemberName，再使用已安装的 `C:\Program Files\RenderDoc\plugins\spirv\spirv-cross.exe`，将二进制中的 SPIR-V 经 stdin 输入、GLSL 输出到 stdout。没有向 Tiny Glade 目录写解码文件。工具自报版本 `vulkan-sdk-1.4.350.0-19-g146679ff`。下文“stdout L...”是此次可复现解码的输出行号，不是磁盘 GLSL 文件行号；同时给出资源和公式，避免依赖易变的临时变量编号。

两个核心输入的 SHA-256：

- [trace](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_trace_gi_candidates.cs.hlsl_cs_main_0.bin>)：`9CE80D1042477C7B4F14CB07FEEBFCD27CB7087CC8A143DDDAB64D256A78C59F`
- [update](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_update_gi_radiance.cs.hlsl_cs_main_0.bin>)：`603CEB869C8FDA906D8091C57E490F49F070704C0C72B89512E585DBCF54399B`

`build-info/Cargo.lock` [L3782](</D:/MyProject/Tiny Glade/build-info/Cargo.lock:3782>) 确有 `kajiya 0.2.0`，[L5936](</D:/MyProject/Tiny Glade/build-info/Cargo.lock:5936>) 有 `rhapsody 0.1.0`。这只能证明包名出现在锁文件，不能证明当前 GI 等同开源 kajiya 的某个算法/版本。

## 1. 几何与追踪

**已证实：GI 回退使用软件 BVH 和压缩三角形，而非硬件光追指令。**

[trace 二进制](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_trace_gi_candidates.cs.hlsl_cs_main_0.bin>) 的 `cs_main` 解码为 `main`：

- stdout L154–172：`wall_color_ids`、`rt_triangle_meta`、`StructuredBuffer<PackedBlBvhNode>`、`StructuredBuffer<PackedTriangle>`。
- stdout L990–1099：手动解码节点包围盒、位掩码、子节点并遍历；L1127–1157 从 `v` 和半精度打包的 `e` 恢复三角形边，使用 cross/dot 和重心坐标进行射线相交。
- 模块能力列表为 Shader / RuntimeDescriptorArray，不包含硬件 ray query / ray tracing 能力；代码本身呈现完整软件遍历。
- [PDB 符号 L49418](</D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt:49418>) 一段包含 `RtMesh`、`RtWorld`、`build_gpu_bvh`、`bvh_builder_worker`，以及 L49453/49454 的 `update_rt_world` / `upload_rt_world`。动态几何维护是有依据的，但 BVH 重建频率、增量粒度和 CPU 成本未确认。

**已证实：地形另走高度图。** trace stdout L1260–1351 限定约 X/Z ±65 的场景范围，对 `visual_terrain_heightmap` 最多粗步进 32 次，再二分 8 次；同时比较 `terrain_heightmap` 以分类命中。它没有把所有地形三角形强行塞入同一追踪路径。

**未确认：** RT 简化几何与屏幕真实网格的差异、透明/镂空材质覆盖、极薄墙的可靠厚度，以及所有装饰物是否进入 BVH。不能仅因为使用三角形追踪就宣布不存在漏光。

## 2. 光照来自哪里、实际几次反弹

**已证实：先追屏幕，能有效重投影时直接取上一帧颜色。**

trace stdout L207–292 从 half depth / normal 重建接收点和世界方向，按帧变化的序列生成半球方向；L294–883 在屏幕深度上寻找交点。L887–901 要求 `history_valid`、非首帧、屏幕命中、`reprojection_tex.z > 0`，然后读取：

```glsl
candidate = vec4(textureLod(taa_history_tex, hit_uv + reprojection.xy, 0).rgb, hit_distance);
```

**已证实：屏幕失败后才走世界追踪。** stdout L909–923 初始化 throughput 为 1、初始 radiance 为 0，从屏幕追踪后的距离继续；循环条件为 `_1221 < 1u`，随后才执行 BVH / 高度图查询。

当前编译变体只有 **1 次世界追踪迭代**。三角形命中读取简化材质颜色、太阳与阴影；地形命中在末次直接查询 `terrain_lighting_texture`；未命中则取天空和艺术补光。虽然代码中保留生成下一跳方向和 throughput 的结构，这个常量上限不允许本帧继续第二次世界追踪。不能将保留的循环结构说成已经开启多次路径反弹。

相关证据：trace stdout L1429–1433 查询地形光照；L1626–1660 解码墙颜色或 packed RGB，L1795–1828 加入太阳项、更新方向、递增并退出；L1370–1397 为 miss 的天空/补光路径。原有 [terrain_lighting GLSL L259](</D:/MyProject/Tiny Glade/tmp/shaders/_terrain_lighting_texture.cs.hlsl_cs_main_0.cs_main.glsl:259>) 到 L264 明确将带遮挡的太阳项和天空项写到纹理。

**强推断：多反弹外观的一部分来自跨帧屏幕颜色反馈。** 若 TAA history 包含上一帧最终间接光，下一帧屏幕命中再取其颜色，就会形成多帧反馈。但未拿到 CPU render graph 和历史纹理的实际内容，不能确认反馈包含哪些后处理、精确反弹数或能量闭环。

**已证实：存在抑制高亮异常值的上限。** trace stdout L1834–1836 按最大 RGB 分量将候选 radiance 限到 10，保持 RGB 比例。它可以减少极亮样本噪声，但会损失强光能量，不能当作无偏面积光求解。

## 3. 稀疏采样与方向表示

**2026-09-28 补查：候选方向使用空间 Hilbert 索引与 R2 低差异序列。** 对同一 trace 二进制重新解码，stdout L236–272 将射线像素坐标映射到 64×64 Hilbert 顺序，再以 `frame_index % 64` 和每帧偏移 288 构造 R2 的二维样本，投影到半球。绑定中虽然有 `blue_noise_texture`，但此段实际候选方向公式不是逐帧独立 hash 随机，也不能仅凭绑定名说此段使用蓝噪声贴图。

**已证实：trace 一次计算后向 2×2 半分辨率 texel 写相同候选。** trace stdout L207 使用 `global_id * 2`，L1839–1845 把同一 radiance / direction 写入该 2×2 区域。结合现有 deferred 以 `pixel / 2` 取缓存，可强推断追踪密度约为每 4×4 最终像素一条候选射线；实际 dispatch 范围仍需帧捕获确认。

**已证实：每颜色四个球谐系数（L0+L1，共 12 个标量）。**

[eval 二进制](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_eval_screen_radiance.cs.hlsl_cs_main_0.bin>) `cs_main`，stdout L104–126：8 个邻域候选以螺旋布局取得，按深度相似、单通道遮蔽差异和 `dot(receiver_normal, ray_direction) > 0` 加权，再投影到：

```glsl
basis = vec4(0.282094985, ray_direction * 0.488602996) * 4;
sh_r += basis * radiance.r * weight;
sh_g += basis * radiance.g * weight;
sh_b += basis * radiance.b * weight;
```

三个 RGBA16F 区域横向排放。实际归一化还包含 `0.5 / sum_weights`；上述摘录仅展示表示方式，不作为可直接复制的完整 estimator。

这与只存一个 RGB 的面片照度不同：方向信息仍保留到最后，用当前像素法线计算响应。低阶 SH 本身是低频近似，不能保留任意尖锐方向分布，也不会自动补回光源附近缺失的空间梯度。

## 4. 为什么 `restir` 命名不能直接当结论

[update 二进制](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_update_gi_radiance.cs.hlsl_cs_main_0.bin>) `cs_main` 解码只有 73 行，main 在 stdout L66–72，关键行为完整如下：

```glsl
imageStore(restir_radiance_out_tex, pixel, texelFetch(candidate_radiance_tex, pixel, 0));
imageStore(restir_ray_dir_out_tex, pixel, vec4(candidate_hit.xyz, rotating_subpixel_depth));
// 自己的像素坐标 + 两个固定 half(1) 字段
imageStore(restir_reservoir_out_tex, pixel, packed_self_pixel_and_unit_weights);
```

没有读取上一帧 reservoir，没有候选竞争或 reservoir merge。eval 读取该单位权重后做 8 邻域加权累积到 SH；也不输出新的 reservoir。因此，这条已检查路径是**带 reservoir 命名的数据接口与稀疏候选共享**。游戏的灯笼直接光另有 `Reservoir1spp` 结构，它不是证明 GI 使用 ReSTIR 的证据。

## 5. 时空重建和墙角保护

**已证实：重投影、法线拒绝和最长 48 帧累积。**

2026-09-28 补查：reproject 内核使用线性 sampler 读取三个 SH 历史区域，并结合重投影有效 tap 信息调整采样位置；历史法线使用邻域 gather 判定，而不是只检查最近的一个历史 texel。对带抖动的半分辨率历史，正确的亚像素采样和遮挡拒绝是持续积累的必要条件。

[reproject 二进制](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_reproject_screen_radiance.cs.hlsl_cs_main_0.bin>) `cs_main` stdout L114–187：先检查 history 有效性，按 motion/reprojection 找历史位置，对历史中压缩保存的法线做一致性检查；通过后 `sample_count = clamp(previous_count + 1, 1, 48)`，以 `1 / sample_count` 混合当前 SH。无效历史不混入。另有 `allow_non_determinism` 条件控制的 5×5 邻域补洞支路，是否默认启用未知。

**已证实：3×3 引导滤波，年轻历史用更大跨度。**

[blur 二进制](</D:/MyProject/Tiny Glade/compiled-shaders/_gi_blur_screen_radiance.cs.hlsl_cs_main_0.bin>) `cs_main` stdout L96–121：历史样本数 >33 时步长为 1，否则为 3；邻域权重是相对深度差、遮蔽差和法线关系的乘积：

```text
depth: exp2(-min(30, 80 * normal_factor * abs(1 - neighbor_depth / center_depth)))
occlusion: exp2(-20 * abs(neighbor_occlusion - center_occlusion))
normal: 0.01 / (1 - 0.9 * dot(n0,n1)^2)^2
```

它不是普通 RGB 高斯模糊。不过法线项使用平方，因此单独这项不能区分相同方向和完全相反方向；深度与遮蔽等其它限制仍然必要，不能据此保证薄墙两侧永不混合。

**已证实：最后仍做深度引导的半分辨率上采样和逐像素法线求值。** 原有 [canopy deferred GLSL L299](</D:/MyProject/Tiny Glade/tmp/shaders/_deferred_canopy_light_sun_sky.raster.hlsl_b903e43ffb3da915.ps_main.glsl:299>) 到 L345 先取 3 个深度加权候选，权重过小时再取另外 3 个，归一化三组 vec4；L390–402 用真实像素法线求入射响应并乘接收材质。另有从低阶方向推导的非线性高阶形状修正，不等同完整 L2 九系数存储。[grass deferred L289](</D:/MyProject/Tiny Glade/tmp/shaders/_deferred_grass_light_sun_sky.raster.hlsl_b903e43ffb3da915.ps_main.glsl:289>) 到 L335 也有同类重建。

对墙角的意义是：**先避免混合几何上不相关的样本，再在目标像素重建方向光照**。这比增加所有位置的射线数更直接针对块状感。它仍依赖正确深度、正常法线和足够空间采样，不能用滤波掩盖几何缺失。

## 6. `ssgi` 实际是什么

[ssgi 二进制](</D:/MyProject/Tiny Glade/compiled-shaders/_ssgi_ssgi.cs.hlsl_cs_main_0.bin>) `cs_main` 解码 stdout L81–83 只有 depth / half normal 输入和 R16F 输出；L181–231 在屏幕深度两侧搜索地平线方向并积分；L233 写单通道、最低 0.03 的可见性数值。

**已证实：此变体不读取颜色、自发光或 radiance，也不输出 RGB。** 它是屏幕几何遮蔽信号，在 eval/blur 里作为边界引导。不要把它和 trace 中“命中屏幕后取 TAA radiance”的彩色屏幕 GI 混为一谈。

## 7. 自发光、灯笼与动态变化

**已证实：灯笼的可见发光和它照亮别人是分开的实现。**

- 原有 [lantern emissive fragment L155](</D:/MyProject/Tiny Glade/tmp/shaders/_nani_lantern_emissive.raster.hlsl_b903e43ffb3da915.ps_main.glsl:155>) 把颜色/强度/视线距离形状写入第二渲染目标。它证明可见发光，不证明网格本身自动成为 GI 面积光。
- [canopy deferred L154](</D:/MyProject/Tiny Glade/tmp/shaders/_deferred_canopy_light_sun_sky.raster.hlsl_b903e43ffb3da915.ps_main.glsl:154>) 绑定 `lantern_buf`、light grid 和 indices；L648–653 从 `intensity_color_id`、`position` 求光源贡献，后续有 `Reservoir1spp` 直接光采样。不要将其当作 GI reservoir。
- [PDB L85822](</D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt:85822>) 有 `maintain_lantern_lights`，[L47599](</D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt:47599>) 有 `nani_update_lantern_emissive`，也支持这两个分离的更新通路。

**强推断：可见自发光和灯笼照亮的表面可通过 TAA history 被下一帧屏幕 GI 再利用。** 是否包含这些目标及各分量的确切阶段，要靠 render graph / 帧捕获确认。

**未确认：任意屏外移动自发光网格的稳定 GI。** 已读 trace 的 fallback 绑定没有通用发光材质表、emissive triangle 表或 lantern buffer；实际命中着色是简化颜色+太阳/地形/天空。不能声称它像当前 Godot 自发光球那样明确积分屏外发光几何。灯笼的显式直接灯可以缓解小亮物体难采中的问题，但不等于自动自发光 GI。

**动态质量取舍：** 已证实 frame_index 改变采样，BVH 有 update/upload 符号，GI 有重投影/累积。固定相机有利于历史稳定；移动物体和移动光源需要迅速放弃错误历史，否则最多 48 帧累积会产生延迟。此处没有运行时测量，不能给出毫秒成本或断言实际拖影程度。

**开发者公开确认：最终 TAA 也承担 GI 降噪。** h3r2tic 在 [2024-09-14 的开发者回复](https://steamcommunity.com/app/2198150/discussions/0/6610810166138629225/) 中说明 TAA 为多个效果提供降噪和超采样，禁用后会出现大量闪烁与视觉噪声；后续回复明确包括 global illumination。TG 的柔和观感包含这一层重建，不能要求其原始单帧 GI 本来就无噪声。这与单独的 SH 历史累积是不同环节。

## 8. 艺术处理与颜色解释

原有 [canopy deferred L352](</D:/MyProject/Tiny Glade/tmp/shaders/_deferred_canopy_light_sun_sky.raster.hlsl_b903e43ffb3da915.ps_main.glsl:352>) 到 L353 从 `indirect_light_tint` 构造一个按亮度归一的颜色，再与 SH 的 L0 RGB 做系数约 1.17 的外插和 1.2 倍缩放。grass L327–328 同样有该逻辑。trace miss 路径也有 `fake_light_color`、`fake_light2_color` 和方向性补光。

**已证实：至少这些材质和环境路径并非纯粹物理运输。** 美术设定参与最终间接光颜色/对比度，不能仅凭 Tiny Glade 的柔和截图推断其 GI 无偏或多弹射数很高。

对用户看到的“黄灯把红墙照红”：这套方法仍然会乘接收材质颜色。SH、降噪或时空重建不会把红材质变成中性灰。应分别显示无材质的入射辐照度、最终材质响应和色调映射结果，判断到底是正常颜色反射、显示映射、艺术 tint，还是漏光。**不能通过强行去饱和掩盖 GI 错误。**

## 9. 可借鉴到 Godot topdown GI 的具体顺序

以下是针对本项目的建议，不是 Tiny Glade 已实现内容的额外断言。

1. **先保持高频光源照明与低频反弹分离。** 小球、墙上发光面附近的快速空间变化继续用显式面积/球体光照及精细可见性求解；平滑的后续反弹再走较粗缓存。不要让低阶 SH 或宽滤波承担高频近场全部照明。
2. **把最终接收着色从缓存位置解耦。** 每像素拿到实际法线、世界位置、面 ID、材质，再从可靠样本重建入射光；必要时比较 RGB 照度与 L0/L1 方向表示。SH 对朝向/法线细节有价值，但不会自动消除沿墙面方向的空间格子。
3. **在固定 topdown 下做有边界的重建。** 可以直接在已知接收面使用面 ID、精确可见性、位置/深度/法线作指导。Tiny Glade 的 depth/normal/AO 引导是可借鉴思路；本项目已知箱体表面拓扑，可用比屏幕 AO 更明确的“不能跨薄墙/不相关面”约束。
4. **仅对随机的反弹分量引入时间复用。** 运动自发光源附近缩短或重置历史，远处静态反弹才较长累积。不要让小球转移后留下 48 帧亮斑，也不要在灯关闭后保留残光。保留现有零历史数值路径用于正确性对照。
5. **不要照抄屏幕颜色回灌或 reservoir 名称。** 当前项目要求屏外光源稳定、固定 topdown，世界空间传输有价值。Tiny Glade 的屏幕优先路径可以省射线，但会引入视角和历史依赖；是否值得引入应单独实验。当前证据也不支持仅因 `restir` 名称就改造成 ReSTIR。

后续验证重点应是中性墙颜色对照、移动光球在角落/薄墙两侧的能量和梯度、关灯归零、相机不动/运动对照，以及同一几何的高采样参考。不要只凭更模糊、更亮或更饱和的截图判定 GI 改善。

## 复现本轮静态解码

以下 Python 片段只读一个 `.bin`，将已有 SPIR-V 送入本机 RenderDoc 附带工具并打印 stdout；不创建解码文件。对本报告所列内核已验证 SPIR-V 起始偏移为 23 字节，其前 4 字节是字数。

```python
from pathlib import Path
import struct
import subprocess

path = Path(r"D:\MyProject\Tiny Glade\compiled-shaders\_gi_trace_gi_candidates.cs.hlsl_cs_main_0.bin")
data = path.read_bytes()
start = data.find(bytes.fromhex("03022307"))
words = struct.unpack_from("<I", data, start - 4)[0]
result = subprocess.run(
    [r"C:\Program Files\RenderDoc\plugins\spirv\spirv-cross.exe", "-", "--vulkan-semantics"],
    input=data[start:start + words * 4], capture_output=True, check=True,
)
for line_number, line in enumerate(result.stdout.decode().splitlines(), 1):
    print(f"{line_number}: {line}")
```

本轮没有完成 CPU 反编译、完整运行时帧图重建、性能测量或原游戏薄墙/动态发光测试；这些均不在“已证实”范围内。
