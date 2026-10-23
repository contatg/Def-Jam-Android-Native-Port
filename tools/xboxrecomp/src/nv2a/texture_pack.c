/* Optional content-addressed PNG/DDS packs; no work on the original path when off.
 * v1 ID: XRTEX01 NUL + five LE32 descriptor fields + tight pixels + BGRA palette.
 * Source identities and replacement resources are deliberately separate caches.
 */
#if defined(_WIN32)
#define COBJMACROS
#endif
#include "texture_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <shlobj.h>
#else
#include <errno.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/uio.h>
#endif
#define _stricmp strcasecmp
#endif

#define MAX_ENTRIES 65536
#define SOURCE_SLOTS 2048
#define PATH_CAP 1024
typedef struct PackEntry {
    char id[65], path[PATH_CAP];
    unsigned order;
    IDirect3DTexture8 *texture;
    uint64_t bytes, used;
    unsigned levels;
    int channel_mips;
    int failed;
    ULONGLONG retry;
    struct LoadJob *job;     /* a background decode in flight or finished, not yet uploaded */
} PackEntry;
static PackEntry *g_entries;
static size_t g_count, g_capacity;
static struct SourceState {
    RecompTextureSource shape;
    char id[65];
    ULONGLONG checked;
    uint64_t quick;          /* texture_pack_quick() of the bytes that gave id */
    ULONGLONG interval;      /* ms between checks: REVALIDATE_MIN_MS while the bytes move, up to REVALIDATE_STABLE_MS */
    unsigned changes;        /* consecutive checks that found new bytes */
    int used, dumped, has_quick, unstable;
} g_sources[SOURCE_SLOTS];
/* Identity checks back off per source. A stable texture is re-read at most every 64 ms; one whose
 * bytes change on three checks in a row is treated as dynamic and drawn with its original
 * texture for up to 500 ms between checks, since a frame-specific texture would not match a pack anyway. */
#define REVALIDATE_MIN_MS 8
#define REVALIDATE_STABLE_MS 64
#define REVALIDATE_UNSTABLE_CHANGES 3
#define REVALIDATE_UNSTABLE_MS 500
static unsigned g_source_next;
#if defined(_WIN32)
static IWICImagingFactory *g_wic;
static BCRYPT_ALG_HANDLE g_sha;
static int g_com;
#endif
static int g_initialized, g_active, g_dump_limit=2000;
static char g_dump[PATH_CAP];
static uint64_t g_budget, g_resident, g_clock, g_hits, g_misses, g_hashed, g_dumps;
static unsigned g_errors,g_loads;
static int g_trace, g_sync_load;
/* Counted over two seconds and logged with the frame rate, so a play log shows what the packs cost. */
static struct { ULONGLONG start; unsigned loads, evictions, rejected, max_ms, revalidated, full_hashes; uint64_t ms, bytes, revalidated_bytes; } g_win;

/* ---- Host services. Everything below this block is the same code on every host. ---- */
static ULONGLONG now_ms(void);
static int file_exists(const char *path);
static void make_directories(const char *path);
static int memory_readable(const void *data, uint64_t bytes);
static int write_png(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h);
static uint8_t *read_png(const char *path, uint32_t *w, uint32_t *h, int *retry);
typedef struct Sha Sha;
#if defined(_WIN32)
struct Sha { BCRYPT_HASH_HANDLE hash; };
#elif defined(__APPLE__) && !defined(TEXTURE_PACK_PORTABLE_SHA)
#include <CommonCrypto/CommonDigest.h>
struct Sha { CC_SHA256_CTX ctx; };
#else
struct Sha { uint32_t state[8]; uint64_t bytes; uint8_t block[64]; size_t used; };
#endif
static int sha_start(Sha *sha);
static int sha_add(Sha *sha, const void *data, size_t bytes);
static int sha_finish(Sha *sha, uint8_t out[32]);
static void host_init(void);
static void host_shutdown(void);

static void warn(const char *what, const char *path)
{
    if (g_errors++ < 32) fprintf(stderr,"[TEXPACK] %s: %s\n",what,path ? path : "");
}
static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s)) ++s;
    end=s+strlen(s);
    while (end>s && isspace((unsigned char)end[-1])) *--end=0;
    return s;
}
static int valid_id(const char *s)
{
    unsigned i;
    if(strlen(s)!=64) return 0;
    for(i=0;i<64;i++) if(!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}
static int valid_name(const char *s, int relative)
{
    const unsigned char *p=(const unsigned char *)s;
    if(!*p || *p=='.' || *p=='/' || strstr(s,"..")) return 0;
    for(;*p;p++) if(!isalnum(*p) && *p!='_' && *p!='-' && *p!='.' && !(relative && *p=='/')) return 0;
    return 1;
}
static int entry_compare(const void *a,const void *b)
{
    const PackEntry *x=a,*y=b;
    int c=strcmp(x->id,y->id);
    return c ? c : (x->order>y->order)-(x->order<y->order);
}
static void read_pack(const char *root, const char *name)
{
    char path[PATH_CAP],line[2048],section[32]="";
    FILE *file;
    size_t start=g_count;
    int schema=0,channel_mips=0,mips_valid=1;
    if(!valid_name(name,0)) { warn("Invalid pack name",name); return; }
    if(snprintf(path,sizeof path,"%s/%s/manifest.ini",root,name)>=(int)sizeof path) return;
    file=fopen(path,"rb");
    if(!file) { warn("Pack manifest unavailable",path); return; }
    while(fgets(line,sizeof line,file)) {
        char *s=trim(line),*equal,*end;
        if(strlen(s)>1900) { warn("Manifest line too long",path); break; }
        if((unsigned char)s[0]==0xef && (unsigned char)s[1]==0xbb && (unsigned char)s[2]==0xbf) s+=3;
        if(!*s || *s==';' || *s=='#') continue;
        if(*s=='[') {
            end=strchr(s,']');
            if(!end || end-s>30) { section[0]=0; continue; }
            *end=0; snprintf(section,sizeof section,"%s",s+1); continue;
        }
        equal=strchr(s,'='); if(!equal) continue;
        *equal++=0; s=trim(s); equal=trim(equal);
        if(!strcmp(section,"pack") && !strcmp(s,"schema")) schema=!strcmp(equal,"1");
        if(!strcmp(section,"pack") && !strcmp(s,"png_mips")) {
            if(!strcmp(equal,"channels")) channel_mips=1;
            else if(!strcmp(equal,"opacity")) channel_mips=0;
            else mips_valid=0;
        }
        if(strcmp(section,"textures")) continue;
        if(!valid_id(s) || !valid_name(equal,1)) { warn("Invalid texture entry",s); continue; }
        end=strrchr(equal,'.');
        if(!end || (_stricmp(end,".png") && _stricmp(end,".dds"))) { warn("Unsupported extension",equal); continue; }
        if(g_count==MAX_ENTRIES) { warn("Pack entry limit",path); break; }
        if(g_count==g_capacity) {
            size_t capacity=g_capacity ? g_capacity*2 : 256;
            PackEntry *grown=realloc(g_entries,capacity*sizeof(*grown));
            if(!grown) break;
            g_entries=grown; g_capacity=capacity;
        }
        memset(&g_entries[g_count],0,sizeof g_entries[g_count]);
        for(end=s;*end;end++) *end=(char)tolower((unsigned char)*end);
        memcpy(g_entries[g_count].id,s,65);
        if(snprintf(g_entries[g_count].path,PATH_CAP,"%s/%s/%s",root,name,equal)>=PATH_CAP) continue;
        g_entries[g_count].order=(unsigned)g_count;
        g_count++;
    }
    fclose(file);
    if(!schema) { g_count=start; warn("Unsupported manifest schema",path); }
    else if(!mips_valid) { g_count=start; warn("Unsupported PNG mip policy",path); }
    else {
        size_t i;
        for(i=start;i<g_count;i++) g_entries[i].channel_mips=channel_mips;
    }
}
static void initialize(void)
{
    char root[PATH_CAP],packs[256],*next,*name;
    const char *e;
    long mb;
    if(g_initialized) return;
    g_initialized=1;
    mb=(e=getenv("RECOMP_TEXTURE_CACHE_MB")) ? strtol(e,NULL,10) : 512;
    if(mb<32) mb=32; if(mb>4096) mb=4096;
    g_budget=(uint64_t)mb*1024*1024;
    g_sync_load=(e=getenv("RECOMP_TEXTURE_SYNC")) && *e && *e!='0';
    g_trace=(e=getenv("RECOMP_TEXTURE_TRACE")) && *e && *e!='0';
    e=getenv("RECOMP_TEXTURE_DUMP_DIR"); if(e && *e) snprintf(g_dump,sizeof g_dump,"%s",e);
    if((e=getenv("RECOMP_TEXTURE_DUMP_LIMIT"))) {
        g_dump_limit=atoi(e); if(g_dump_limit<0) g_dump_limit=0; if(g_dump_limit>65536) g_dump_limit=65536;
    }
    e=getenv("RECOMP_TEXTURE_ROOT");
    if(e && *e) snprintf(root,sizeof root,"%s",e);
    else { e=getenv("DEFJAM_DATA"); snprintf(root,sizeof root,"%s/mods",e && *e ? e : "."); }
    e=getenv("RECOMP_TEXTURE_PACKS"); snprintf(packs,sizeof packs,"%s",e ? e : "");
    name=packs;
    while(*name) {
        next=strchr(name,';'); if(next) *next++=0;
        read_pack(root,trim(name));
        if(!next) break; name=next;
    }
    if(g_count) qsort(g_entries,g_count,sizeof(*g_entries),entry_compare);
    g_active=g_count || g_dump[0];
    if(g_active) {
        host_init();
        if(g_dump[0]) make_directories(g_dump);
        fprintf(stderr,"[TEXPACK] schema=1 entries=%zu cache=%ld MiB dump=%s\n",g_count,mb,g_dump);
    }
}
int texture_pack_active(void) { initialize(); return g_active; }

static int readable(const void *data,uint64_t bytes)
{
    if(!data || !bytes || bytes>64u*1024u*1024u) return 0;
    return memory_readable(data,bytes);
}
static int source_readable(const RecompTextureSource *s)
{
    return readable(s->data,s->bytes) && (!s->palette_entries || readable(s->palette,s->palette_entries*4u));
}

int texture_pack_hash(const RecompTextureSource *s,char out[65])
{
    Sha sha;
    uint8_t header[28]={'X','R','T','E','X','0','1',0},result[32];
    uint32_t fields[5]={s->format,s->width,s->height,s->palette_entries,s->row_texels};
    unsigned i,j;
    int ok;
    if(!s->data || !s->width || !s->height || s->width>4096 || s->height>4096 ||
       s->row_texels<s->width || s->row_texels>16384 || !s->bytes || s->bytes>64u*1024u*1024u || s->palette_entries>256 ||
       (s->palette_entries && !s->palette)) return 0;
    if(s->stride && (!s->rows || !s->row_bytes || s->row_bytes>s->stride ||
       (uint64_t)(s->rows-1)*s->stride+s->row_bytes>s->bytes)) return 0;
    /* A cached replacement bind does not read guest bytes. Validate on refresh. */
    if(!source_readable(s)) return 0;
    for(i=0;i<5;i++) for(j=0;j<4;j++) header[8+i*4+j]=(uint8_t)(fields[i]>>(8*j));
    if(!sha_start(&sha)) return 0;
    ok=sha_add(&sha,header,sizeof header);
    if(s->stride) {
        for(i=0;ok && i<s->rows;i++) ok=sha_add(&sha,(const uint8_t *)s->data+(size_t)i*s->stride,s->row_bytes);
        g_hashed+=(uint64_t)s->row_bytes*s->rows;
    } else { if(ok) ok=sha_add(&sha,s->data,s->bytes); g_hashed+=s->bytes; }
    if(ok && s->palette_entries) ok=sha_add(&sha,s->palette,s->palette_entries*4);
    if(!sha_finish(&sha,result) || !ok) return 0;
    g_win.full_hashes++;
    for(i=0;i<32;i++) sprintf(out+i*2,"%02x",result[i]);
    return 1;
}

/* A cheap check between identity hashes: four lanes of the xxHash64 round over the bytes
 * the identity covers. A bound texture is re-identified every 8 ms; while this value is
 * unchanged its SHA-256 is too (barring a 64-bit collision on a changed texture), so the
 * full hash runs only for new or rewritten textures. That hashing was the steady cost of
 * the packs on the GPU thread (a four-fighter match on a Steam Deck held 53 fps, not 60). */
static uint64_t quick_round(uint64_t acc,uint64_t word)
{
    acc+=word*0xC2B2AE3D27D4EB4Full;
    acc=(acc<<31)|(acc>>33);
    return acc*0x9E3779B185EBCA87ull;
}
static void quick_add(uint64_t lane[4],const uint8_t *p,size_t bytes)
{
    uint64_t word[4];
    for(;bytes>=32;bytes-=32,p+=32) {
        memcpy(word,p,32);
        lane[0]=quick_round(lane[0],word[0]); lane[1]=quick_round(lane[1],word[1]);
        lane[2]=quick_round(lane[2],word[2]); lane[3]=quick_round(lane[3],word[3]);
    }
    for(;bytes>=8;bytes-=8,p+=8) { memcpy(word,p,8); lane[0]=quick_round(lane[0],word[0]); }
    for(;bytes;bytes--,p++) lane[1]=quick_round(lane[1],*p);
}
static int texture_pack_quick(const RecompTextureSource *s,uint64_t *out)
{
    uint64_t lane[4]={0x60ea27eeadc0b5d6ull,0xc2b2ae3d27d4eb4full,0,0x61c8864e7a143579ull},h;
    unsigned i;
    if(!source_readable(s)) return 0;
    g_win.revalidated++; g_win.revalidated_bytes+=s->bytes;
    if(s->stride) for(i=0;i<s->rows;i++) quick_add(lane,(const uint8_t *)s->data+(size_t)i*s->stride,s->row_bytes);
    else quick_add(lane,s->data,s->bytes);
    if(s->palette_entries) quick_add(lane,s->palette,s->palette_entries*4u);
    h=((lane[0]<<1)|(lane[0]>>63))+((lane[1]<<7)|(lane[1]>>57))+((lane[2]<<12)|(lane[2]>>52))+((lane[3]<<18)|(lane[3]>>46));
    h^=h>>33; h*=0xC2B2AE3D27D4EB4Full; h^=h>>29; h*=0x165667B19E3779F9ull; h^=h>>32;
    *out=h;
    return 1;
}
static int same_layout(const RecompTextureSource *a,const RecompTextureSource *b)
{
    return a->data==b->data && a->bytes==b->bytes && a->stride==b->stride && a->row_bytes==b->row_bytes &&
           a->rows==b->rows && a->palette==b->palette && a->palette_entries==b->palette_entries &&
           a->format==b->format && a->width==b->width && a->height==b->height && a->row_texels==b->row_texels;
}

static void release_entry(IDirect3DDevice8 *dev,PackEntry *e)
{
    unsigned stage;
    if(!e->texture) return;
    for(stage=0;stage<4;stage++) {
        IDirect3DBaseTexture8 *current=NULL;
        if(SUCCEEDED(dev->lpVtbl->GetTexture(dev,stage,&current)) && current) {
            current->lpVtbl->Release(current);
            if(current==(IDirect3DBaseTexture8 *)e->texture) dev->lpVtbl->SetTexture(dev,stage,NULL);
        }
    }
    e->texture->lpVtbl->Release(e->texture); e->texture=NULL;
    g_resident-=e->bytes; e->bytes=0;
}
static int make_room(IDirect3DDevice8 *dev,uint64_t need)
{
    IDirect3DBaseTexture8 *bound[4]={0};
    unsigned stage;
    if(need>g_budget) return 0;
    for(stage=0;stage<4;stage++) {
        if(SUCCEEDED(dev->lpVtbl->GetTexture(dev,stage,&bound[stage])) && bound[stage])
            bound[stage]->lpVtbl->Release(bound[stage]);
    }
    while(g_resident+need>g_budget) {
        PackEntry *old=NULL; size_t i;
        for(i=0;i<g_count;i++) {
            int in_use=0;
            for(stage=0;stage<4;stage++) if(bound[stage]==(IDirect3DBaseTexture8 *)g_entries[i].texture) in_use=1;
            if(g_entries[i].texture && !in_use && (!old || g_entries[i].used<old->used)) old=&g_entries[i];
        }
        if(!old) return 0;
        release_entry(dev,old); g_win.evictions++;
    }
    return 1;
}
static int valid_scale(uint32_t w,uint32_t h,const RecompTextureSource *s)
{
    uint32_t scale=w/s->width;
    return scale>=1 && scale<=8 && w==s->width*scale && h==s->height*scale;
}
/* A decoded replacement with its whole mip chain, ready to copy into a texture. It
 * touches no D3D object, so a worker thread can build it. */
typedef struct Prepared {
    uint32_t w,h,levels;
    uint64_t memory;
    uint8_t *level[16];
    uint32_t lw[16],lh[16];
} Prepared;
static void prepared_free(Prepared *p)
{
    unsigned i;
    if(!p) return;
    for(i=0;i<16;i++) free(p->level[i]);
    free(p);
}
/* Decode, pad to the guest's row length and build the mips. Returns NULL for a replacement that
 * is rejected (*retry 0) or that failed for a reason that can clear (*retry 1). */
static Prepared *prepare_png(const char *path,uint32_t src_w,uint32_t src_h,uint32_t row_texels,int channel_mips,int *retry)
{
    uint32_t w=0,h=0,physical_w,levels=1,dim,y,x,level;
    uint8_t *data,*padded=NULL;
    Prepared *out;
    RecompTextureSource shape={0};
    shape.width=src_w; shape.height=src_h;
    *retry=0;
    data=read_png(path,&w,&h,retry);
    if(!data) return NULL;
    if(!valid_scale(w,h,&shape)) { free(data); return NULL; }
    physical_w=row_texels*(w/src_w);
    if(physical_w<w || physical_w>16384 || (uint64_t)physical_w*h*4>g_budget/4) { free(data); return NULL; }
    if(physical_w!=w) {
        padded=malloc((size_t)physical_w*h*4);
        if(!padded) { free(data); *retry=1; return NULL; }
        for(y=0;y<h;y++) {
            memcpy(padded+(size_t)y*physical_w*4,data+(size_t)y*w*4,w*4);
            for(x=w;x<physical_w;x++) memcpy(padded+((size_t)y*physical_w+x)*4,data+((size_t)y*w+w-1)*4,4);
        }
        free(data); data=padded; w=physical_w;
    }
    for(dim=w>h?w:h;dim>1;dim>>=1) levels++;
    out=calloc(1,sizeof *out);
    if(!out) { free(data); *retry=1; return NULL; }
    out->w=w; out->h=h; out->levels=levels;
    for(level=0;level<levels;level++) out->memory+=(uint64_t)(w>>level?w>>level:1)*(h>>level?h>>level:1)*8;
    if(out->memory>g_budget) { free(data); free(out); return NULL; }
    for(level=0;level<levels;level++) {
        uint32_t nw=w>1?w/2:1,nh=h>1?h/2:1;
        out->level[level]=data; out->lw[level]=w; out->lh[level]=h;
        if(level+1<levels) {
            uint8_t *next=malloc((size_t)nw*nh*4);
            if(!next) { prepared_free(out); *retry=1; return NULL; }
            for(y=0;y<nh;y++) {
                uint32_t y0=y*h/nh,y1=(y+1)*h/nh;
                for(x=0;x<nw;x++) {
                    unsigned c,alpha=0,colour[3]={0},samples=0;
                    uint32_t sx,sy,x0=x*w/nw,x1=(x+1)*w/nw;
                    /* Include odd final rows/columns instead of discarding their
                     * artwork. Power-of-two textures retain the original 2x2 box. */
                    for(sy=y0;sy<y1;sy++) for(sx=x0;sx<x1;sx++) {
                        const uint8_t *q=data+((size_t)sy*w+sx)*4;
                        samples++;
                        alpha+=q[3]; for(c=0;c<3;c++) colour[c]+=q[c]*(channel_mips ? 1 : q[3]);
                    }
                    for(c=0;c<3;c++) next[((size_t)y*nw+x)*4+c]=(uint8_t)(channel_mips ? (colour[c]+samples/2)/samples : alpha ? (colour[c]+alpha/2)/alpha : 0);
                    next[((size_t)y*nw+x)*4+3]=(uint8_t)((alpha+samples/2)/samples);
                }
            }
            data=next; w=nw; h=nh;
        }
    }
    return out;
}
/* Create the texture and copy the chain in. D3D calls: the render thread only. 1 done, -1 retry later. */
static int commit_prepared(IDirect3DDevice8 *dev,PackEntry *e,const Prepared *p)
{
    IDirect3DTexture8 *texture=NULL; uint32_t level,y;
    ULONGLONG t0=now_ms(),t1,t2,t3;
    if(!make_room(dev,p->memory) || FAILED(dev->lpVtbl->CreateTexture(dev,p->w,p->h,p->levels,0,D3DFMT_LIN_A8R8G8B8,0,&texture))) return -1;
    t1=now_ms(); t2=t3=0;
    for(level=0;level<p->levels;level++) {
        D3DLOCKED_RECT lock={0}; uint32_t w=p->lw[level],h=p->lh[level];
        ULONGLONG a=now_ms(),b;
        if(FAILED(texture->lpVtbl->LockRect(texture,level,&lock,NULL,0)) || !lock.pBits || lock.Pitch<(INT)(w*4)) goto fail;
        for(y=0;y<h;y++) memcpy((uint8_t *)lock.pBits+(size_t)y*lock.Pitch,p->level[level]+(size_t)y*w*4,w*4);
        b=now_ms(); t2+=b-a;
        if(FAILED(texture->lpVtbl->UnlockRect(texture,level))) goto fail;
        t3+=now_ms()-b;
    }
    if(g_trace && now_ms()-t0>=8) fprintf(stderr,"[TEXPACK] commit %ux%u: room+create %llu, copy %llu, unlock %llu ms\n",p->w,p->h,(unsigned long long)(t1-t0),(unsigned long long)t2,(unsigned long long)t3);
    e->texture=texture; e->bytes=p->memory; e->levels=p->levels; g_resident+=p->memory; return 1;
fail:
    texture->lpVtbl->Release(texture); return -1;
}
static int upload_png(IDirect3DDevice8 *dev,PackEntry *e,const RecompTextureSource *source)
{
    int retry=0,result;
    Prepared *p=prepare_png(e->path,source->width,source->height,source->row_texels,e->channel_mips,&retry);
    if(!p) return retry ? -1 : 0;
    result=commit_prepared(dev,e,p);
    prepared_free(p);
    return result;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int upload_dds(IDirect3DDevice8 *dev,PackEntry *e,const RecompTextureSource *source)
{
    FILE *file=fopen(e->path,"rb"); uint8_t header[128],*data=NULL;
    uint32_t w,h,levels,fourcc,block,level,y;
    D3DFORMAT fmt; uint64_t total=0;
    IDirect3DTexture8 *texture=NULL; int retry=0;
    if(!file) return 0;
    if(fread(header,1,128,file)!=128 || memcmp(header,"DDS ",4) || le32(header+4)!=124 || le32(header+76)!=32) goto fail;
    h=le32(header+12); w=le32(header+16); levels=le32(header+28); fourcc=le32(header+84);
    if(!levels) levels=1;
    if(!w || !h || w>16384 || h>16384 || levels>15 || !valid_scale(w,h,source) || source->row_texels!=source->width || le32(header+112) || !(le32(header+80)&4) || le32(header+24)>1) goto fail;
    if(fourcc==0x31545844) { fmt=D3DFMT_DXT1; block=8; }
    else if(fourcc==0x33545844) { fmt=D3DFMT_DXT3; block=16; }
    else if(fourcc==0x35545844) { fmt=D3DFMT_DXT5; block=16; }
    else goto fail;
    for(level=0;level<levels;level++) {
        uint32_t lw=w>>level?w>>level:1,lh=h>>level?h>>level:1;
        if(level && (w>>(level-1))<=1 && (h>>(level-1))<=1) goto fail;
        total+=(uint64_t)((lw+3)/4)*((lh+3)/4)*block;
    }
    if(total>g_budget/4) goto fail;
    if(!make_room(dev,total*2)) { fclose(file); return -1; }
    data=malloc((size_t)total); if(!data) { retry=1; goto fail; }
    if(fread(data,1,(size_t)total,file)!=total || fgetc(file)!=EOF) goto fail;
    retry=1; /* Device/allocation failures can clear; malformed files cannot. */
    if(FAILED(dev->lpVtbl->CreateTexture(dev,w,h,levels,0,fmt,0,&texture))) goto fail;
    { uint64_t offset=0;
      for(level=0;level<levels;level++) {
        D3DLOCKED_RECT lock={0}; uint32_t lw=w>>level?w>>level:1,lh=h>>level?h>>level:1;
        uint32_t row=((lw+3)/4)*block,rows=(lh+3)/4;
        if(FAILED(texture->lpVtbl->LockRect(texture,level,&lock,NULL,0)) || !lock.pBits || lock.Pitch<(INT)row) goto fail;
        for(y=0;y<rows;y++) memcpy((uint8_t *)lock.pBits+(size_t)y*lock.Pitch,data+offset+(size_t)y*row,row);
        if(FAILED(texture->lpVtbl->UnlockRect(texture,level))) goto fail;
        offset+=(uint64_t)row*rows;
      }
    }
    fclose(file); free(data); e->texture=texture; e->bytes=total*2; e->levels=levels; g_resident+=e->bytes; return 1;
fail:
    fclose(file); free(data); if(texture) texture->lpVtbl->Release(texture); return retry ? -1 : 0;
}
static PackEntry *lookup(const char *id)
{
    size_t a=0,b=g_count;
    while(a<b) { size_t mid=a+(b-a)/2; if(strcmp(g_entries[mid].id,id)<=0) a=mid+1; else b=mid; }
    return a && !strcmp(g_entries[a-1].id,id) ? &g_entries[a-1] : NULL;
}
static unsigned source_slot(const RecompTextureSource *s)
{
    uintptr_t palette=(uintptr_t)s->palette;
    uint32_t key=s->address ^ s->format*0x9e3779b9u ^ (uint32_t)palette;
    /* Guest allocation alignment must not discard all useful address bits. */
#if UINTPTR_MAX > 0xffffffffu
    key^=(uint32_t)(palette>>32);
#endif
    key^=key>>16; key*=0x7feb352du;
    key^=key>>15; key*=0x846ca68bu; key^=key>>16;
    return key & (SOURCE_SLOTS-1);
}
/* ---- Background decoding. ------------------------------------------------------------
 * Reading a replacement is a PNG decode and a mip chain: 100 to 350 ms for the big menu art,
 * and done on the render thread at the first bind it stalled every screen change. Workers do
 * that part; the render thread only creates the texture and copies the chain in. Until a
 * replacement is ready the original texture is drawn. */
#if defined(_WIN32)
typedef CRITICAL_SECTION PoolLock;
typedef CONDITION_VARIABLE PoolCond;
typedef HANDLE PoolThread;
#define POOL_THREAD_RESULT DWORD WINAPI
#else
#include <pthread.h>
typedef pthread_mutex_t PoolLock;
typedef pthread_cond_t PoolCond;
typedef pthread_t PoolThread;
#define POOL_THREAD_RESULT void *
#endif
#define LOAD_JOBS 16
#define LOAD_WORKERS 3
#define LOAD_STALE_MS 4000
enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };
typedef struct LoadJob {
    int state;
    PackEntry *entry;
    char path[PATH_CAP];
    uint32_t w,h,row_texels;
    int channel_mips,retry;
    Prepared *image;
    ULONGLONG done_at;
} LoadJob;
static LoadJob g_jobs[LOAD_JOBS];
#if defined(_WIN32)
static PoolLock g_job_lock;
static PoolCond g_job_wake;
#else
static PoolLock g_job_lock=PTHREAD_MUTEX_INITIALIZER;
static PoolCond g_job_wake=PTHREAD_COND_INITIALIZER;
#endif
static PoolThread g_workers[LOAD_WORKERS];
static int g_worker_count, g_workers_stop;

static void pool_lock(void)
{
#if defined(_WIN32)
    EnterCriticalSection(&g_job_lock);
#else
    pthread_mutex_lock(&g_job_lock);
#endif
}
static void pool_unlock(void)
{
#if defined(_WIN32)
    LeaveCriticalSection(&g_job_lock);
#else
    pthread_mutex_unlock(&g_job_lock);
#endif
}
static void pool_wait(void)             /* with the lock held; returns with it held */
{
#if defined(_WIN32)
    SleepConditionVariableCS(&g_job_wake,&g_job_lock,INFINITE);
#else
    pthread_cond_wait(&g_job_wake,&g_job_lock);
#endif
}
static void pool_wake(int all)
{
#if defined(_WIN32)
    if(all) WakeAllConditionVariable(&g_job_wake); else WakeConditionVariable(&g_job_wake);
#else
    if(all) pthread_cond_broadcast(&g_job_wake); else pthread_cond_signal(&g_job_wake);
#endif
}

static POOL_THREAD_RESULT load_worker(
#if defined(_WIN32)
    LPVOID unused
#else
    void *unused
#endif
    )
{
    (void)unused;
#if defined(_WIN32)
    /* WIC wants COM on every thread that calls it; the factory itself is free-threaded. */
    HRESULT com=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);   /* the render and audio threads come first */
#elif defined(__linux__) && defined(SYS_gettid)
    setpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid),10);
#endif
    pool_lock();
    for(;;) {
        LoadJob *job=NULL; int i; Prepared *image; int retry=0;
        for(i=0;i<LOAD_JOBS;i++) if(g_jobs[i].state==JOB_QUEUED) { job=&g_jobs[i]; break; }
        if(!job) {
            if(g_workers_stop) break;
            pool_wait();
            continue;
        }
        job->state=JOB_RUNNING;
        pool_unlock();
        image=prepare_png(job->path,job->w,job->h,job->row_texels,job->channel_mips,&retry);
        pool_lock();
        job->image=image; job->retry=retry; job->done_at=now_ms(); job->state=JOB_DONE;
    }
    pool_unlock();
#if defined(_WIN32)
    if(SUCCEEDED(com)) CoUninitialize();
    return 0;
#else
    return NULL;
#endif
}
static int workers_start(void)
{
    int want;
#if defined(_WIN32)
    SYSTEM_INFO info; GetSystemInfo(&info);
    want=info.dwNumberOfProcessors>=6 ? LOAD_WORKERS : info.dwNumberOfProcessors>=3 ? 2 : 1;
#else
    long cpus=sysconf(_SC_NPROCESSORS_ONLN);
    want=cpus>=6 ? LOAD_WORKERS : cpus>=3 ? 2 : 1;
#endif
    if(g_worker_count) return 1;
    g_workers_stop=0;
#if defined(_WIN32)
    { static int ready; if(!ready) { InitializeCriticalSection(&g_job_lock); InitializeConditionVariable(&g_job_wake); ready=1; } }
    while(g_worker_count<want && (g_workers[g_worker_count]=CreateThread(NULL,0,load_worker,NULL,0,NULL))) g_worker_count++;
#else
    while(g_worker_count<want && !pthread_create(&g_workers[g_worker_count],NULL,load_worker,NULL)) g_worker_count++;
#endif
    return g_worker_count>0;
}
static void workers_stop(void)
{
    int i;
    if(!g_worker_count) return;
    pool_lock();
    g_workers_stop=1;
    pool_wake(1);
    pool_unlock();
    for(i=0;i<g_worker_count;i++) {
#if defined(_WIN32)
        WaitForSingleObject(g_workers[i],INFINITE); CloseHandle(g_workers[i]);
#else
        pthread_join(g_workers[i],NULL);
#endif
    }
    g_worker_count=0;
    for(i=0;i<LOAD_JOBS;i++) { prepared_free(g_jobs[i].image); memset(&g_jobs[i],0,sizeof g_jobs[i]); }
    for(i=0;(size_t)i<g_count;i++) g_entries[i].job=NULL;
}
/* Called with the clock at each bind: drop finished decodes nobody came back for. */
static void jobs_reap(ULONGLONG now)
{
    int i;
    if(!g_worker_count) return;
    pool_lock();
    for(i=0;i<LOAD_JOBS;i++) {
        LoadJob *job=&g_jobs[i];
        if(job->state==JOB_DONE && now-job->done_at>LOAD_STALE_MS) {
            job->entry->job=NULL; prepared_free(job->image); memset(job,0,sizeof *job);
        }
    }
    pool_unlock();
}
/* 1 uploaded, 0 rejected for good, -1 try again later, -2 still being decoded. */
static int upload_png_async(IDirect3DDevice8 *dev,PackEntry *e,const RecompTextureSource *source,ULONGLONG now)
{
    LoadJob *job=e->job; int i, result;
    if(!job) {
        if(!workers_start()) return upload_png(dev,e,source);
        pool_lock();
        for(i=0;i<LOAD_JOBS && g_jobs[i].state!=JOB_FREE;i++) ;
        if(i==LOAD_JOBS) { pool_unlock(); return -2; }
        job=&g_jobs[i]; memset(job,0,sizeof *job);
        job->entry=e; snprintf(job->path,sizeof job->path,"%s",e->path);
        job->w=source->width; job->h=source->height; job->row_texels=source->row_texels;
        job->channel_mips=e->channel_mips; job->state=JOB_QUEUED;
        e->job=job;
        pool_wake(0);
        pool_unlock();
        return -2;
    }
    pool_lock();
    if(job->state!=JOB_DONE) { pool_unlock(); return -2; }
    pool_unlock();
    /* A worker never touches a finished job again, so it is ours until we free it. */
    e->job=NULL;
    if(!job->image) result=job->retry ? -1 : 0;
    else { result=commit_prepared(dev,e,job->image); prepared_free(job->image); }
    pool_lock();
    memset(job,0,sizeof *job);
    pool_unlock();
    (void)now;
    return result;
}

int texture_pack_bind(IDirect3DDevice8 *dev,unsigned stage,const RecompTextureSource *s,RecompTextureSample sample,void *user)
{
    struct SourceState *state=NULL; char id[65]; PackEntry *entry;
    uint64_t quick=0; int have_quick=0;
    unsigned i, slot=source_slot(s);
    ULONGLONG now=now_ms();
    if(!texture_pack_active() || !dev || stage>3) return 0;
    if(!g_win.start) g_win.start=now;
    if(now-g_win.start>=2000) {
        if(g_win.loads || g_win.evictions)
            fprintf(stderr,"  [TEXPACK] %.1fs: %u replacements uploaded (%llu MiB, %llu ms on the render thread, slowest %u ms), %u evicted, %llu of %llu MiB resident\n",
                    (double)(now-g_win.start)/1000.0,g_win.loads,(unsigned long long)(g_win.bytes>>20),(unsigned long long)g_win.ms,g_win.max_ms,
                    g_win.evictions,(unsigned long long)(g_resident>>20),(unsigned long long)(g_budget>>20));
        if(g_trace)
            fprintf(stderr,"  [TEXPACK] %.1fs: %u source revalidations (%llu MiB read), %u full hashes\n",
                    (double)(now-g_win.start)/1000.0,g_win.revalidated,(unsigned long long)(g_win.revalidated_bytes>>20),g_win.full_hashes);
        memset(&g_win,0,sizeof g_win); g_win.start=now;
    }
    for(i=0;i<4;i++) {
        struct SourceState *c=&g_sources[(slot+i)&(SOURCE_SLOTS-1)];
        if(c->used && c->shape.address==s->address && c->shape.format==s->format && c->shape.width==s->width &&
           c->shape.height==s->height && c->shape.row_texels==s->row_texels && c->shape.palette==s->palette && c->shape.palette_entries==s->palette_entries) { state=c; break; }
    }
    if(state && now-state->checked<state->interval) {
        if(state->unstable) return 0;          /* dynamic texture: serve the original until its bytes settle */
        memcpy(id,state->id,65);
    } else if(state && state->has_quick && same_layout(&state->shape,s) &&
              (have_quick=texture_pack_quick(s,&quick)) && quick==state->quick) {
        memcpy(id,state->id,65); state->checked=now; state->changes=0; state->unstable=0;
        state->interval = state->interval<REVALIDATE_STABLE_MS ? state->interval*2 : REVALIDATE_STABLE_MS;
    } else {
        { ULONGLONG h0=now_ms(); int ok=texture_pack_hash(s,id), ms=(int)(now_ms()-h0);
          if(ms>=1 && g_trace) fprintf(stderr,"[TEXPACK] hash %ux%u fmt %x: %d ms\n",s->width,s->height,s->format,ms);
          if(!ok) return 0; }
        if(!state) {
            state=&g_sources[slot];
            for(i=0;i<4;i++) {
                struct SourceState *candidate=&g_sources[(slot+i)&(SOURCE_SLOTS-1)];
                if(!candidate->used) { state=candidate; break; }
                if(candidate->checked<state->checked) state=candidate;
            }
            state->dumped=0; state->changes=0; state->unstable=0;
        } else state->changes++;
        if(state->used && strcmp(state->id,id)) state->dumped=0;
        state->shape=*s; state->checked=now; memcpy(state->id,id,65); state->used=1;
        state->has_quick=have_quick || texture_pack_quick(s,&quick);
        state->quick=quick;
        if(state->changes>=REVALIDATE_UNSTABLE_CHANGES) { state->unstable=1; state->interval=REVALIDATE_UNSTABLE_MS; }
        else state->interval=REVALIDATE_MIN_MS;
    }
    if(!state->dumped && g_dump[0] && g_dumps<(uint64_t)g_dump_limit && sample && source_readable(s)) {
        state->dumped=1;
        char path[PATH_CAP]; snprintf(path,sizeof path,"%s/%s.png",g_dump,id);
        if(!file_exists(path)) {
            uint32_t *pixels=malloc((size_t)s->width*s->height*4),x,y; int ok=1;
            if(pixels) {
                for(y=0;y<s->height && ok;y++) for(x=0;x<s->width;x++) if(!sample(user,x,y,&pixels[(size_t)y*s->width+x])) { ok=0; break; }
                if(ok && write_png(path,(const uint8_t *)pixels,s->width,s->height)) {
                    char meta[PATH_CAP]; FILE *log;
                    char decoded_id[65]="";
                    RecompTextureSource decoded={0};
                    decoded.format=0x12; decoded.width=decoded.row_texels=s->width; decoded.height=s->height;
                    decoded.data=pixels; decoded.bytes=s->width*s->height*4;
                    texture_pack_hash(&decoded,decoded_id);
                    snprintf(meta,sizeof meta,"%s/textures.jsonl",g_dump); log=fopen(meta,"ab");
                    if(log) { fprintf(log,"{\"id\":\"%s\",\"decoded_id\":\"%s\",\"format\":%u,\"width\":%u,\"height\":%u,\"row_texels\":%u,\"palette_entries\":%u,\"stage\":%u}\n",id,decoded_id,s->format,s->width,s->height,s->row_texels,s->palette_entries,stage); fclose(log); }
                    ++g_dumps;
                } else warn("RGBA dump failed",path);
                free(pixels);
            }
        }
    }
    entry=lookup(id); if(!entry) { ++g_misses; return 0; }
    if(entry->failed || now<entry->retry) return 0;
    if(!entry->texture) {
        const char *extension=strrchr(entry->path,'.');
        int is_dds=extension && !_stricmp(extension,".dds");
        ULONGLONG t0=now_ms();
        int loaded;
        jobs_reap(now);
        loaded=is_dds ? upload_dds(dev,entry,s) : g_sync_load ? upload_png(dev,entry,s) : upload_png_async(dev,entry,s,now);
        if(loaded==-2) return 0;
        { unsigned took=(unsigned)(now_ms()-t0);
          if(loaded>0) { g_win.loads++; g_win.ms+=took; g_win.bytes+=entry->bytes; if(took>g_win.max_ms) g_win.max_ms=took; }
          if(g_trace) fprintf(stderr,"[TEXPACK] upload %s %ux%u: %u ms (%d)\n",entry->id,s->width,s->height,took,loaded); }
        if(loaded<0) { entry->retry=now+1000; return 0; }
        if(!loaded) { entry->failed=1; warn("Replacement rejected; using original",entry->path); return 0; }
        if(g_loads++<32) fprintf(stderr,"[TEXPACK] loaded %s (%u mips, %llu bytes)\n",entry->id,entry->levels,(unsigned long long)entry->bytes);
    }
    entry->used=++g_clock; ++g_hits;
    if(FAILED(dev->lpVtbl->SetTexture(dev,stage,(IDirect3DBaseTexture8 *)entry->texture))) return 0;
    /* SetTexture leaves the title's combiners, addressing and filtering intact. */
    return 1;
}
void texture_pack_shutdown(IDirect3DDevice8 *dev)
{
    size_t i;
    if(g_active) fprintf(stderr,"[TEXPACK] hits=%llu misses=%llu dumps=%llu hashed=%llu resident=%llu\n",
        (unsigned long long)g_hits,(unsigned long long)g_misses,(unsigned long long)g_dumps,(unsigned long long)g_hashed,(unsigned long long)g_resident);
    workers_stop();
    if(dev) for(i=0;i<g_count;i++) release_entry(dev,&g_entries[i]);
    free(g_entries); g_entries=NULL; g_count=g_capacity=0;
    memset(g_sources,0,sizeof g_sources);
    host_shutdown();
    g_initialized=g_active=0; g_dump[0]=0;
    g_resident=g_hits=g_misses=g_dumps=g_hashed=g_clock=0; g_source_next=0; g_loads=0;
}

/* ---- Host services: Windows (WIC, BCrypt, VirtualQuery) ---- */
#if defined(_WIN32)
/* Monotonic and sub-millisecond. GetTickCount64 advances in 15.6 ms steps, which made the
 * 8 ms re-identify window in texture_pack_bind expire on almost every bind and hid the true
 * cost of each hash in the trace (every timing read 15 or 16 ms). The POSIX clock has no such step. */
static ULONGLONG now_ms(void)
{
    LARGE_INTEGER frequency, ticks;
    ULONGLONG count, hz;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&ticks);
    count=(ULONGLONG)ticks.QuadPart; hz=(ULONGLONG)frequency.QuadPart;
    return (count/hz)*1000u + (count%hz)*1000u/hz;
}
static int file_exists(const char *path) { return GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES; }
static void make_directories(const char *path) { SHCreateDirectoryExA(NULL,path,NULL); }
static int memory_readable(const void *data,uint64_t bytes)
{
    MEMORY_BASIC_INFORMATION region;
    uintptr_t begin=(uintptr_t)data,end=begin+(uintptr_t)bytes,at=begin,next;
    if(end<begin) return 0;
    while(at<end) {
        if(!VirtualQuery((const void *)at,&region,sizeof region) || region.State!=MEM_COMMIT ||
           (region.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return 0;
        next=(uintptr_t)region.BaseAddress+region.RegionSize;
        if(next<=at) return 0;
        at=next;
    }
    return 1;
}
static int wide_path(const char *path,WCHAR out[PATH_CAP])
{ return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,out,PATH_CAP)>0; }
static int write_png(const char *path,const uint8_t *bgra,uint32_t w,uint32_t h)
{
    IWICStream *stream=NULL; IWICBitmapEncoder *encoder=NULL; IWICBitmapFrameEncode *frame=NULL;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    WCHAR wide[PATH_CAP]; HRESULT hr=E_FAIL;
    if(!g_wic || !wide_path(path,wide)) return 0;
    hr=IWICImagingFactory_CreateStream(g_wic,&stream);
    if(SUCCEEDED(hr)) hr=IWICStream_InitializeFromFilename(stream,wide,GENERIC_WRITE);
    if(SUCCEEDED(hr)) hr=IWICImagingFactory_CreateEncoder(g_wic,&GUID_ContainerFormatPng,NULL,&encoder);
    if(SUCCEEDED(hr)) hr=IWICBitmapEncoder_Initialize(encoder,(IStream *)stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr)) hr=IWICBitmapEncoder_CreateNewFrame(encoder,&frame,NULL);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Initialize(frame,NULL);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetSize(frame,w,h);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetPixelFormat(frame,&format);
    if(SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat32bppBGRA)) hr=E_FAIL;
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_WritePixels(frame,h,w*4,w*h*4,(BYTE *)bgra);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Commit(frame);
    if(SUCCEEDED(hr)) hr=IWICBitmapEncoder_Commit(encoder);
    if(frame) IWICBitmapFrameEncode_Release(frame);
    if(encoder) IWICBitmapEncoder_Release(encoder);
    if(stream) IWICStream_Release(stream);
    return SUCCEEDED(hr);
}
static uint8_t *read_png(const char *path,uint32_t *w,uint32_t *h,int *retry)
{
    IWICBitmapDecoder *decoder=NULL; IWICBitmapFrameDecode *frame=NULL; IWICFormatConverter *converter=NULL;
    WCHAR wide[PATH_CAP]; uint8_t *data=NULL; HRESULT hr=E_FAIL;
    if(!g_wic || !wide_path(path,wide)) return NULL;
    hr=IWICImagingFactory_CreateDecoderFromFilename(g_wic,wide,NULL,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder);
    if(SUCCEEDED(hr)) hr=IWICBitmapDecoder_GetFrame(decoder,0,&frame);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameDecode_GetSize(frame,w,h);
    if(SUCCEEDED(hr) && (!*w || !*h || *w>16384 || *h>16384 || (uint64_t)*w**h*4>g_budget/4)) hr=E_INVALIDARG;
    if(SUCCEEDED(hr)) hr=IWICImagingFactory_CreateFormatConverter(g_wic,&converter);
    if(SUCCEEDED(hr)) hr=IWICFormatConverter_Initialize(converter,(IWICBitmapSource *)frame,&GUID_WICPixelFormat32bppBGRA,
                                  WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    if(SUCCEEDED(hr)) { data=malloc((size_t)*w**h*4); if(!data) hr=E_OUTOFMEMORY; }
    if(SUCCEEDED(hr)) hr=IWICFormatConverter_CopyPixels(converter,NULL,*w*4,*w**h*4,data);
    if(converter) IWICFormatConverter_Release(converter);
    if(frame) IWICBitmapFrameDecode_Release(frame);
    if(decoder) IWICBitmapDecoder_Release(decoder);
    if(FAILED(hr)) { free(data); data=NULL; *retry=(hr==E_OUTOFMEMORY); }
    return data;
}
static int sha_start(Sha *sha)
{
    if(!g_sha && BCryptOpenAlgorithmProvider(&g_sha,BCRYPT_SHA256_ALGORITHM,NULL,0)<0) return 0;
    return BCryptCreateHash(g_sha,&sha->hash,NULL,0,NULL,0,0)>=0;
}
static int sha_add(Sha *sha,const void *data,size_t bytes) { return BCryptHashData(sha->hash,(PUCHAR)data,(ULONG)bytes,0)>=0; }
static int sha_finish(Sha *sha,uint8_t out[32])
{
    int ok=BCryptFinishHash(sha->hash,out,32,0)>=0;
    BCryptDestroyHash(sha->hash);
    return ok;
}
static void host_init(void)
{
    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    g_com=SUCCEEDED(hr);
    if(FAILED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,
                              &IID_IWICImagingFactory,(void **)&g_wic))) warn("WIC unavailable","");
}
static void host_shutdown(void)
{
    if(g_wic) IWICImagingFactory_Release(g_wic); g_wic=NULL;
    if(g_sha) BCryptCloseAlgorithmProvider(g_sha,0); g_sha=NULL;
    if(g_com) CoUninitialize();
    g_com=0;
}

/* ---- Host services: macOS and Linux (stb PNG, a portable SHA-256, memory protection checks) ---- */
#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

static ULONGLONG now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (ULONGLONG)t.tv_sec*1000u+(ULONGLONG)t.tv_nsec/1000000u;
}
static int file_exists(const char *path) { return access(path,F_OK)==0; }
static void make_directories(const char *path)
{
    char copy[PATH_CAP],*p;
    if(snprintf(copy,sizeof copy,"%s",path)>=(int)sizeof copy) return;
    for(p=copy+1;*p;p++) if(*p=='/') { *p=0; mkdir(copy,0777); *p='/'; }
    mkdir(copy,0777);
}
/* Readable now and not a protected page. The runtime traps hardware registers with
 * inaccessible pages, and reading one would raise the trap rather than fail. */
static int memory_readable(const void *data,uint64_t bytes)
{
#if defined(__APPLE__)
    mach_vm_address_t at=(mach_vm_address_t)(uintptr_t)data,end=at+bytes;
    if(end<at) return 0;
    while(at<end) {
        mach_vm_address_t region=at; mach_vm_size_t size=0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object=MACH_PORT_NULL;
        if(mach_vm_region(mach_task_self(),&region,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)!=KERN_SUCCESS)
            return 0;
        if(region>at || !(info.protection & VM_PROT_READ) || !size) return 0;
        at=region+size;
    }
    return 1;
#else
    uintptr_t begin=(uintptr_t)data,end=begin+(uintptr_t)bytes,at=begin;
    FILE *maps;
    char line[512];
    if(end<begin) return 0;
#if defined(__linux__) && defined(SYS_process_vm_readv)
    {
        /* One byte of every page, read through the kernel: an inaccessible page makes the
         * read stop short instead of raising the trap. Parsing /proc/self/maps for every
         * hash refresh took a quarter of the GPU thread on a Steam Deck with the HD packs;
         * it stays as the fallback where the call is refused (seccomp, old kernels). */
        static int refused;
        static uintptr_t page;
        if(!refused) {
            char sink[64];
            struct iovec local, remote[64];
            if(!page) { long size=sysconf(_SC_PAGESIZE); page=size>0 ? (uintptr_t)size : 4096u; }
            at=begin&~(page-1);
            while(at<end) {
                long got; unsigned n=0;
                for(;n<64 && at<end;n++,at+=page) {
                    remote[n].iov_base=(void *)(at<begin ? begin : at); remote[n].iov_len=1;
                }
                local.iov_base=sink; local.iov_len=n;
                got=syscall(SYS_process_vm_readv,(long)getpid(),&local,1ul,remote,(unsigned long)n,0ul);
                if(got==(long)n) continue;
                if(got<0 && (errno==ENOSYS || errno==EPERM)) { refused=1; break; }
                return 0;
            }
            if(!refused) return 1;
            at=begin;
        }
    }
#endif
    maps=fopen("/proc/self/maps","r");
    if(!maps) return 1;
    while(at<end && fgets(line,sizeof line,maps)) {
        unsigned long long low,high; char perms[8];
        if(sscanf(line,"%llx-%llx %7s",&low,&high,perms)!=3) continue;
        if(high<=at) continue;
        if(low>at || perms[0]!='r') { fclose(maps); return 0; }
        at=(uintptr_t)high;
    }
    fclose(maps);
    return at>=end;
#endif
}
static int write_png(const char *path,const uint8_t *bgra,uint32_t w,uint32_t h)
{
    size_t i,count=(size_t)w*h;
    uint8_t *rgba=malloc(count*4);
    int ok;
    if(!rgba) return 0;
    for(i=0;i<count;i++) {
        rgba[i*4]=bgra[i*4+2]; rgba[i*4+1]=bgra[i*4+1]; rgba[i*4+2]=bgra[i*4]; rgba[i*4+3]=bgra[i*4+3];
    }
    ok=stbi_write_png(path,(int)w,(int)h,4,rgba,(int)(w*4));
    free(rgba);
    return ok;
}
/* BGRA, as the Windows decoder returns it. A missing or malformed file is permanent (retry 0). */
static uint8_t *read_png(const char *path,uint32_t *w,uint32_t *h,int *retry)
{
    int x=0,y=0,n=0;
    uint8_t *data;
    size_t i,count;
    *retry=0;
    if(!stbi_info(path,&x,&y,&n) || x<=0 || y<=0 || x>16384 || y>16384 || (uint64_t)x*y*4>g_budget/4) return NULL;
    data=stbi_load(path,&x,&y,&n,4);
    if(!data) return NULL;
    *w=(uint32_t)x; *h=(uint32_t)y;
    count=(size_t)x*y;
    for(i=0;i<count;i++) { uint8_t t=data[i*4]; data[i*4]=data[i*4+2]; data[i*4+2]=t; }
    return data;
}

#if defined(__APPLE__) && !defined(TEXTURE_PACK_PORTABLE_SHA)
/* The system's SHA-256 uses the CPU's hash instructions; this runs on every texture bind. */
static int sha_start(Sha *sha) { return CC_SHA256_Init(&sha->ctx); }
static int sha_add(Sha *sha,const void *data,size_t bytes)
{
    const uint8_t *p=data;
    while(bytes) {
        CC_LONG take=bytes>0x40000000u ? 0x40000000u : (CC_LONG)bytes;
        if(!CC_SHA256_Update(&sha->ctx,p,take)) return 0;
        p+=take; bytes-=take;
    }
    return 1;
}
static int sha_finish(Sha *sha,uint8_t out[32]) { return CC_SHA256_Final(out,&sha->ctx); }
#else
static const uint32_t k_sha256[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha_block_c(uint32_t state[8],const uint8_t *p)
{
    uint32_t w[64],a,b,c,d,e,f,g,h,t1,t2,i;
    for(i=0;i<16;i++) w[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|(uint32_t)p[i*4+2]<<8|p[i*4+3];
    for(i=16;i<64;i++) {
        uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3),s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];
    for(i=0;i<64;i++) {
        t1=h+(ROR(e,6)^ROR(e,11)^ROR(e,25))+((e&f)^(~e&g))+k_sha256[i]+w[i];
        t2=(ROR(a,2)^ROR(a,13)^ROR(a,22))+((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(TEXTURE_PACK_NO_SHA_NI)
/* x86 SHA extensions (Intel since Goldmont/Ice Lake, AMD since Zen; the Steam Deck has them).
 * The texture hash runs on every bind refresh, and the C rounds above made it the largest
 * cost on the GPU thread on Linux, where no system SHA-256 is used. Same digest, checked
 * against the C rounds by the fixture. */
#include <cpuid.h>
#include <immintrin.h>
#define TEXTURE_PACK_SHA_NI 1
__attribute__((target("sha,sse4.1,ssse3")))
static void sha_blocks_ni(uint32_t state[8],const uint8_t *p,size_t blocks)
{
    const __m128i swap=_mm_set_epi64x(0x0c0d0e0f08090a0bULL,0x0405060700010203ULL);
    __m128i abef,cdgh,tmp,saved_abef,saved_cdgh,msg,w[4];
    unsigned g;
    tmp=_mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&state[0]),0xB1);   /* CDAB */
    cdgh=_mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&state[4]),0x1B);  /* EFGH */
    abef=_mm_alignr_epi8(tmp,cdgh,8);
    cdgh=_mm_blend_epi16(cdgh,tmp,0xF0);
    while(blocks--) {
        saved_abef=abef; saved_cdgh=cdgh;
        for(g=0;g<4;g++) w[g]=_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p+16*g)),swap);
        for(g=0;g<16;g++) {
            /* Four rounds on W[4g..4g+3], then the schedule's W[4g+16..4g+19] into the same slot. */
            msg=_mm_add_epi32(w[g&3],_mm_loadu_si128((const __m128i *)&k_sha256[4*g]));
            cdgh=_mm_sha256rnds2_epu32(cdgh,abef,msg);
            abef=_mm_sha256rnds2_epu32(abef,cdgh,_mm_shuffle_epi32(msg,0x0E));
            if(g<12)
                w[g&3]=_mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w[g&3],w[(g+1)&3]),
                                                          _mm_alignr_epi8(w[(g+3)&3],w[(g+2)&3],4)),w[(g+3)&3]);
        }
        abef=_mm_add_epi32(abef,saved_abef); cdgh=_mm_add_epi32(cdgh,saved_cdgh);
        p+=64;
    }
    tmp=_mm_shuffle_epi32(abef,0x1B);    /* FEBA */
    cdgh=_mm_shuffle_epi32(cdgh,0xB1);   /* DCHG */
    _mm_storeu_si128((__m128i *)&state[0],_mm_blend_epi16(tmp,cdgh,0xF0));   /* DCBA */
    _mm_storeu_si128((__m128i *)&state[4],_mm_alignr_epi8(cdgh,tmp,8));      /* HGFE */
}
static int sha_ni_available(void)
{
    static int known=-1;
    unsigned a,b,c,d;
    if(known<0) {
        known=__get_cpuid(1,&a,&b,&c,&d) && (c&bit_SSE4_1) && (c&bit_SSSE3) &&
              __get_cpuid_count(7,0,&a,&b,&c,&d) && (b&(1u<<29));
        if(getenv("RECOMP_TEXTURE_SHA_C")) known=0;   /* diagnostics: force the C rounds */
    }
    return known;
}
#endif
static void sha_blocks(uint32_t state[8],const uint8_t *p,size_t blocks)
{
#if defined(TEXTURE_PACK_SHA_NI)
    if(sha_ni_available()) { sha_blocks_ni(state,p,blocks); return; }
#endif
    for(;blocks;blocks--,p+=64) sha_block_c(state,p);
}
static int sha_start(Sha *sha)
{
    static const uint32_t init[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(sha->state,init,sizeof init); sha->bytes=0; sha->used=0;
    return 1;
}
static int sha_add(Sha *sha,const void *data,size_t bytes)
{
    const uint8_t *p=data;
    sha->bytes+=bytes;
    if(sha->used) {
        size_t take=64-sha->used; if(take>bytes) take=bytes;
        memcpy(sha->block+sha->used,p,take); sha->used+=take; p+=take; bytes-=take;
        if(sha->used<64) return 1;
        sha_blocks(sha->state,sha->block,1); sha->used=0;
    }
    if(bytes>=64) { sha_blocks(sha->state,p,bytes/64); p+=bytes&~(size_t)63; bytes&=63; }
    memcpy(sha->block,p,bytes); sha->used=bytes;
    return 1;
}
static int sha_finish(Sha *sha,uint8_t out[32])
{
    uint64_t bits=sha->bytes*8; unsigned i;
    uint8_t pad=0x80,zero=0,length[8];
    sha_add(sha,&pad,1);
    while(sha->used!=56) sha_add(sha,&zero,1);
    for(i=0;i<8;i++) length[i]=(uint8_t)(bits>>(56-8*i));
    sha_add(sha,length,8);
    for(i=0;i<8;i++) { out[i*4]=(uint8_t)(sha->state[i]>>24); out[i*4+1]=(uint8_t)(sha->state[i]>>16); out[i*4+2]=(uint8_t)(sha->state[i]>>8); out[i*4+3]=(uint8_t)sha->state[i]; }
    return 1;
}
#endif
static void host_init(void) {}
static void host_shutdown(void) {}
#endif

void texture_pack_stats(uint64_t *resident_bytes, uint64_t *budget_bytes)
{
    if (resident_bytes) *resident_bytes = texture_pack_active() ? g_resident : 0;
    if (budget_bytes) *budget_bytes = texture_pack_active() ? g_budget : 0;
}
