/* Synthetic programs only: cache mutation, shared constants and WARP execution. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "nv2a_vsh_interp.h"
#include "nv2a_vsh_cpu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while(0)
static void src(uint32_t w[4], int which, int mux, int reg, int swz)
{
    if (!which) { w[1]|=swz; w[2]|=(mux<<26)|(reg<<28); }
    else if (which==1) w[2]|=(swz<<17)|(mux<<11)|(reg<<13);
    else { w[2]|=(swz<<2)|(reg>>2); w[3]|=(mux<<28)|((reg&3)<<30); }
}
static void mov_output(uint32_t w[4], unsigned input, unsigned output, unsigned mask)
{
    memset(w,0,16); w[1]=(1u<<21)|(input<<9);
    src(w,0,2,0,0x1B); w[3]=(mask<<12)|(1u<<11)|(output<<3)|1u;
}
static int same(float a,float b)
{
    if (isnan(a)||isnan(b)) return isnan(a)&&isnan(b);
    if (isinf(a)||isinf(b)) return isinf(a)&&isinf(b)&&signbit(a)==signbit(b);
    return fabsf(a-b)<=1e-5f*fmaxf(1.0f,fabsf(b));
}
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11GeometryShader *gs;
static int gpu_run(const float input[16][4], float output[15])
{
    char hlsl[65536]; uint32_t used;
    ID3DBlob *code=NULL,*errors=NULL;
    ID3D11VertexShader *vs=NULL; ID3D11InputLayout *layout=NULL;
    ID3D11Buffer *vertices=NULL,*constants=NULL,*frame=NULL,*stream=NULL,*readback=NULL;
    D3D11_BUFFER_DESC bd={0}; D3D11_SUBRESOURCE_DATA data={0};
    D3D11_INPUT_ELEMENT_DESC elements[16]={0}; unsigned n=0, stride=sizeof(float)*64, offset=0;
    const float screen[8]={2.0f/640,2.0f/480,1.0f/1000,0,1,1,1,1};
    D3D11_MAPPED_SUBRESOURCE mapped; HRESULT hr; int ok=0;
    if (nv2a_vsh_emit_hlsl(hlsl,sizeof hlsl,&used)<0) goto done;
    hr=D3DCompile(hlsl,strlen(hlsl),"synthetic",NULL,NULL,"main","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if (FAILED(hr)) { if(errors) fprintf(stderr,"%s\n",(char *)ID3D10Blob_GetBufferPointer(errors)); goto done; }
    if (FAILED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&vs))) goto done;
    if (!used) used=1;
    for(unsigned a=0;a<16;a++) if(used&(1u<<a)) {
        elements[n].SemanticName="ATTR"; elements[n].SemanticIndex=a;
        elements[n].Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        elements[n].AlignedByteOffset=a*16; ++n;
    }
    if(FAILED(ID3D11Device_CreateInputLayout(device,elements,n,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&layout))) goto done;
    bd.ByteWidth=sizeof(float)*64; bd.Usage=D3D11_USAGE_IMMUTABLE; bd.BindFlags=D3D11_BIND_VERTEX_BUFFER; data.pSysMem=input;
    if(FAILED(ID3D11Device_CreateBuffer(device,&bd,&data,&vertices))) goto done;
    bd.ByteWidth=192*16; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; data.pSysMem=nv2a_vsh_constants(NULL);
    if(FAILED(ID3D11Device_CreateBuffer(device,&bd,&data,&constants))) goto done;
    bd.ByteWidth=sizeof screen; data.pSysMem=screen;
    if(FAILED(ID3D11Device_CreateBuffer(device,&bd,&data,&frame))) goto done;
    memset(&bd,0,sizeof bd); bd.ByteWidth=sizeof(float)*15; bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;
    if(FAILED(ID3D11Device_CreateBuffer(device,&bd,NULL,&stream))) goto done;
    bd.Usage=D3D11_USAGE_STAGING; bd.BindFlags=0; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(ID3D11Device_CreateBuffer(device,&bd,NULL,&readback))) goto done;
    ID3D11DeviceContext_IASetInputLayout(context,layout);
    ID3D11DeviceContext_IASetVertexBuffers(context,0,1,&vertices,&stride,&offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(context,vs,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(context,1,1,&constants);
    ID3D11DeviceContext_VSSetConstantBuffers(context,2,1,&frame);
    ID3D11DeviceContext_GSSetShader(context,gs,NULL,0);
    ID3D11DeviceContext_SOSetTargets(context,1,&stream,&offset);
    ID3D11DeviceContext_Draw(context,1,0);
    ID3D11DeviceContext_SOSetTargets(context,0,NULL,NULL);
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)readback,(ID3D11Resource *)stream);
    if(FAILED(ID3D11DeviceContext_Map(context,(ID3D11Resource *)readback,0,D3D11_MAP_READ,0,&mapped))) goto done;
    memcpy(output,mapped.pData,sizeof(float)*15); ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)readback,0); ok=1;
done:
#define RELEASE(p) if(p) IUnknown_Release((IUnknown *)p)
    RELEASE(readback); RELEASE(stream); RELEASE(frame); RELEASE(constants); RELEASE(vertices); RELEASE(layout); RELEASE(vs); RELEASE(errors); RELEASE(code);
    return ok;
}
static int start_warp(void)
{
    ID3DBlob *code=NULL,*errors=NULL; HRESULT hr; unsigned stride=15*sizeof(float);
    const char *source="struct O {float4 p:SV_POSITION;float4 d:COLOR0;float4 s:COLOR1;float3 t:TEXCOORD0;}; [maxvertexcount(1)] void main(point O i[1],inout PointStream<O> o){o.Append(i[0]);}";
    const D3D11_SO_DECLARATION_ENTRY decl[]={{0,"SV_POSITION",0,0,4,0},{0,"COLOR",0,0,4,0},{0,"COLOR",1,0,4,0},{0,"TEXCOORD",0,0,3,0}};
    hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&device,NULL,&context);
    if(FAILED(hr)) return 0;
    hr=D3DCompile(source,strlen(source),"capture",NULL,NULL,"main","gs_5_0",0,0,&code,&errors);
    if(SUCCEEDED(hr)) hr=ID3D11Device_CreateGeometryShaderWithStreamOutput(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),decl,4,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,NULL,&gs);
    RELEASE(code); RELEASE(errors); return SUCCEEDED(hr);
}
int main(void)
{
    uint32_t w[4],revision,start,version,changed,hash; char hlsl[65536];
    float input[16][4]={{0}}, gpu[15]; Nv2aVshOutput cpu; NvVshOutput ready;
    CHECK(start_warp()); if(!device||!gs) return 1;
    input[0][0]=480;input[0][1]=180;input[0][2]=125;input[0][3]=2;
    mov_output(w,0,0,15); nv2a_vsh_set_instruction(0,w);
    CHECK(nv2a_vsh_inputs_used(NULL)==1); hash=nv2a_vsh_program_hash();
    CHECK(nv2a_vsh_run(input,&cpu)); CHECK(nv2a_vsh_run_ready(input,&ready));
    CHECK(!memcmp(cpu.pos,ready.pos,sizeof cpu.pos)); CHECK(gpu_run(input,gpu));
    CHECK(same(gpu[0],1)&&same(gpu[1],0.5f)&&same(gpu[2],0.25f)&&same(gpu[3],2));
    CHECK(gpu[7]==1&&gpu[11]==1);
    mov_output(w,3,0,15); nv2a_vsh_set_instruction(0,w);
    CHECK(nv2a_vsh_inputs_used(NULL)==8); CHECK(nv2a_vsh_program_hash()!=hash);
    const Nv2aVshInstruction *view=nv2a_vsh_program_data(&revision,&start); CHECK(!memcmp(view[0],w,16));
    nv2a_vsh_set_load_slot(0); for(unsigned k=0;k<4;k++) nv2a_vsh_program_word(w[k]);
    nv2a_vsh_program_data(&changed,NULL); CHECK(changed!=revision);
    nv2a_vsh_constants(&version); nv2a_vsh_constant_component(58,0,0x3f800000); nv2a_vsh_constants(&changed); CHECK(version!=changed);
    /* xyz-only positions preserve canonical zero w; unwritten colours opaque. */
    mov_output(w,0,0,14); nv2a_vsh_set_instruction(0,w); CHECK(nv2a_vsh_run(input,&cpu)); CHECK(cpu.pos[3]==0);
    CHECK(gpu_run(input,gpu)); CHECK(gpu[3]==0&&gpu[7]==1&&gpu[11]==1);
    /* Shared context writes change constants only when enabled; GPU rejects them. */
    mov_output(w,0,5,15); w[3]&=~(1u<<11); nv2a_vsh_set_instruction(0,w);
    nv2a_vsh_constants(&version); nv2a_vsh_set_cxt_write(0); CHECK(nv2a_vsh_run(input,&cpu)); nv2a_vsh_constants(&changed); CHECK(version==changed);
    nv2a_vsh_set_cxt_write(1); CHECK(nv2a_vsh_run(input,&cpu)); nv2a_vsh_constants(&changed); CHECK(version!=changed);
    CHECK(nv2a_vsh_constants(NULL)[5*4]==480); CHECK(nv2a_vsh_emit_hlsl(hlsl,sizeof hlsl,NULL)<0); nv2a_vsh_set_cxt_write(0);
    /* Invalid constants read c[0], including relative negative and high indices. */
    const float c0[4]={0.125f,0.25f,0.5f,1}, c191[4]={0.875f,0.75f,0.625f,1};
    nv2a_vsh_set_constant(0,c0);nv2a_vsh_set_constant(191,c191);
    for(unsigned relative=0;relative<2;relative++) for(unsigned high=0;high<2;high++) {
        unsigned slot=0;
        if(relative) {
            memset(w,0,16);w[1]=13u<<21;src(w,0,2,0,0); // ARL a0.x, v0.x
            nv2a_vsh_set_instruction(slot++,w);input[0][0]=high?200:-1;
        }
        memset(w,0,16);w[1]=(1u<<21)|((relative?0u:255u)<<13);src(w,0,3,0,0x1B);
        w[3]=(15u<<12)|(1u<<11)|(9u<<3)|1u|(relative?2u:0u);
        nv2a_vsh_set_instruction(slot,w);
        CHECK(nv2a_vsh_run(input,&cpu));CHECK(gpu_run(input,gpu));
        CHECK(same(cpu.tex[0][0],c0[0]));CHECK(same(gpu[12],c0[0]));
    }
    /* ILU results through texture x/y avoid colour saturation and clipping. */
    const float samples[]={2,-2,0,-0.0f,1e-30f,-1e-30f,1e30f,-1e30f,INFINITY,-INFINITY};
    for(unsigned ilu=2;ilu<=7;ilu++) for(unsigned v=0;v<sizeof samples/sizeof samples[0];v++) {
        memset(w,0,16);w[1]=ilu<<25;w[3]=15u<<16;src(w,2,2,0,0x1B);nv2a_vsh_set_instruction(0,w);
        mov_output(w,0,9,15);w[1]=1u<<21;w[2]=0;src(w,0,1,0,0x1B);nv2a_vsh_set_instruction(1,w);
        input[0][0]=samples[v];input[0][1]=0.5f;input[0][2]=0;input[0][3]=129;
        CHECK(nv2a_vsh_run(input,&cpu));CHECK(gpu_run(input,gpu));
        if (!same(gpu[12],cpu.tex[0][0]) || !same(gpu[13],cpu.tex[0][1]))
            fprintf(stderr,"ilu %u input %g GPU %g %g CPU %g %g\n",ilu,samples[v],gpu[12],gpu[13],cpu.tex[0][0],cpu.tex[0][1]);
        CHECK(same(gpu[12],cpu.tex[0][0]));CHECK(same(gpu[13],cpu.tex[0][1]));
    }
    ID3D11DeviceContext_ClearState(context); RELEASE(gs); RELEASE(context); RELEASE(device);
    if(!failures) puts("PASS canonical shader uploads, cache mutation, context writes and 60 WARP numeric cases");
    return failures?1:0;
}
