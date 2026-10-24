#[compute]
#version 450
layout(local_size_x = 64) in;
struct Node { vec4 lo; vec4 hi; uvec4 data; };
struct Triangle { vec4 a; vec4 b; vec4 c; vec4 ua; vec4 ub; vec4 uc; };
struct Surfel { vec4 position; vec4 normal; vec4 albedo; };
struct Material { vec4 color; vec4 params; };
layout(set=0,binding=0,std430) readonly buffer Nodes { Node nodes[]; };
layout(set=0,binding=1,std430) readonly buffer Triangles { Triangle triangles[]; };
layout(set=0,binding=2,std430) readonly buffer Surfels { Surfel surfels[]; };
layout(set=0,binding=3,std430) buffer Hits { int hits[]; };
layout(set=0,binding=4,std430) readonly buffer Padding { int padding[]; };
layout(set=0,binding=5,std430) readonly buffer Materials { Material materials[]; };
layout(set=0,binding=6) uniform sampler2DArray albedo_textures;
layout(set=0,binding=7,std430) readonly buffer Lighting { vec4 sun_direction; vec4 sun_color; vec4 lamp_position; vec4 lamp_color; };
layout(rgba16f,set=0,binding=8) uniform readonly image2D previous_outgoing;
layout(rgba16f,set=0,binding=9) uniform writeonly image2D next_outgoing;
layout(rgba16f,set=0,binding=10) uniform image2D total_incoming;
layout(push_constant,std430) uniform Params { uint stage; uint bounce; uint count; uint rays; uint width; uint height; uint node_count; uint offset; } p;

ivec2 pixel(int id) { return ivec2(id % int(p.width), id / int(p.width)); }
bool intersects(vec3 origin, vec3 inverse_direction, Node node, float distance) {
    vec3 a = (node.lo.xyz - origin) * inverse_direction;
    vec3 b = (node.hi.xyz - origin) * inverse_direction;
    vec3 near_axis = min(a,b), far_axis = max(a,b);
    float near_t = max(max(near_axis.x,near_axis.y),max(near_axis.z,0.0001));
    float far_t = min(min(far_axis.x,far_axis.y),min(far_axis.z,distance));
    return near_t <= far_t;
}
int trace(vec3 origin, vec3 direction, inout float distance, out vec2 bary, bool any_hit) {
    vec3 inverse_direction = 1.0 / mix(vec3(1e-20),direction,greaterThan(abs(direction),vec3(1e-20)));
    uint index = 0u;
    int found = -1;
    while (index < p.node_count) {
        Node node = nodes[index];
        if (!intersects(origin,inverse_direction,node,distance)) { index=node.data.z; continue; }
        if (node.data.y == 0u) { index++; continue; }
        for (uint i=0u; i<node.data.y; i++) {
            uint triangle_id = node.data.x+i;
            Triangle triangle = triangles[triangle_id];
            vec3 edge1=triangle.b.xyz-triangle.a.xyz, edge2=triangle.c.xyz-triangle.a.xyz;
            vec3 h=cross(direction,edge2);
            float determinant=dot(edge1,h);
            if (abs(determinant)<1e-9) continue;
            vec3 delta=origin-triangle.a.xyz;
            float u=dot(delta,h)/determinant;
            if (u<0.0 || u>1.0) continue;
            vec3 q=cross(delta,edge1);
            float v=dot(direction,q)/determinant;
            if (v<0.0 || u+v>1.0) continue;
            float t=dot(edge2,q)/determinant;
            if (t<=0.001 || t>=distance) continue;
            uint material_id=uint(triangle.a.w+0.5);
            Material material=materials[material_id];
            if (material.params.x>=0.0) {
                vec2 uv=triangle.ua.xy*(1.0-u-v)+triangle.ub.xy*u+triangle.uc.xy*v;
                float opacity=textureLod(albedo_textures,vec3(uv,float(material_id)),0.0).a*material.color.a;
                if (opacity<material.params.x) continue;
            }
            distance=t; bary=vec2(u,v); found=int(triangle_id);
            if (any_hit) return found;
        }
        index=node.data.z;
    }
    return found;
}
bool visible(vec3 origin,vec3 direction,float distance) {
    vec2 bary;
    return trace(origin,direction,distance,bary,true)<0;
}
void main() {
    uint id=gl_GlobalInvocationID.x+p.offset;
    if (p.stage==3u) {
        if (id>=p.width*p.height) return;
        int target=padding[id];
        vec4 value=target<0 ? vec4(0.0) : imageLoad(total_incoming,pixel(target));
        imageStore(next_outgoing,pixel(int(id)),value);
        return;
    }
    if (id>=p.count) return;
    Surfel surfel=surfels[id];
    vec3 normal=normalize(surfel.normal.xyz);
    vec3 origin=surfel.position.xyz+normal*0.015;
    ivec2 atlas=pixel(int(surfel.position.w+0.5));
    if (p.stage==0u) {
        vec3 tangent=normalize(cross(abs(normal.y)<0.95 ? vec3(0,1,0) : vec3(1,0,0),normal));
        vec3 bitangent=cross(normal,tangent);
        float rotation=fract(float(id)*0.754877666)*6.283185307;
        for (uint sample_id=0u; sample_id<p.rays; sample_id++) {
            float u=(float(sample_id)+0.5)/float(p.rays);
            float phi=float(sample_id)*2.39996323+rotation;
            vec3 direction=tangent*(sqrt(u)*cos(phi))+bitangent*(sqrt(u)*sin(phi))+normal*sqrt(1.0-u);
            float distance=100.0; vec2 bary;
            int hit=trace(origin,direction,distance,bary,false);
            int target=-1;
            if (hit>=0) {
                Triangle triangle=triangles[hit];
                // Godot uses clockwise front faces. Back sides still occlude.
                vec3 geometric_normal=-cross(triangle.b.xyz-triangle.a.xyz,triangle.c.xyz-triangle.a.xyz);
                if (dot(geometric_normal,direction)<0.0) {
                    vec2 uv=triangle.ua.zw*(1.0-bary.x-bary.y)+triangle.ub.zw*bary.x+triangle.uc.zw*bary.y;
                    ivec2 coordinate=clamp(ivec2(uv*vec2(p.width,p.height)),ivec2(0),ivec2(p.width-1u,p.height-1u));
                    target=padding[coordinate.y*int(p.width)+coordinate.x];
                }
            }
            hits[id*p.rays+sample_id]=target;
        }
    } else if (p.stage==1u) {
        vec3 direct=vec3(0.0);
        float cosine=max(dot(normal,sun_direction.xyz),0.0);
        if (cosine>0.0 && any(greaterThan(sun_color.rgb,vec3(0.0))) && visible(origin,sun_direction.xyz,100.0)) direct+=sun_color.rgb*cosine;
        vec3 delta=lamp_position.xyz-surfel.position.xyz;
        float distance=length(delta);
        vec3 direction=delta/max(distance,0.0001);
        cosine=max(dot(normal,direction),0.0);
        if (distance<lamp_position.w && cosine>0.0 && any(greaterThan(lamp_color.rgb,vec3(0.0)))) {
            vec3 segment=lamp_position.xyz-origin;
            if (visible(origin,normalize(segment),length(segment)-0.001)) {
                float window=max(1.0-pow(distance/max(lamp_position.w,0.01),4.0),0.0);
                direct+=lamp_color.rgb*cosine*window*window*pow(max(distance,0.01),-lamp_color.w);
            }
        }
        imageStore(next_outgoing,atlas,vec4(surfel.albedo.rgb*direct,1.0));
    } else {
        vec3 incoming=vec3(0.0);
        for (uint sample_id=0u; sample_id<p.rays; sample_id++) {
            int hit=hits[id*p.rays+sample_id];
            if (hit>=0) incoming+=imageLoad(previous_outgoing,pixel(hit)).rgb;
        }
        incoming/=float(p.rays);
        vec3 previous=p.bounce==0u ? vec3(0.0) : imageLoad(total_incoming,atlas).rgb;
        imageStore(total_incoming,atlas,vec4(previous+incoming,1.0));
        imageStore(next_outgoing,atlas,vec4(surfel.albedo.rgb*incoming,1.0));
    }
}
