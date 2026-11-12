/* Two separable 4x4 reconstruction filters, operating on presentation color.
 * Catmull-Rom bicubic and Lanczos-2; neither changes the game render target. */
static const char s_spatial34_shader[] =
"float Weight34(float x){\n"
" x=abs(x);\n"
" #ifdef SPATIAL_LANCZOS\n"
" if(x<0.0001)return 1; if(x>=2)return 0; float p=3.14159265359*x; return (sin(p)/p)*(sin(p*0.5)/(p*0.5));\n"
" #else\n"
" if(x<1)return (1.5*x-2.5)*x*x+1; if(x<2)return ((-0.5*x+2.5)*x-4)*x+2; return 0;\n"
" #endif\n"
"}\n"
"float4 main(float4 p:SV_Position):SV_Target {\n"
" float2 uv=p.xy/dims.zw,xy=uv*dims.xy-0.5,base=floor(xy),f=xy-base; float3 sum=0; float weight=0;\n"
" [unroll] for(int y=-1;y<=2;y++){[unroll] for(int x=-1;x<=2;x++){\n"
"  float w=Weight34((float)x-f.x)*Weight34((float)y-f.y);\n"
"  int2 q=clamp((int2)base+int2(x,y),int2(0,0),(int2)dims.xy-1);\n"
"  sum+=pixels.Load(int3(q,0)).rgb*w;weight+=w; }}\n"
" return float4(Filter144(saturate(sum/max(weight,0.0001)),uv,p.xy),1);\n"
"}\n";
