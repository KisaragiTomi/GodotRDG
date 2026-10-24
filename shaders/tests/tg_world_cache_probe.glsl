#[compute]
#version 450
layout(local_size_x=64) in;
#include "../tg_world_cache.glslinc"
struct Query { vec4 position; vec4 normal; uvec4 epoch_tick; };
layout(set=0,binding=0,std430) readonly buffer Queries { Query queries[]; };
layout(set=0,binding=1,std430) writeonly buffer Results { vec4 results[]; };
layout(push_constant,std430) uniform Parameters { uint count; uint unused0; uint unused1; uint unused2; } p;
void main() {
    uint index=gl_GlobalInvocationID.x;
    if (index>=p.count) return;
    Query query=queries[index];
    vec3 value;
    bool hit=world_cache_lookup(query.position.xyz,query.normal.xyz,query.epoch_tick.x,query.epoch_tick.y,value);
    results[index]=vec4(value,hit ? 1.0 : 0.0);
}
