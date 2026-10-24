// TG-style sparse screen / BVH GI for TGScreenGI (modules/tg_gi), addressed from C++ as "/Project/tg/TGScreenGI.glsl".
// TG-inspired reconstruction, not a port of proprietary shader code.
// One entry point per pass; each declares only the resources it reads or writes, so RDG sees the real per-pass
// dependencies. Parameters bind by name to FTGScreenGIParameters (C++), the frame constants come from the TGView
// uniform buffer, the per-dispatch values from the push constants.

layout(local_size_x = 8, local_size_y = 8) in;

// Permutation dimensions of the C++ shader classes; 0 when the current shader class does not have them.
#ifndef TG_SPARSE
#define TG_SPARSE 0
#endif
#ifndef TG_ADAPTIVE
#define TG_ADAPTIVE 0
#endif
#ifndef TG_WORLD_CACHE
#define TG_WORLD_CACHE 0
#endif
#ifndef TG_SURFACE_HISTORY
#define TG_SURFACE_HISTORY 0
#endif
#ifndef FILTER_PASS
#define FILTER_PASS 0
#endif

#if defined(TGGuideCS) || defined(TGBudgetCS) || defined(TGTraceCS) || defined(TGReconstructCS) || defined(TGBasicFilterCS) || defined(TGFilterCS) || defined(TGHistoryCS) || defined(TGCacheSelectCS)
layout(set = 0, binding = 0, std140) uniform FTGViewUniformParameters {
	mat4 InvViewProj;
	mat4 ViewProj;
	mat4 CameraToWorld;
	mat4 WorldToView;
	mat4 PrevViewProj;
	mat4 PrevInvViewProj;
	vec4 SunDirection;
	vec4 SunColor;
	vec4 LampPosition;
	vec4 LampColor;
	vec4 BoxMin;
	vec4 BoxMax;
	vec4 BoxColor;
	vec4 SkyRadiance;
} TGView;
#endif

layout(push_constant, std430) uniform FParameters {
	uint Frame; uint Width; uint Height; uint HalfWidth;
	uint HalfHeight; uint NodeCount; uint HistoryValid; uint ScreenTrace;
	uint MaxHistory; uint StableDenoise; uint ReactiveFrames; uint TraceStride;
	uint BaseRays; uint FreshRays; uint SamplingProfile; uint PostDenoise;
	uint AdaptivePriority; uint SurfaceHistory; uint RadianceTemporalOk; uint WorldCacheEnabled;
	uint WorldCacheEpoch; uint WorldCacheTick; uint WorldCacheWrite;
} p;

// Current guide buffers (written by the guide pass, read as images by everything after it).
#if !defined(TGHistoryCS) && !defined(TGProjectCS)
layout(rgba32f, set = 0, binding = 1) uniform image2D GuidePosition;
layout(rgba16f, set = 0, binding = 2) uniform image2D GuideNormal;
#endif

// Scene inputs from Godot's render buffers.
#if defined(TGGuideCS) || defined(TGTraceCS) || defined(TGHistoryCS)
layout(set = 0, binding = 3) uniform sampler2D DepthTexture;
#endif
#if defined(TGGuideCS) || defined(TGTraceCS)
layout(set = 0, binding = 4) uniform sampler2D NormalTexture;
#endif

// Previous frame's guides and GI statistics.
#if defined(TGBudgetCS) || defined(TGTraceCS) || defined(TGReconstructCS)
layout(set = 0, binding = 5) uniform sampler2D PrevPositionTexture;
layout(set = 0, binding = 6) uniform sampler2D PrevNormalTexture;
layout(set = 0, binding = 7) uniform sampler2D PrevAgeTexture;
#endif
#if defined(TGBudgetCS) || defined(TGReconstructCS)
layout(set = 0, binding = 8) uniform sampler2D PrevMomentsTexture;
#endif
#if defined(TGReconstructCS)
layout(set = 0, binding = 9) uniform sampler2D PrevSHTexture;
#endif

// Previous frame's radiance (TAA or surface history) for screen hits and the surface history reprojection.
#if defined(TGTraceCS) || (defined(TGHistoryCS) && TG_SURFACE_HISTORY)
layout(set = 0, binding = 10) uniform sampler2D PrevColorTexture;
layout(set = 0, binding = 11) uniform sampler2D PrevDepthTexture;
#endif

#if defined(TGBudgetCS) || defined(TGTraceCS) || defined(TGReconstructCS) || defined(TGCacheResolveCS)
layout(set = 0, binding = 12, std430) buffer Counters { uint counters[]; };
#endif

// Low two bits: ray count. Upper bits: priority reason, retained for diagnostics.
#if defined(TGBudgetCS) || (defined(TGTraceCS) && TG_ADAPTIVE)
layout(r8ui, set = 0, binding = 13) uniform uimage2D RayBudget;
#endif

#if defined(TGTraceCS) || defined(TGProjectCS) || (defined(TGReconstructCS) && TG_SPARSE)
layout(rgba16f, set = 0, binding = 14) uniform image2DArray CandidateColor;
layout(rgba16f, set = 0, binding = 15) uniform image2DArray CandidateDirection;
#endif
#if defined(TGProjectCS) || (defined(TGReconstructCS) && !TG_SPARSE)
layout(rgba16f, set = 0, binding = 16) uniform image2D CandidateSH;
#endif

#if defined(TGTraceCS)
struct Node { vec4 lo; vec4 hi; uvec4 data; };
struct Triangle { vec4 a; vec4 b; vec4 c; vec4 ua; vec4 ub; vec4 uc; };
struct Material { vec4 color; vec4 params; };
layout(set = 0, binding = 17, std430) readonly buffer Nodes { Node nodes[]; };
layout(set = 0, binding = 18, std430) readonly buffer Triangles { Triangle triangles[]; };
layout(set = 0, binding = 19, std430) readonly buffer Materials { Material materials[]; };
layout(set = 0, binding = 20) uniform sampler2DArray AlbedoTexture;
layout(set = 0, binding = 21) uniform sampler2DArray MetalTexture;
#endif

#if defined(TGReconstructCS) || defined(TGBasicFilterCS) || (defined(TGFilterCS) && FILTER_PASS == 3) || defined(TGCacheSelectCS)
layout(r16f, set = 0, binding = 22) uniform image2D AgeOutput;
#endif
#if defined(TGReconstructCS) || defined(TGBasicFilterCS)
layout(rgba16f, set = 0, binding = 23) uniform image2D SHOutput;
#endif
#if defined(TGReconstructCS) || (defined(TGFilterCS) && FILTER_PASS == 3)
layout(rg32f, set = 0, binding = 24) uniform image2D MomentsOutput;
#endif

// Post denoise: FilterInput / VarianceInput -> FilterOutput / VarianceOutput, bound per level by the C++ side.
#if defined(TGFilterCS)
layout(rgba16f, set = 0, binding = 25) uniform image2D FilterInput;
layout(rgba16f, set = 0, binding = 26) uniform image2D FilterOutput;
#if FILTER_PASS != 3
layout(r32f, set = 0, binding = 27) uniform image2D VarianceInput;
#endif
#if FILTER_PASS != 8
layout(r32f, set = 0, binding = 28) uniform image2D VarianceOutput;
#endif
#endif

#if defined(TGBasicFilterCS) || (defined(TGFilterCS) && FILTER_PASS == 8) || defined(TGCacheResolveCS)
#if defined(TGCacheResolveCS)
layout(rgba16f, set = 0, binding = 29) uniform image2D FilteredOutput;
#elif defined(TGBasicFilterCS)
layout(rgba16f, set = 0, binding = 29) uniform writeonly image2D FilteredOutput;
#endif
#endif
#if defined(TGBasicFilterCS) || (defined(TGFilterCS) && FILTER_PASS == 8)
layout(rgba32f, set = 0, binding = 30) uniform writeonly image2D DisplayPosition;
layout(rgba16f, set = 0, binding = 31) uniform writeonly image2D DisplayNormal;
#endif

#if defined(TGHistoryCS)
layout(set = 0, binding = 32) uniform sampler2D SceneColorTexture;
layout(rgba16f, set = 0, binding = 33) uniform writeonly image2D ColorHistoryOutput;
layout(r32f, set = 0, binding = 34) uniform writeonly image2D DepthHistoryOutput;
#endif

#if defined(TGCacheSelectCS) || defined(TGCacheResolveCS)
layout(set = 0, binding = 36, std430) buffer WorldCacheOwners { uint world_owners[]; };
#endif
// The cache key / hash functions, plus WorldCache (binding 35) and the lookup for the passes that read or write entries.
#if defined(TGCacheSelectCS)
#define WORLD_CACHE_KEYS_ONLY
#endif
#if defined(TGCacheSelectCS) || defined(TGCacheResolveCS) || (defined(TGTraceCS) && TG_WORLD_CACHE)
#include "../tg_world_cache.glslinc"
#endif

const float PI=3.14159265359;
shared uint trace_counts[16];
shared float tile_priority[64];
vec3 unproject(vec2 uv,float depth,mat4 inverse_matrix) {
    vec4 h=inverse_matrix*vec4(uv*2.0-1.0,depth,1.0);
    return h.xyz/h.w;
}
vec2 project(vec3 point,mat4 matrix) {
    vec4 h=matrix*vec4(point,1.0);
    return h.w>0.0 ? h.xy/h.w*0.5+0.5 : vec2(-1.0);
}
bool on_screen(vec2 uv) { return all(greaterThanEqual(uv,vec2(0.0))) && all(lessThan(uv,vec2(1.0))); }
uint hash(uint x) { x^=x>>16u; x*=0x7feb352du; x^=x>>15u; x*=0x846ca68bu; return x^(x>>16u); }
float guide_weight(vec4 center,vec4 n,vec4 other,vec4 other_n) {
    if (other.w<=0.0) return 0.0;
    float normal_agreement=max(dot(n.xyz,other_n.xyz),0.0);
    vec3 delta=other.xyz-center.xyz;
    float plane=max(abs(dot(delta,n.xyz)),abs(dot(delta,other_n.xyz)));
    float scale=0.04+0.006*center.w;
    return pow(normal_agreement,12.0)*exp(-plane/scale)*exp(-abs(center.w-other.w)/(0.3+0.05*center.w))*exp(-12.0*abs(n.w-other_n.w));
}
float sh_luminance(vec4 red,vec4 green,vec4 blue) {
    return max(dot(vec3(red.x,green.x,blue.x),vec3(0.2126,0.7152,0.0722))*0.282094792,0.0);
}

#if defined(TGGuideCS) || defined(TGTraceCS)
vec3 normal_at(ivec2 xy) {
    return normalize(mat3(TGView.CameraToWorld)*normalize(texelFetch(NormalTexture,xy,0).xyz*2.0-1.0));
}
#endif

#if defined(TGGuideCS)
vec3 world_at(ivec2 pixel,vec3 fallback) {
    pixel=clamp(pixel,ivec2(0),ivec2(p.Width-1u,p.Height-1u));
    float depth=texelFetch(DepthTexture,pixel,0).r;
    return depth>0.000001 ? unproject((vec2(pixel)+0.5)/vec2(p.Width,p.Height),depth,TGView.InvViewProj) : fallback;
}
vec3 surface_normal(ivec2 pixel,vec3 world,vec3 shading_normal) {
    // A brick normal map must not split a flat wall into unrelated GI history.
    // Choose the nearer one-sided derivative to avoid crossing silhouettes.
    vec3 left=world-world_at(pixel-ivec2(1,0),world),right=world_at(pixel+ivec2(1,0),world)-world;
    vec3 up=world-world_at(pixel-ivec2(0,1),world),down=world_at(pixel+ivec2(0,1),world)-world;
    vec3 dx=dot(left,left)<dot(right,right) ? left : right;
    vec3 dy=dot(up,up)<dot(down,down) ? up : down;
    vec3 normal=cross(dx,dy);
    if (dot(normal,normal)<1e-14) return shading_normal;
    normal=normalize(normal);
    return dot(normal,shading_normal)<0.0 ? -normal : normal;
}
void TGGuideCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    if (any(greaterThanEqual(xy,half_size))) return;
    ivec2 full_xy=min(xy*2+1,ivec2(p.Width-1u,p.Height-1u));
    float depth=texelFetch(DepthTexture,full_xy,0).r;
    vec4 position=vec4(0.0),normal=vec4(0.0);
    if (depth>0.000001) {
        vec2 uv=(vec2(full_xy)+0.5)/vec2(p.Width,p.Height);
        vec3 world=unproject(uv,depth,TGView.InvViewProj),n=normal_at(full_xy);
        if (p.StableDenoise!=0u) n=surface_normal(full_xy,world,n);
        float occlusion=0.0;
        for (int i=0;i<8;i++) {
            float phi=float(i)*PI*0.25;
            ivec2 neighbor=clamp(full_xy+ivec2(vec2(cos(phi),sin(phi))*12.0),ivec2(0),ivec2(p.Width-1u,p.Height-1u));
            float d=texelFetch(DepthTexture,neighbor,0).r;
            if (d<=0.000001) continue;
            vec3 delta=unproject((vec2(neighbor)+0.5)/vec2(p.Width,p.Height),d,TGView.InvViewProj)-world;
            occlusion+=max(dot(n,normalize(delta+vec3(1e-8)))-0.08,0.0)*exp(-length(delta)*2.0);
        }
        position=vec4(world,-(TGView.WorldToView*vec4(world,1.0)).z);
        normal=vec4(n,occlusion/8.0);
    }
    imageStore(GuidePosition,xy,position); imageStore(GuideNormal,xy,normal);
}
#endif

#if defined(TGTraceCS)
float trace_history_age(vec4 position,vec3 normal) {
    if (p.HistoryValid==0u) return 0.0;
    vec2 uv=project(position.xyz,TGView.PrevViewProj);
    if (!on_screen(uv)) return 0.0;
    vec2 coordinate=uv*vec2(p.Width,p.Height)*0.5-0.75;
    ivec2 base=ivec2(floor(coordinate));
    float age=0.0;
    for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
        ivec2 tap=base+ivec2(i,j);
        if (any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,ivec2(p.HalfWidth,p.HalfHeight)))) continue;
        vec4 old_p=texelFetch(PrevPositionTexture,tap,0);
        vec3 old_n=texelFetch(PrevNormalTexture,tap,0).xyz,delta=old_p.xyz-position.xyz;
        float plane=max(abs(dot(delta,normal)),abs(dot(delta,old_n)));
        if (old_p.w>0.0 && plane<0.015+position.w*0.0015 && length(delta)<0.04+position.w*0.035 && dot(old_n,normal)>0.90)
            age=max(age,texelFetch(PrevAgeTexture,tap,0).r);
    }
    return age;
}
#endif

#if defined(TGBudgetCS)
vec3 trace_history_statistics(vec4 position,vec3 normal) {
    if (p.HistoryValid==0u || p.MaxHistory<=1u) return vec3(0.0);
    vec2 uv=project(position.xyz,TGView.PrevViewProj);
    if (!on_screen(uv)) return vec3(0.0);
    vec2 coordinate=uv*vec2(p.Width,p.Height)*0.5-0.75,fraction=fract(coordinate);
    ivec2 base=ivec2(floor(coordinate));
    vec3 statistics=vec3(0.0); float total=0.0;
    for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
        ivec2 tap=base+ivec2(i,j);
        if (any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,ivec2(p.HalfWidth,p.HalfHeight)))) continue;
        vec4 old_p=texelFetch(PrevPositionTexture,tap,0);
        vec3 old_n=texelFetch(PrevNormalTexture,tap,0).xyz,delta=old_p.xyz-position.xyz;
        float plane=max(abs(dot(delta,normal)),abs(dot(delta,old_n)));
        if (old_p.w<=0.0 || plane>0.015+position.w*0.0015 || length(delta)>0.04+position.w*0.035 || dot(old_n,normal)<0.90) continue;
        float weight=(i==0 ? 1.0-fraction.x : fraction.x)*(j==0 ? 1.0-fraction.y : fraction.y);
        statistics+=vec3(texelFetch(PrevAgeTexture,tap,0).r,texelFetch(PrevMomentsTexture,tap,0).xy)*weight;
        total+=weight;
    }
    return total>0.001 ? statistics/total : vec3(0.0);
}
void TGBudgetCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    uint lane=gl_LocalInvocationIndex;
    if (lane<8u) trace_counts[lane]=0u;
    ivec2 trace_size=imageSize(RayBudget);
    bool inside=all(lessThan(xy,trace_size));
    vec4 position=inside ? imageLoad(GuidePosition,min(xy*2,half_size-1)) : vec4(0.0);
    bool valid=position.w>0.0;
    float priority=valid ? 0.0 : -1.0;
    uint reason=0u;
    if (valid) {
        vec3 normal=imageLoad(GuideNormal,min(xy*2,half_size-1)).xyz;
        vec3 history=trace_history_statistics(position,normal);
        float variance=max(history.z-history.y*history.y,0.0);
        // Relative standard error of the accumulated estimate; the absolute
        // floor prevents almost-black surfaces from consuming the whole tile.
        float error=sqrt(variance/max(history.x,1.0))/(0.025+history.y);
        if (error>0.15) { priority=min(error,1.5); reason=2u; }
        if (p.ReactiveFrames>0u && length(position.xyz-TGView.LampPosition.xyz)<TGView.LampPosition.w*1.25) { priority=2.0; reason=3u; }
        if (history.x<8.0) { priority=3.0+(8.0-history.x)/8.0; reason=1u; }
        if (p.AdaptivePriority==0u) { priority=1.0; reason=0u; }
    }
    tile_priority[lane]=priority;
    barrier();
    // Each 8x8 trace tile spends at most floor(valid_receivers/2) extra rays.
    // Rank using history only, before observing any current-frame radiance.
    // A rotating permutation breaks equal-score ties without fixed stripes.
    uint rank=0u,receivers=0u;
    uint permutation=hash(p.Frame+gl_WorkGroupID.x*9781u+gl_WorkGroupID.y*6271u)&63u;
    for (uint other=0u;other<64u;other++) {
        float score=tile_priority[other];
        if (score>=0.0) receivers++;
        if (score>priority || (score==priority && (other^permutation)<(lane^permutation))) rank++;
    }
    bool extra=priority>0.0 && rank<receivers/2u;
    if (extra) {
        atomicAdd(trace_counts[4],1u);
        if (reason>0u) atomicAdd(trace_counts[reason],1u);
    }
    if (inside) imageStore(RayBudget,xy,uvec4((valid ? 1u+uint(extra) : 0u)|(reason<<2u)));
    barrier();
    if (lane==0u) {
        atomicAdd(counters[7],trace_counts[4]);
        atomicAdd(counters[8],receivers);
        atomicAdd(counters[9],trace_counts[1]);
        atomicAdd(counters[10],trace_counts[2]);
        atomicAdd(counters[11],trace_counts[3]);
    }
}
#endif

#if defined(TGTraceCS)
vec3 linear_rgb(vec3 rgb) { return mix(rgb/12.92,pow((rgb+0.055)/1.055,vec3(2.4)),greaterThan(rgb,vec3(0.04045))); }
float random_float(uint x) { return float(hash(x)>>8u)/16777216.0; }
// Spatial Hilbert order + temporally shifted R2 sequence. The TG trace variant
// uses this family of stratification instead of independent per-frame hashes.
vec2 sample_sequence(uvec2 pixel) {
    uvec2 cell=pixel&uvec2(63u);
    uint index=0u;
    for (uint scale=32u;scale>0u;scale>>=1u) {
        uint rx=uint((cell.x&scale)!=0u),ry=uint((cell.y&scale)!=0u);
        index+=scale*scale*((3u*rx)^ry);
        if (ry==0u) {
            if (rx!=0u) cell=uvec2(63u)-cell;
            cell=cell.yx;
        }
    }
    return fract(vec2(0.5)+vec2(0.754877666,0.569840296)*float(index+288u*(p.Frame%64u)));
}
bool aabb(vec3 origin,vec3 inv_dir,vec3 lo,vec3 hi,float distance) {
    vec3 a=(lo-origin)*inv_dir,b=(hi-origin)*inv_dir;
    vec3 n=min(a,b),z=max(a,b);
    return max(max(n.x,n.y),max(n.z,0.001))<=min(min(z.x,z.y),min(z.z,distance));
}
int trace(vec3 origin,vec3 direction,inout float distance,out vec2 bary,bool any_hit) {
    vec3 inv_dir=1.0/mix(vec3(1e-20),direction,greaterThan(abs(direction),vec3(1e-20)));
    int found=-1;
    if (TGView.BoxMin.w>0.5 && aabb(origin,inv_dir,TGView.BoxMin.xyz,TGView.BoxMax.xyz,distance)) {
        vec3 a=(TGView.BoxMin.xyz-origin)*inv_dir,b=(TGView.BoxMax.xyz-origin)*inv_dir;
        vec3 n=min(a,b),z=max(a,b);
        float near_t=max(max(n.x,n.y),n.z),far_t=min(min(z.x,z.y),z.z);
        float t=near_t>0.001 ? near_t : far_t;
        if (t>0.001 && t<distance) { distance=t; found=-2; if (any_hit) return found; }
    }
    uint index=0u;
    while (index<p.NodeCount) {
        Node node=nodes[index];
        if (!aabb(origin,inv_dir,node.lo.xyz,node.hi.xyz,distance)) { index=node.data.z; continue; }
        if (node.data.y==0u) { index++; continue; }
        for (uint i=0u;i<node.data.y;i++) {
            uint id=node.data.x+i; Triangle triangle=triangles[id];
            vec3 e1=triangle.b.xyz-triangle.a.xyz,e2=triangle.c.xyz-triangle.a.xyz;
            vec3 h=cross(direction,e2); float determinant=dot(e1,h);
            if (abs(determinant)<1e-9) continue;
            vec3 delta=origin-triangle.a.xyz;
            float u=dot(delta,h)/determinant;
            if (u<0.0 || u>1.0) continue;
            vec3 q=cross(delta,e1); float v=dot(direction,q)/determinant;
            if (v<0.0 || u+v>1.0) continue;
            float t=dot(e2,q)/determinant;
            if (t<=0.004 || t>=distance) continue;
            uint material_id=uint(triangle.a.w+0.5); Material material=materials[material_id];
            vec2 uv=triangle.ua.xy*(1.0-u-v)+triangle.ub.xy*u+triangle.uc.xy*v;
            if (material.params.x>=0.0 && textureLod(AlbedoTexture,vec3(uv,material_id),0.0).a*material.color.a<material.params.x) continue;
            distance=t; bary=vec2(u,v); found=int(id);
            if (any_hit) return found;
        }
        index=node.data.z;
    }
    return found;
}
bool visible(vec3 origin,vec3 direction,float distance) {
    vec2 bary; return trace(origin,direction,distance,bary,true)==-1;
}
vec3 shade_hit(int hit,vec2 bary,vec3 position,vec3 ray_direction) {
    vec3 normal,albedo;
    if (hit==-2) {
        vec3 middle=(TGView.BoxMin.xyz+TGView.BoxMax.xyz)*0.5;
        vec3 local=(position-middle)/((TGView.BoxMax.xyz-TGView.BoxMin.xyz)*0.5);
        vec3 a=abs(local);
        normal=a.x>max(a.y,a.z) ? vec3(sign(local.x),0,0) : (a.y>a.z ? vec3(0,sign(local.y),0) : vec3(0,0,sign(local.z)));
        albedo=TGView.BoxColor.rgb;
    } else {
        Triangle triangle=triangles[hit];
        normal=normalize(-cross(triangle.b.xyz-triangle.a.xyz,triangle.c.xyz-triangle.a.xyz));
        if (dot(normal,ray_direction)>=0.0) return vec3(0.0);
        uint id=uint(triangle.a.w+0.5); Material material=materials[id];
        vec2 uv=triangle.ua.xy*(1.0-bary.x-bary.y)+triangle.ub.xy*bary.x+triangle.uc.xy*bary.y;
        vec4 metal=textureLod(MetalTexture,vec3(uv,id),0.0);
        int channel=int(material.params.z+0.5);
        float metallic=(channel==0 ? metal.r : (channel==1 ? metal.g : (channel==2 ? metal.b : metal.a)))*material.params.y;
        albedo=linear_rgb(textureLod(AlbedoTexture,vec3(uv,id),0.0).rgb)*material.color.rgb*(1.0-metallic);
    }
    vec3 origin=position+normal*0.018;
    vec3 incoming=vec3(0.0);
    float cosine=max(dot(normal,TGView.SunDirection.xyz),0.0);
    if (cosine>0.0 && dot(TGView.SunColor.rgb,vec3(1.0))>0.0 && visible(origin,TGView.SunDirection.xyz,100.0)) incoming+=TGView.SunColor.rgb*cosine;
    vec3 delta=TGView.LampPosition.xyz-position;
    float distance=length(delta); cosine=max(dot(normal,delta/max(distance,0.001)),0.0);
    if (cosine>0.0 && distance<TGView.LampPosition.w && dot(TGView.LampColor.rgb,vec3(1.0))>0.0) {
        vec3 segment=TGView.LampPosition.xyz-origin;
        if (visible(origin,normalize(segment),length(segment)-0.005)) {
            float window=max(1.0-pow(distance/TGView.LampPosition.w,4.0),0.0);
            incoming+=TGView.LampColor.rgb*cosine*window*window*pow(max(distance,0.05),-TGView.LampColor.w);
        }
    }
#if TG_WORLD_CACHE
    if (hit>=0 && p.WorldCacheEnabled!=0u) {
        atomicAdd(trace_counts[13],1u);
        vec3 indirect;
        if (world_cache_lookup(position,normal,p.WorldCacheEpoch,p.WorldCacheTick,indirect)) {
            incoming+=indirect;
            atomicAdd(trace_counts[14],1u);
            vec4 clip=TGView.ViewProj*vec4(position,1.0);
            if (clip.w<=0.0 || !on_screen(clip.xy/clip.w*0.5+0.5)) atomicAdd(trace_counts[15],1u);
        }
    }
#endif
    return albedo*incoming;
}
bool screen_hit(vec3 origin,vec3 direction,out vec3 radiance) {
    // GLSL out arguments do not preserve the caller's initializer on failure.
    // A screen miss followed by a world miss must contribute exactly zero.
    radiance=vec3(0.0);
    if (p.HistoryValid==0u || p.ScreenTrace==0u) return false;
    float previous_gap=-1.0;
    for (int step=0;step<36;step++) {
        float distance=0.07*(pow(1.18,float(step+1))-1.0)+0.07;
        vec3 point=origin+direction*distance;
        vec2 uv=project(point,TGView.ViewProj);
        if (!on_screen(uv)) return false;
        float depth=textureLod(DepthTexture,uv,0.0).r;
        if (depth<=0.000001) continue;
        vec3 surface=unproject(uv,depth,TGView.InvViewProj);
        float ray_depth=-(TGView.WorldToView*vec4(point,1.0)).z;
        float surface_depth=-(TGView.WorldToView*vec4(surface,1.0)).z;
        float gap=ray_depth-surface_depth;
        if (gap>=0.0 && previous_gap<0.0 && gap<0.12+surface_depth*0.006 && distance>0.15) {
            ivec2 pixel=clamp(ivec2(uv*vec2(p.Width,p.Height)),ivec2(0),ivec2(p.Width-1u,p.Height-1u));
            if (dot(normal_at(pixel),direction)>-0.02) return false;
            vec2 old_uv=project(surface,TGView.PrevViewProj);
            if (!on_screen(old_uv)) return false;
            float old_depth=textureLod(PrevDepthTexture,old_uv,0.0).r;
            if (old_depth<=0.000001) return false;
            vec3 old_surface=unproject(old_uv,old_depth,TGView.PrevInvViewProj);
            if (length(old_surface-surface)>0.07+surface_depth*0.003) return false;
            vec4 cached=textureLod(PrevColorTexture,old_uv,0.0);
            if (p.SurfaceHistory!=0u && cached.a<0.999) return false;
            radiance=cached.rgb;
            return true;
        }
        previous_gap=gap;
    }
    return false;
}
vec3 trace_candidate(vec3 origin,vec3 normal,vec2 sample_uv,out vec3 direction) {
    float z=1.0-sample_uv.x,phi=sample_uv.y*2.0*PI;
    vec3 tangent=normalize(cross(abs(normal.y)<0.95 ? vec3(0,1,0) : vec3(1,0,0),normal));
    vec3 bitangent=cross(normal,tangent);
    direction=normal*z+sqrt(max(1.0-z*z,0.0))*(tangent*cos(phi)+bitangent*sin(phi));
    vec3 radiance=vec3(0);
    if (screen_hit(origin,direction,radiance)) atomicAdd(trace_counts[1],1u);
    else {
        atomicAdd(trace_counts[2],1u);
        bool sky_ray=direction.y>0.0 && any(greaterThan(TGView.SkyRadiance.rgb,vec3(0.0)));
        // An upward miss is sky only after testing the entire scene, including
        // occluders beyond the usual 80 m local GI range. Still one BVH query.
        float distance=sky_ray ? 1e20 : 80.0; vec2 bary;
        int hit=trace(origin,direction,distance,bary,false);
        if (hit!=-1) {
            atomicAdd(trace_counts[3],1u);
            if (hit==-2) atomicAdd(trace_counts[6],1u);
            radiance=shade_hit(hit,bary,origin+direction*distance,direction);
        } else if (sky_ray) {
            radiance=TGView.SkyRadiance.rgb;
            atomicAdd(trace_counts[12],1u);
        }
    }
    float peak=max(max(radiance.r,radiance.g),radiance.b);
    return radiance*min(1.0,10.0/max(peak,0.0001));
}
void TGTraceCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    uint lane=gl_LocalInvocationIndex,ray=gl_GlobalInvocationID.z;
    if (lane<16u) trace_counts[lane]=0u;
    barrier();
    ivec2 trace_size=(half_size+int(p.TraceStride)-1)/int(p.TraceStride);
    if (all(lessThan(xy,trace_size))) {
        ivec2 sample_xy=min(xy*int(p.TraceStride),half_size-1);
        vec4 position=imageLoad(GuidePosition,sample_xy),normal=imageLoad(GuideNormal,sample_xy);
        vec3 radiance=vec3(0.0),direction=vec3(0.0);
        uint rays=0u;
        if (position.w>0.0) {
            rays=p.BaseRays;
#if TG_ADAPTIVE
            if (p.SamplingProfile==3u) rays=imageLoad(RayBudget,xy).r&3u;
#endif
            if (p.SamplingProfile==1u && p.MaxHistory>1u && trace_history_age(position,normal.xyz)<8.0) {
                rays=p.FreshRays;
                if (ray==0u) atomicAdd(trace_counts[7],1u);
            }
            if (ray<rays) {
                atomicAdd(trace_counts[0],1u);
                uint seed=uint(xy.x+xy.y*trace_size.x)*9781u+p.Frame*6271u;
                vec2 jitter=p.StableDenoise!=0u ? sample_sequence(uvec2(xy)) : vec2(1.0-random_float(seed),random_float(seed+49u));
                uint columns=1u<<(uint(findMSB(rays))/2u),rows=rays/columns;
                vec2 sample_uv=(vec2(ray%columns,ray/columns)+jitter)/vec2(columns,rows);
                radiance=trace_candidate(position.xyz+normal.xyz*0.025,normal.xyz,sample_uv,direction);
            }
        }
        imageStore(CandidateColor,ivec3(xy,ray),vec4(radiance,ray<rays ? 1.0/float(rays) : 0.0));
        imageStore(CandidateDirection,ivec3(xy,ray),vec4(direction,float(rays)));
    }
    barrier();
    if (lane<16u && trace_counts[lane]>0u) atomicAdd(counters[lane],trace_counts[lane]);
}
#endif

#if defined(TGProjectCS)
void TGProjectCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    if (any(greaterThanEqual(xy,half_size))) return;
    ivec2 trace_size=imageSize(CandidateDirection).xy;
    if (any(greaterThanEqual(xy,trace_size))) return;
    uint rays=uint(imageLoad(CandidateDirection,ivec3(xy,0)).w);
    vec4 red=vec4(0),green=vec4(0),blue=vec4(0);
    for (uint ray=0u;ray<rays;ray++) {
        vec4 radiance=imageLoad(CandidateColor,ivec3(xy,ray));
        vec3 direction=imageLoad(CandidateDirection,ivec3(xy,ray)).xyz;
        vec4 basis=vec4(0.282094792,0.488602512*direction)*(2.0*PI*radiance.a);
        red+=basis*radiance.r; green+=basis*radiance.g; blue+=basis*radiance.b;
    }
    imageStore(CandidateSH,xy,red);
    imageStore(CandidateSH,xy+ivec2(trace_size.x,0),green);
    imageStore(CandidateSH,xy+ivec2(trace_size.x*2,0),blue);
}
#endif

#if defined(TGReconstructCS)
void TGReconstructCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    if (any(greaterThanEqual(xy,half_size))) return;
    vec4 center=imageLoad(GuidePosition,xy),n=imageLoad(GuideNormal,xy);
    vec4 red=vec4(0.0),green=vec4(0.0),blue=vec4(0.0); float weight_sum=0.0;
    ivec2 trace_size=(half_size+int(p.TraceStride)-1)/int(p.TraceStride);
    if (center.w>0.0) {
        for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
            // Keep the same spatial footprint as sparse mode for the A/B.
            // The rays, rather than the blur radius, supply the improvement.
            ivec2 sample_xy=clamp(xy/int(p.TraceStride)+ivec2(i,j)*(p.TraceStride==2u ? 1 : 2),ivec2(0),trace_size-1);
            ivec2 guide_xy=min(sample_xy*int(p.TraceStride),half_size-1);
            vec4 position=imageLoad(GuidePosition,guide_xy),normal=imageLoad(GuideNormal,guide_xy);
            float weight=guide_weight(center,n,position,normal);
#if !TG_SPARSE
            if (position.w<=0.0) weight=0.0;
            red+=imageLoad(CandidateSH,sample_xy)*weight;
            green+=imageLoad(CandidateSH,sample_xy+ivec2(trace_size.x,0))*weight;
            blue+=imageLoad(CandidateSH,sample_xy+ivec2(trace_size.x*2,0))*weight;
            weight_sum+=weight;
#else
            uint rays=uint(imageLoad(CandidateDirection,ivec3(sample_xy,0)).w);
            for (uint ray=0u;ray<rays;ray++) {
                vec4 candidate=imageLoad(CandidateColor,ivec3(sample_xy,ray));
                vec3 direction=imageLoad(CandidateDirection,ivec3(sample_xy,ray)).xyz;
                float ray_weight=weight*candidate.a;
                if (dot(n.xyz,direction)<=0.0) ray_weight=0.0;
                // Project each ray before averaging; averaging radiance
                // and direction separately would corrupt directional SH.
                vec4 basis=vec4(0.282094792,0.488602512*direction)*(2.0*PI);
                red+=basis*candidate.r*ray_weight; green+=basis*candidate.g*ray_weight; blue+=basis*candidate.b*ray_weight; weight_sum+=ray_weight;
            }
#endif
        }
    }
    if (weight_sum>0.00001) { red/=weight_sum; green/=weight_sum; blue/=weight_sum; }
    float luma=sh_luminance(red,green,blue);
    vec2 moments=vec2(luma,luma*luma),history_moments=vec2(0.0);
    float age=1.0;
    vec2 old_uv=project(center.xyz,TGView.PrevViewProj);
    bool valid=p.HistoryValid!=0u && center.w>0.0 && on_screen(old_uv);
    ivec2 old_xy=clamp(ivec2(old_uv*vec2(half_size)),ivec2(0),half_size-1);
    if (valid && p.StableDenoise==0u) {
        vec4 old_p=texelFetch(PrevPositionTexture,old_xy,0),old_n=texelFetch(PrevNormalTexture,old_xy,0);
        valid=old_p.w>0.0 && length(old_p.xyz-center.xyz)<0.06+center.w*0.004 && dot(old_n.xyz,n.xyz)>0.88;
    }
    vec4 history_r=vec4(0),history_g=vec4(0),history_b=vec4(0);
    float history_age=0.0,history_weight=0.0;
    if (valid && p.StableDenoise!=0u) {
        // Guides are sampled at full pixel 2*x+1, not the cell center.
        vec2 coordinate=old_uv*vec2(p.Width,p.Height)*0.5-0.75;
        ivec2 base=ivec2(floor(coordinate)); vec2 fraction=fract(coordinate);
        for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
            ivec2 tap=base+ivec2(i,j);
            if (any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,half_size))) continue;
            vec4 old_p=texelFetch(PrevPositionTexture,tap,0),old_n=texelFetch(PrevNormalTexture,tap,0);
            vec3 delta=old_p.xyz-center.xyz;
            float plane=max(abs(dot(delta,n.xyz)),abs(dot(delta,old_n.xyz)));
            if (old_p.w<=0.0 || plane>0.015+center.w*0.0015 || length(delta)>0.04+center.w*0.035 || dot(old_n.xyz,n.xyz)<0.90) continue;
            float weight=(i==0 ? 1.0-fraction.x : fraction.x)*(j==0 ? 1.0-fraction.y : fraction.y);
            history_r+=texelFetch(PrevSHTexture,tap,0)*weight;
            history_g+=texelFetch(PrevSHTexture,tap+ivec2(p.HalfWidth,0),0)*weight;
            history_b+=texelFetch(PrevSHTexture,tap+ivec2(p.HalfWidth*2u,0),0)*weight;
            history_age+=texelFetch(PrevAgeTexture,tap,0).r*weight;
            history_moments+=texelFetch(PrevMomentsTexture,tap,0).rg*weight;
            history_weight+=weight;
        }
        valid=history_weight>0.01;
    }
    if (valid) {
        if (p.StableDenoise==0u) {
            history_r=texelFetch(PrevSHTexture,old_xy,0);
            history_g=texelFetch(PrevSHTexture,old_xy+ivec2(p.HalfWidth,0),0);
            history_b=texelFetch(PrevSHTexture,old_xy+ivec2(p.HalfWidth*2u,0),0);
            history_age=texelFetch(PrevAgeTexture,old_xy,0).r; history_weight=1.0;
            history_moments=texelFetch(PrevMomentsTexture,old_xy,0).rg;
        }
        float limit=float(p.MaxHistory);
        if (p.ReactiveFrames>0u && distance(center.xyz,TGView.LampPosition.xyz)<TGView.LampPosition.w*1.25) limit=min(limit,8.0);
        age=min(history_age/history_weight+1.0,limit);
        float blend=1.0/age;
        moments=mix(history_moments/history_weight,moments,max(blend,0.2));
        red=mix(history_r/history_weight,red,blend);
        green=mix(history_g/history_weight,green,blend);
        blue=mix(history_b/history_weight,blue,blend);
        atomicAdd(counters[4],1u);
    } else if (center.w>0.0) atomicAdd(counters[5],1u);
    imageStore(SHOutput,xy,red); imageStore(SHOutput,xy+ivec2(p.HalfWidth,0),green); imageStore(SHOutput,xy+ivec2(p.HalfWidth*2u,0),blue);
    imageStore(AgeOutput,xy,vec4(center.w>0.0 ? age : 0.0));
    imageStore(MomentsOutput,xy,vec4(moments,0.0,0.0));
}
#endif

#if defined(TGBasicFilterCS)
// Basic filter (post denoise off): one guided 3x3 pass over the reconstructed SH.
void TGBasicFilterCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    if (any(greaterThanEqual(xy,half_size))) return;
    vec4 center=imageLoad(GuidePosition,xy),n=imageLoad(GuideNormal,xy);
    vec4 red=vec4(0),green=vec4(0),blue=vec4(0); float sum=0.0;
    int stride=imageLoad(AgeOutput,xy).r>33.0 ? 1 : 3;
    for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
        ivec2 other=clamp(xy+ivec2(i,j)*stride,ivec2(0),half_size-1);
        float weight=guide_weight(center,n,imageLoad(GuidePosition,other),imageLoad(GuideNormal,other));
        if (i==0 && j==0) weight=center.w>0.0 ? 1.0 : 0.0;
        red+=imageLoad(SHOutput,other)*weight;
        green+=imageLoad(SHOutput,other+ivec2(p.HalfWidth,0))*weight;
        blue+=imageLoad(SHOutput,other+ivec2(p.HalfWidth*2u,0))*weight; sum+=weight;
    }
    float scale=1.0/max(sum,0.00001);
    imageStore(FilteredOutput,xy,red*scale); imageStore(FilteredOutput,xy+ivec2(p.HalfWidth,0),green*scale); imageStore(FilteredOutput,xy+ivec2(p.HalfWidth*2u,0),blue*scale);
    imageStore(DisplayPosition,xy,center); imageStore(DisplayNormal,xy,n);
}
#endif

#if defined(TGFilterCS)
// Variance guided post denoise level FILTER_PASS (3: fine, 6: medium, 7: wide, 8: large -> final output).
vec4 filter_input(ivec2 xy) { return imageLoad(FilterInput,xy); }
float filter_variance(ivec2 xy) {
#if FILTER_PASS == 3
    vec2 moments=imageLoad(MomentsOutput,xy).xy;
    return max(moments.y-moments.x*moments.x,0.0)/max(imageLoad(AgeOutput,xy).r,1.0);
#else
    return imageLoad(VarianceInput,xy).r;
#endif
}
void TGFilterCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    if (any(greaterThanEqual(xy,half_size))) return;
    vec4 center=imageLoad(GuidePosition,xy),normal=imageLoad(GuideNormal,xy);
    ivec2 green_offset=ivec2(p.HalfWidth,0),blue_offset=green_offset*2;
    vec4 center_r=filter_input(xy),center_g=filter_input(xy+green_offset),center_b=filter_input(xy+blue_offset);
    float center_luma=sh_luminance(center_r,center_g,center_b);
    float variance=filter_variance(xy);
#if FILTER_PASS == 3
    {
        // A fresh surface has no temporal variance estimate. Bootstrap it
        // from compatible neighbors so isolated noisy candidates can smooth.
        float mean=0.0,second=0.0,total=0.0;
        for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
            ivec2 tap=clamp(xy+ivec2(i,j)*2,ivec2(0),half_size-1);
            float weight=guide_weight(center,normal,imageLoad(GuidePosition,tap),imageLoad(GuideNormal,tap));
            float luma=sh_luminance(filter_input(tap),filter_input(tap+green_offset),filter_input(tap+blue_offset));
            mean+=luma*weight; second+=luma*luma*weight; total+=weight;
        }
        mean/=max(total,0.00001); second/=max(total,0.00001);
        float young=1.0-clamp((imageLoad(AgeOutput,xy).r-1.0)/16.0,0.0,1.0);
        variance=max(variance,max(second-mean*mean,0.0)*max(young,0.1));
    }
#endif
#if FILTER_PASS == 3
    int stride=1;
#else
    int stride=1<<(FILTER_PASS-5);
#endif
    const float kernel[3]=float[](6.0,4.0,1.0);
    vec4 red=vec4(0),green=vec4(0),blue=vec4(0);
    float sum=0.0,variance_sum=0.0;
    for (int j=-2;j<=2;j++) for (int i=-2;i<=2;i++) {
        ivec2 tap=clamp(xy+ivec2(i,j)*stride,ivec2(0),half_size-1);
        vec4 position=imageLoad(GuidePosition,tap),n=imageLoad(GuideNormal,tap);
        vec3 delta=position.xyz-center.xyz;
        // Normal and two-sided plane tests protect corners and thin walls.
        float plane=max(abs(dot(delta,normal.xyz)),abs(dot(delta,n.xyz)));
        float weight=guide_weight(center,normal,position,n);
        if (dot(normal.xyz,n.xyz)<0.85 || plane>0.02+center.w*0.002) weight=0.0;
        vec4 r=filter_input(tap),g=filter_input(tap+green_offset),b=filter_input(tap+blue_offset);
        float neighbor_variance=filter_variance(tap);
        float sigma=6.0*sqrt(max(variance+neighbor_variance,0.0))+0.01+0.08*center_luma;
        weight*=exp(-abs(sh_luminance(r,g,b)-center_luma)/sigma)*kernel[abs(i)]*kernel[abs(j)];
        if (i==0 && j==0) weight=center.w>0.0 ? 36.0 : 0.0;
        red+=r*weight; green+=g*weight; blue+=b*weight; sum+=weight;
        variance_sum+=weight*weight*(i==0 && j==0 ? variance : neighbor_variance);
    }
    float inverse=1.0/max(sum,0.00001);
    imageStore(FilterOutput,xy,red*inverse); imageStore(FilterOutput,xy+green_offset,green*inverse); imageStore(FilterOutput,xy+blue_offset,blue*inverse);
#if FILTER_PASS != 8
    // Shared candidate/history samples are correlated: retain a conservative
    // variance floor rather than claiming every tap is an independent ray.
    float propagated=max(variance_sum*inverse*inverse,variance*0.5);
    imageStore(VarianceOutput,xy,vec4(propagated));
#else
    imageStore(DisplayPosition,xy,center); imageStore(DisplayNormal,xy,normal);
#endif
}
#endif

#if defined(TGHistoryCS)
// After the opaque pass: this frame's radiance and depth for next frame's screen hits.
void TGHistoryCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy,ivec2(p.Width,p.Height)))) return;
    float depth=texelFetch(DepthTexture,xy,0).r;
    vec4 color=texelFetch(SceneColorTexture,xy,0);
#if TG_SURFACE_HISTORY
    {
        // Opaque alpha resolves to coverage against the zero-alpha clear.
        // A mixed silhouette contains background, not pure surface light.
        if (depth<=0.000001 || color.a<0.999) { color=vec4(0.0); depth=0.0; }
        else if (p.HistoryValid!=0u && p.RadianceTemporalOk!=0u && p.StableDenoise!=0u) {
            vec3 world=unproject((vec2(xy)+0.5)/vec2(p.Width,p.Height),depth,TGView.InvViewProj);
            vec2 old_uv=project(world,TGView.PrevViewProj);
            if (on_screen(old_uv)) {
                vec2 coordinate=old_uv*vec2(p.Width,p.Height)-0.5,fraction=fract(coordinate);
                ivec2 base=ivec2(floor(coordinate));
                vec3 sum=vec3(0.0); float total=0.0;
                float distance=-(TGView.WorldToView*vec4(world,1.0)).z;
                for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
                    ivec2 tap=base+ivec2(i,j);
                    if (any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,ivec2(p.Width,p.Height)))) continue;
                    float old_depth=texelFetch(PrevDepthTexture,tap,0).r;
                    vec3 old_world=unproject((vec2(tap)+0.5)/vec2(p.Width,p.Height),old_depth,TGView.PrevInvViewProj);
                    if (old_depth<=0.000001 || length(old_world-world)>0.04+distance*0.003) continue;
                    float weight=(i==0 ? 1.0-fraction.x : fraction.x)*(j==0 ? 1.0-fraction.y : fraction.y);
                    sum+=texelFetch(PrevColorTexture,tap,0).rgb*weight; total+=weight;
                }
                if (total>0.001) {
                    // Short, independently reprojected surface history. Clamp
                    // to current radiance bounds to limit disocclusion trails.
                    vec3 lo=color.rgb,hi=color.rgb;
                    for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
                        ivec2 tap=clamp(xy+ivec2(i,j),ivec2(0),ivec2(p.Width-1u,p.Height-1u));
                        vec3 value=texelFetch(SceneColorTexture,tap,0).rgb;
                        lo=min(lo,value); hi=max(hi,value);
                    }
                    color.rgb=mix(clamp(sum/total,lo,hi),color.rgb,0.25);
                }
            }
        }
    }
#endif
    imageStore(ColorHistoryOutput,xy,color);
    imageStore(DepthHistoryOutput,xy,vec4(depth));
}
#endif

#if defined(TGCacheSelectCS)
void TGCacheSelectCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy),half_size=ivec2(p.HalfWidth,p.HalfHeight);
    if (any(greaterThanEqual(xy,half_size))) return;
    vec4 position=imageLoad(GuidePosition,xy),normal=imageLoad(GuideNormal,xy);
    if (position.w<=0.0 || imageLoad(AgeOutput,xy).r<8.0) return;
    // The live box is not a static cache receiver, nor is nearby geometry.
    if (TGView.BoxMin.w>0.5 && all(greaterThanEqual(position.xyz,TGView.BoxMin.xyz-vec3(0.025))) && all(lessThanEqual(position.xyz,TGView.BoxMax.xyz+vec3(0.025)))) return;
    ivec4 key=world_cache_key(position.xyz,normal.xyz);
    vec3 offset=position.xyz/WORLD_CACHE_CELL-(vec3(key.xyz)+0.5);
    uint score=uint(clamp(dot(offset,offset)*(254.0/0.75),0.0,254.0));
    uint pixel=uint(xy.x)+uint(xy.y)*p.HalfWidth;
    // Prefer the sample nearest the world-cell center; pixel ID breaks ties.
    // A separate resolve pass gives each bucket exactly one writer.
    atomicMin(world_owners[world_cache_hash(key)],(score<<24u)|pixel);
}
#endif

#if defined(TGCacheResolveCS)
void TGCacheResolveCS() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy);
    uint slot=uint(xy.x)+uint(xy.y)*256u;
    if (slot>=WORLD_CACHE_SIZE) return;
    uint owner=world_owners[slot];
    world_owners[slot]=0xffffffffu;
    if (owner==0xffffffffu) return;
    uint pixel=owner&0xffffffu;
    ivec2 tap=ivec2(pixel%p.HalfWidth,pixel/p.HalfWidth);
    vec3 position=imageLoad(GuidePosition,tap).xyz,normal=imageLoad(GuideNormal,tap).xyz;
    vec4 basis=vec4(0.282094792,normal*0.325735008);
    vec3 value=max(vec3(dot(imageLoad(FilteredOutput,tap),basis),dot(imageLoad(FilteredOutput,tap+ivec2(p.HalfWidth,0)),basis),dot(imageLoad(FilteredOutput,tap+ivec2(2u*p.HalfWidth,0)),basis)),vec3(0.0));
    if (any(isnan(value)) || any(isinf(value))) return;
    WorldCacheEntry entry;
    entry.cell=world_cache_key(position,normal);
    entry.position_tick=vec4(position,uintBitsToFloat(p.WorldCacheTick));
    entry.normal_epoch=vec4(normal,uintBitsToFloat(p.WorldCacheEpoch));
    entry.irradiance=vec4(min(value,vec3(10.0)),0.0);
    world_cache[slot]=entry;
    atomicAdd(counters[16],1u);
}
#endif
