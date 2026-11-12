/* Bounded replay catalogue for startup warm-up. This renderer has a single
 * Vulkan owner: warm synchronously before gameplay instead of racing its
 * compiler, module tables and VkPipelineCache on an unsafe worker. */
#define WARM_SHADERS144 128u
#define WARM_PIPES144 512u
#define WARM_BYTES144 (8u<<20)
typedef struct { char *text; uint32_t bytes,stage; } WarmShader144;
static struct {
    WarmShader144 shaders[WARM_SHADERS144];
    PipeKey pipes[WARM_PIPES144];
    uint32_t idmap[MAX_SHADERS+1];
    unsigned ns,np,bytes;int loading,dirty;
    char path[1200];
} warm144;
typedef struct {
    char magic[8]; uint32_t version,bytes,shaders,pipes,key_size,color,depth;
    VkcDevice device; uint64_t hash;
} WarmHeader144;
static void warm_note_shader144(const char *text,size_t bytes,int stage,uint32_t id)
{
    if(!id||id>MAX_SHADERS||warm144.loading||!s_preload144||!vkc_enabled())return;
    if(warm144.idmap[id])return;
    for(unsigned i=0;i<warm144.ns;i++)if(warm144.shaders[i].stage==(uint32_t)stage&&warm144.shaders[i].bytes==bytes&&
        !memcmp(warm144.shaders[i].text,text,bytes)){warm144.idmap[id]=i+1;return;}
    if(!bytes||bytes>(512u<<10)||warm144.ns==WARM_SHADERS144||bytes>WARM_BYTES144-warm144.bytes)return;
    char *copy=malloc(bytes+1);if(!copy)return;memcpy(copy,text,bytes);copy[bytes]=0;
    warm144.shaders[warm144.ns]=(WarmShader144){copy,(uint32_t)bytes,(uint32_t)stage};
    warm144.idmap[id]=++warm144.ns;warm144.bytes+=(unsigned)bytes;warm144.dirty=1;
}
static void warm_note_pipe144(const PipeKey *key)
{
    if(warm144.loading||!s_preload144||!vkc_enabled()||key->vs>MAX_SHADERS||key->ps>MAX_SHADERS)return;
    PipeKey k=*key;k.vs=warm144.idmap[k.vs];k.ps=warm144.idmap[k.ps];
    if(!k.vs||!k.ps)return;
    for(unsigned i=0;i<warm144.np;i++)if(!memcmp(&warm144.pipes[i],&k,sizeof k))return;
    if(warm144.np<WARM_PIPES144){warm144.pipes[warm144.np++]=k;warm144.dirty=1;}
}
static void warm_clear144(void)
{
    for(unsigned i=0;i<warm144.ns;i++)free(warm144.shaders[i].text);
    memset(warm144.shaders,0,sizeof warm144.shaders);memset(warm144.idmap,0,sizeof warm144.idmap);
    warm144.ns=warm144.np=warm144.bytes=0;warm144.dirty=0;
}
static void warm_save144(void)
{
    if(!warm144.dirty||!warm144.path[0]||!vkc_enabled())return;
    size_t size=warm144.bytes+warm144.ns*8u+warm144.np*sizeof(PipeKey);
    unsigned char *body=malloc(size?size:1),*p=body;if(!body)return;
    for(unsigned i=0;i<warm144.ns;i++){
        WarmShader144 *s=&warm144.shaders[i];memcpy(p,&s->stage,4);memcpy(p+4,&s->bytes,4);p+=8;
        memcpy(p,s->text,s->bytes);p+=s->bytes;
    }
    memcpy(p,warm144.pipes,warm144.np*sizeof(PipeKey));
    WarmHeader144 h={0};memcpy(h.magic,"DJWARM44",8);h.version=1;h.bytes=(uint32_t)size;
    h.shaders=warm144.ns;h.pipes=warm144.np;h.key_size=sizeof(PipeKey);h.color=s_fixed_color_rgba133;
    h.depth=vk.depth_fmt;h.device=vk.pcache_dev;h.hash=vkc_hash64(body,size,VKC_HASH_SEED);
    char tmp[1240];snprintf(tmp,sizeof tmp,"%s.tmp",warm144.path);FILE *f=fopen(tmp,"wb");
    if(f){int ok=fwrite(&h,1,sizeof h,f)==sizeof h&&fwrite(body,1,size,f)==size;
        if(fclose(f)!=0)ok=0;
        if(ok&&rename(tmp,warm144.path)==0){warm144.dirty=0;LOG("[WARM144] catalogue saved: %u shaders, %u pipelines",warm144.ns,warm144.np);}
        else remove(tmp);
    }free(body);
}
static void warm_load144(void)
{
    const char *env=getenv("RECOMP_SHADER_CACHE"),*dir=env?env:s_cache_dir;
    if(!s_preload144||!vkc_enabled()||!*dir||strlen(dir)>1024)return;
    snprintf(warm144.path,sizeof warm144.path,"%s/preload144.bin",dir);
    FILE *f=fopen(warm144.path,"rb");if(!f){LOG("[WARM144] first run: recording pipelines for next launch");return;}
    WarmHeader144 h;unsigned char *body=NULL,*p,*end;int valid=0;
    if(fread(&h,1,sizeof h,f)!=sizeof h||memcmp(h.magic,"DJWARM44",8)||h.version!=1||h.key_size!=sizeof(PipeKey)||
       h.color!=(unsigned)s_fixed_color_rgba133||h.depth!=(unsigned)vk.depth_fmt||memcmp(&h.device,&vk.pcache_dev,sizeof h.device)||
       h.shaders>WARM_SHADERS144||h.pipes>WARM_PIPES144||h.bytes>WARM_BYTES144+WARM_SHADERS144*8+WARM_PIPES144*sizeof(PipeKey))goto done;
    body=malloc(h.bytes?h.bytes:1);if(!body)goto done;
    if(fread(body,1,h.bytes,f)!=h.bytes||fgetc(f)!=EOF||vkc_hash64(body,h.bytes,VKC_HASH_SEED)!=h.hash)goto done;
    p=body;end=body+h.bytes;
    for(unsigned i=0;i<h.shaders;i++){
        uint32_t stage,n;if((size_t)(end-p)<8)goto done;memcpy(&stage,p,4);memcpy(&n,p+4,4);p+=8;
        if(stage>1||!n||n>(512u<<10)||n>(size_t)(end-p)||n>WARM_BYTES144-warm144.bytes||memchr(p,0,n))goto done;
        char *s=malloc(n+1);if(!s)goto done;memcpy(s,p,n);s[n]=0;p+=n;
        warm144.shaders[warm144.ns++]=(WarmShader144){s,n,stage};warm144.bytes+=n;
    }
    if((size_t)(end-p)!=h.pipes*sizeof(PipeKey))goto done;
    for(unsigned i=0;i<h.pipes;i++){
        PipeKey k;memcpy(&k,p,sizeof k);p+=sizeof k;
        if(!k.vs||k.vs>h.shaders||!k.ps||k.ps>h.shaders||warm144.shaders[k.vs-1].stage!=1||warm144.shaders[k.ps-1].stage!=0||
           k.layout>16||k.topology>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN||k.instanced>1||k.blend>1||k.ztest>1||k.zwrite>1||k.stencil>1)goto done;
        warm144.pipes[warm144.np++]=k;
    }
    valid=1;
done:
    fclose(f);free(body);
    if(!valid){warm_clear144();LOG("[WARM144] stale/corrupt catalogue ignored");return;}
    uint32_t ids[WARM_SHADERS144]={0};uint64_t start=usec_now();unsigned ns=0,np=0;
    warm144.loading=1;
    for(unsigned i=0;i<warm144.ns;i++){
        if(usec_now()-start>2000000)break; /* Bound launch work, never spread compilation across combat. */
        WarmShader144 *s=&warm144.shaders[i];
        ids[i]=shader_make(s->text,(int)s->stage,"warm144");
        if(ids[i]){warm144.idmap[ids[i]]=i+1;ns++;}
    }
    for(unsigned i=0;i<warm144.np;i++){
        if(usec_now()-start>2000000)break;
        PipeKey k=warm144.pipes[i];if(!ids[k.vs-1]||!ids[k.ps-1])continue;
        k.vs=ids[k.vs-1];k.ps=ids[k.ps-1];if(pipeline_get(&k))np++;
        else for(unsigned j=0;j<PIPE_SLOTS;j++)if(vk.pipes[j].used&&!vk.pipes[j].pipe&&!memcmp(&vk.pipes[j].key,&k,sizeof k)){
#if defined(__ANDROID__)
            if(vk.last_pipe==&vk.pipes[j])vk.last_pipe=NULL;
#endif
            vk.pipes[j].used=0;break;
        }
    }
    warm144.loading=0;warm144.dirty=0;
    LOG("[WARM144] startup %u/%u shaders, %u/%u pipelines in %.2f ms; unseen shaders still compile on demand",
        ns,warm144.ns,np,warm144.np,(usec_now()-start)/1000.0);
}
