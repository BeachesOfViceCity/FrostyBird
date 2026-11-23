#version 450
layout(set=2,binding=0) uniform sampler2D lettering;
layout(std140,set=3,binding=0) uniform Frame {
    vec4 viewport,motion,meter,page,opening,completion;
} f;
layout(location=0) out vec4 outputColor;
float hash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}

// Draw monospaced bitmap digits and invented symbols, not randomized pixels.
// Each row is five bits; row zero occupies the low bits of the first word.
const uvec2 glyphs[20]=uvec2[](
    uvec2(0x2318c62eu,0x174u), uvec2(0x084210c4u,0x1f4u),
    uvec2(0x0888882eu,0x3e1u), uvec2(0x20c8410fu,0x1f0u),
    uvec2(0x3f294988u,0x108u), uvec2(0x20f0863fu,0x1f0u),
    uvec2(0x22f0842eu,0x1d1u), uvec2(0x0888883fu,0x084u),
    uvec2(0x22e8c62eu,0x1d1u), uvec2(0x20f8c62eu,0x1d0u),
    uvec2(0x228fa22eu,0x051u), uvec2(0x04447904u,0x042u),
    uvec2(0x248fe124u,0x121u), uvec2(0x042e9021u,0x3e4u),
    uvec2(0x228fe00eu,0x1f0u), uvec2(0x2823e942u,0x1c8u),
    uvec2(0x084e212eu,0x1c4u), uvec2(0x20e4111fu,0x107u),
    uvec2(0x144f842eu,0x241u), uvec2(0x2110fa21u,0x3f0u));
float character(vec2 space, int id) {
    vec2 q=space*vec2(8.,10.)-vec2(1.5,1.5);
    if(any(lessThan(q,vec2(0)))||any(greaterThanEqual(q,vec2(5.,7.)))) return 0.;
    ivec2 at=ivec2(q);
    uint mask=at.y<6?glyphs[id].x:glyphs[id].y;
    float bit=float((mask>>uint((at.y%6)*5+at.x))&1u);
    return bit*mix(.8,1.,smoothstep(.15,.5,fract(q.y)));
}

vec3 rain(vec2 px,float pitch,float t,float depth) {
    vec2 grid=px/vec2(pitch,pitch*1.35);
    float column=floor(grid.x),row=floor(grid.y);
    float seed=hash(vec2(column,depth)), speed=mix(4.,12.,seed)*depth;
    float cycle=37.+floor(17.*hash(vec2(column,4.)));
    float head=t*speed+seed*139.;
    float lag=mod(floor(head)-row,cycle);
    float trail=exp(-lag/mix(3.,9.,seed))*(1.-smoothstep(10.,22.,lag));
    float tick=floor(t*(.4+1.6*seed)+row*.07);
    int ch=int(20.*hash(vec2(column*3.+tick,row+depth*72.)));
    float cut=mix(.22,1.,step(.2,hash(vec2(column,row))));
    vec3 ink=mix(vec3(.015,.29,.115),vec3(.59,1.,.72),exp(-lag*1.6)*.8);
    return ink*trail*character(fract(grid),ch)*cut;
}

vec3 background(vec2 px,float t,float install) {
    vec2 uv=px/vec2(1280.,720.);
    // Menu: keep motion in the margin. Install: the rain extends across
    // the full drawable, lit by a soft sweep, with no panel behind it.
    float edge=1.-smoothstep(.24,.75,length((uv-.5)*vec2(.76,1.)));
    float sweep=.5+.5*sin(uv.x*3.5+uv.y*2.7+t*.33);
    vec3 black=vec3(.012,.026,.021);
    vec3 bg=black+vec3(.006,.028,.016)*edge;
    vec3 code=rain(px+vec2(64.,16.),11.,t,.69)*.50;
    code+=rain(px+vec2(8.,4.),17.,t,1.)*.76;
    float curtain=smoothstep(.58,1.03,uv.x)*(.34+.46*(1.-smoothstep(.05,.65,uv.y)));
    float intensity=mix(.06+curtain,.60+.26*edge+.12*sweep,install);
    bg+=code*intensity;
    if(install>0.){
        float horizon=exp(-pow((uv.y-.63-.065*sin(uv.x*4.5+t*.21))*4.2,2.));
        bg+=install*vec3(.012,.067,.034)*horizon*(.37+.63*edge);
        // Clear a quiet strip for the time/status line, without a hard panel.
        bg*=1.-install*.55*smoothstep(.64,1.,uv.y);
    }
    return bg;
}

void main(){
    vec2 uv=(gl_FragCoord.xy-f.viewport.xy)/f.viewport.zw,px=uv*vec2(1280,720);
    vec3 bg=background(px,f.motion.x,f.motion.y);
    vec2 samplePos=px;
    bool sameLayer=true;
    if(f.page.z>0.){
        if(px.x<f.page.x) samplePos.x-=f.opening.y;
        else if(px.y>=f.page.y){samplePos.y-=f.opening.w;sameLayer=samplePos.y>=f.page.y;}
        else samplePos.x-=f.opening.z;
        sameLayer=sameLayer&&(samplePos.x<f.page.x)==(px.x<f.page.x);
    }
    vec4 ink=vec4(0);
    if(sameLayer&&all(greaterThanEqual(samplePos,vec2(0)))&&all(lessThan(samplePos,vec2(1280,720))))
        ink=texture(lettering,samplePos/vec2(1280,720));
    vec3 result=bg*(1.-ink.a)+ink.rgb;
    vec2 b=(px-f.meter.xy)/f.meter.zw;
    if(f.motion.z>=0.&&all(greaterThanEqual(b,vec2(0)))&&all(lessThan(b,vec2(1)))){
        float aa=max(fwidth(b.x)*.5,.0001);
        float fill=f.motion.z==0.?0.:1.-smoothstep(f.motion.z-aa,f.motion.z+aa,b.x);
        result=mix(vec3(.10,.21,.15),vec3(.53,.88,.63),fill);
    }
    if(f.completion.x>=0.){
        float t=f.completion.x;
        float halo=(1.-smoothstep(0.,145.,min(min(px.x,1280.-px.x),min(px.y,720.-px.y))));
        result+=vec3(.035,.14,.064)*halo*smoothstep(0.,.13,t)*(1.-smoothstep(.18,.9,t));
    }
    outputColor=vec4(result*f.opening.x*(1.-f.motion.w),1);
}
