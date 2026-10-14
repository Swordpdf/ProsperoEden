// SPDX-License-Identifier: GPL-3.0-or-later
// Generic validated atlas composition. No launcher-specific artwork or geometry is embedded here.
uint runtime_word(int index) {
    int width=textureSize(loading_art,0).x;
    uvec4 bytes=uvec4(round(texelFetch(loading_art,ivec2(index%width,index/width),0)*255.0));
    return bytes.r|(bytes.g<<8)|(bytes.b<<16)|(bytes.a<<24);
}
float runtime_number(int index) { return uintBitsToFloat(runtime_word(index)); }
vec3 runtime_rgb(uint word) { return vec3(float((word>>16)&255u),float((word>>8)&255u),float(word&255u))/255.0; }
vec3 runtime_loading_scene(vec2 pixel,vec2 size,float seconds) {
    float unit=max(size.y/1080.0,0.001);vec2 canvas=size/unit;
    vec2 point=vec2(pixel.x,size.y-pixel.y)/unit;
    bool calm=seconds>=1000.0;float time=calm?0.0:max(seconds,0.0);
    vec3 result=runtime_rgb(runtime_word(2));
    int count=min(int(runtime_word(1)),16);
    for(int layer=0;layer<count;++layer) {
        int offset=8+layer*24;
        vec4 rect=vec4(runtime_number(offset+1),runtime_number(offset+2),runtime_number(offset+3),runtime_number(offset+4));
        if(rect.x<0.0)rect.x+=canvas.x;if(rect.y<0.0)rect.y+=canvas.y;
        float period=runtime_number(offset+13);float phase=period>0.0?mod(time,period):time;
        float start=runtime_number(offset+16),end=runtime_number(offset+17);
        float move=calm || end<=start?1.0:smoothstep(start,end,phase);
        rect.y+=mix(runtime_number(offset+14),runtime_number(offset+15),move);
        float alpha=runtime_number(offset+10);
        if(!calm) {
            float fade_in=runtime_number(offset+18),out_start=runtime_number(offset+19),out_end=runtime_number(offset+20);
            if(fade_in>0.0)alpha*=smoothstep(0.0,fade_in,phase);
            if(out_end>out_start)alpha*=1.0-smoothstep(out_start,out_end,phase);
        }
        uint kind=runtime_word(offset);
        vec3 ink=runtime_rgb(runtime_word(offset+9));
        if(kind==1u && all(greaterThanEqual(point,rect.xy)) && all(lessThan(point,rect.xy+rect.zw))) {
            vec4 uv=vec4(runtime_number(offset+5),runtime_number(offset+6),runtime_number(offset+7),runtime_number(offset+8));
            vec2 at=uv.xy+((point-rect.xy)/rect.zw)*uv.zw;
            vec4 sample_color=textureLod(loading_art,at,0.0);
            result=mix(result,sample_color.rgb*ink,clamp(alpha*sample_color.a,0.0,1.0));
        } else if(kind==2u) {
            float radius=runtime_number(offset+11);vec2 q=abs(point-(rect.xy+rect.zw*.5))-rect.zw*.5+radius;
            float distance=length(max(q,0.0))+min(max(q.x,q.y),0.0)-radius;
            float coverage=1.0-smoothstep(-.5,.5,abs(distance)-runtime_number(offset+12)*.5);
            result=mix(result,ink,coverage*alpha);
        }
    }
    return clamp(result,0.0,1.0);
}
