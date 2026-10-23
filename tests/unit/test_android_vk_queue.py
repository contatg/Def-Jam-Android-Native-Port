"""Production Vulkan scheduler under mocked GPU completion; no phone/GPU claim.

Compiles the entire backend, then links only the functions used by the fixture.
The fixture requires the Vulkan/volk/shaderc headers used by the Android build.
DEFJAM_TEST_VK_HEADERS may point at a host-only header bundle.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_frame_contexts_and_completion(tmp_path):
    # Only the native-window type is needed by the loader's declarations.
    # A host test does not use Android's window implementation or an NDK ABI.
    (tmp_path / 'android').mkdir()
    (tmp_path / 'android/native_window.h').write_text('typedef struct ANativeWindow ANativeWindow;\n')
    fixture = tmp_path / 'queue.c'
    fixture.write_text(r'''
#define __ANDROID__ 1
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include "d3d8_vk.c"
#include <assert.h>
static unsigned waits, resets, submits, freed, device_lost, submit_failed;
static unsigned gpu_pending[2];
static uint64_t gpu_serial[2], queue_serial;
static unsigned allocation_step, fail_allocation, swap_mode, semaphore_number, destroyed_sync;
#define H(type, number) ((type)(uintptr_t)(number))
static unsigned slot_of(VkFence f) { assert(f == H(VkFence, 100) || f == H(VkFence, 200)); return f == H(VkFence, 200); }
static VKAPI_ATTR VkResult VKAPI_CALL reset_pool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags f) {
    (void)d;(void)f;unsigned slot=p==H(VkCommandPool,20); assert(!gpu_pending[slot]);++resets;return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL reset_dpool(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f) {
    (void)d;(void)f;assert(!gpu_pending[p==H(VkDescriptorPool,40)]);return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL begin_cb(VkCommandBuffer b,const VkCommandBufferBeginInfo *i) { (void)b;(void)i;return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL end_cb(VkCommandBuffer b) { (void)b;return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL end_pass(VkCommandBuffer b) { (void)b; }
static VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice d,VkFence f) { (void)d;return gpu_pending[slot_of(f)]?VK_NOT_READY:VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL wait_fence(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
    (void)d;assert(n==1&&all&&timeout==UINT64_MAX);++waits;
    if(device_lost)return VK_ERROR_DEVICE_LOST;
    uint64_t serial=gpu_serial[slot_of(*f)];
    for(unsigned i=0;i<2;++i)if(gpu_serial[i]<=serial)gpu_pending[i]=0;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL reset_fence(VkDevice d,uint32_t n,const VkFence *f) {
    (void)d;assert(n==1&&!gpu_pending[slot_of(*f)]);return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f) {
    (void)q;assert(n==1&&i->commandBufferCount==2);unsigned slot=slot_of(f);
    assert(!gpu_pending[slot]);
    assert(i->pCommandBuffers[0]==H(VkCommandBuffer,slot?22:12));
    assert(i->pCommandBuffers[1]==H(VkCommandBuffer,slot?21:11));
    if(i->waitSemaphoreCount)assert(i->waitSemaphoreCount==1&&*i->pWaitDstStageMask==VK_PIPELINE_STAGE_TRANSFER_BIT);
    if(i->signalSemaphoreCount)assert(i->signalSemaphoreCount==1&&*i->pSignalSemaphores==H(VkSemaphore,99));
    if(submit_failed)return VK_ERROR_DEVICE_LOST;
    ++submits;gpu_pending[slot]=1;gpu_serial[slot]=++queue_serial;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice d,VkImageView v,const VkAllocationCallbacks *a) { (void)d;(void)v;(void)a; }
static VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice d,VkImage i,const VkAllocationCallbacks *a) { (void)d;(void)i;(void)a; }
static VKAPI_ATTR void VKAPI_CALL free_mem(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a) { (void)d;(void)m;(void)a;++freed; }
static VKAPI_ATTR VkResult VKAPI_CALL invalidate(VkDevice d,uint32_t n,const VkMappedMemoryRange *m) { (void)d;(void)n;(void)m;return VK_SUCCESS; }
static int alloc_ok(void) { return ++allocation_step!=fail_allocation; }
static VKAPI_ATTR VkResult VKAPI_CALL create_pool(VkDevice d,const VkCommandPoolCreateInfo *i,const VkAllocationCallbacks *a,VkCommandPool *p) {
    (void)d;(void)i;(void)a;if(!alloc_ok())return VK_ERROR_OUT_OF_DEVICE_MEMORY;*p=H(VkCommandPool,20);return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_cbs(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *p) {
    (void)d;assert(i->commandBufferCount==2);if(!alloc_ok())return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    p[0]=H(VkCommandBuffer,21);p[1]=H(VkCommandBuffer,22);return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice d,const VkFenceCreateInfo *i,const VkAllocationCallbacks *a,VkFence *p) {
    (void)d;(void)i;(void)a;if(!alloc_ok())return VK_ERROR_OUT_OF_DEVICE_MEMORY;*p=H(VkFence,200);return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_sem(VkDevice d,const VkSemaphoreCreateInfo *i,const VkAllocationCallbacks *a,VkSemaphore *p) {
    (void)d;(void)i;(void)a;if(!alloc_ok())return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *p=H(VkSemaphore,swap_mode?1000+(++semaphore_number):2);return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_dpool(VkDevice d,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorPool *p) {
    (void)d;(void)i;(void)a;if(!alloc_ok())return VK_ERROR_OUT_OF_DEVICE_MEMORY;*p=H(VkDescriptorPool,40);return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks *a) {
    (void)d;(void)a;assert(p==H(VkCommandPool,20));++destroyed_sync;
}
static VKAPI_ATTR void VKAPI_CALL destroy_dpool(VkDevice d,VkDescriptorPool p,const VkAllocationCallbacks *a) {
    (void)d;(void)a;assert(p==H(VkDescriptorPool,40));++destroyed_sync;
}
static VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice d,VkFence p,const VkAllocationCallbacks *a) {
    (void)d;(void)a;assert(p==H(VkFence,200)&&!gpu_pending[1]);++destroyed_sync;
}
static VKAPI_ATTR void VKAPI_CALL destroy_sem(VkDevice d,VkSemaphore p,const VkAllocationCallbacks *a) {
    (void)d;(void)p;(void)a;assert(!gpu_pending[0]&&!gpu_pending[1]);++destroyed_sync;
}
PFN_vkResetCommandPool vkResetCommandPool=reset_pool;
PFN_vkResetDescriptorPool vkResetDescriptorPool=reset_dpool;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer=begin_cb;
PFN_vkEndCommandBuffer vkEndCommandBuffer=end_cb;
PFN_vkCmdEndRenderPass vkCmdEndRenderPass=end_pass;
PFN_vkGetFenceStatus vkGetFenceStatus=fence_status;
PFN_vkWaitForFences vkWaitForFences=wait_fence;
PFN_vkResetFences vkResetFences=reset_fence;
PFN_vkQueueSubmit vkQueueSubmit=submit;
PFN_vkDestroyImageView vkDestroyImageView=destroy_view;
PFN_vkDestroyImage vkDestroyImage=destroy_image;
PFN_vkFreeMemory vkFreeMemory=free_mem;
PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges=invalidate;
PFN_vkCreateCommandPool vkCreateCommandPool=create_pool;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers=allocate_cbs;
PFN_vkCreateFence vkCreateFence=create_fence;
PFN_vkCreateSemaphore vkCreateSemaphore=create_sem;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool=create_dpool;
PFN_vkDestroyCommandPool vkDestroyCommandPool=destroy_pool;
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool=destroy_dpool;
PFN_vkDestroyFence vkDestroyFence=destroy_fence;
PFN_vkDestroySemaphore vkDestroySemaphore=destroy_sem;
BOOL QueryPerformanceCounter(LARGE_INTEGER *p) { static int64_t t;p->QuadPart=++t;return TRUE; }

int main(void) {
    s_ps_uniform.valid=s_frame_uniform.valid=1;
    static uint8_t memory[4][128];
    Ring *rings[]={&vk.vtx,&vk.idx,&vk.ubo,&vk.stage};
    for(unsigned i=0;i<4;++i){rings[i]->map=memory[i];rings[i]->size=128;}
    vk.ubo_align=16;s_vk_frame_count=2;
    for(unsigned i=0;i<2;++i){
        VkFrameSlot *f=&s_vk_frames[i];
        f->pool=H(VkCommandPool,10+i*10);f->dpool=H(VkDescriptorPool,30+i*10);
        f->cmd=H(VkCommandBuffer,11+i*10);f->cmd_up=H(VkCommandBuffer,12+i*10);
        f->fence=H(VkFence,100+i*100);f->acquired=H(VkSemaphore,1+i);
    }
    // First context returns after submission, while its ranges remain occupied.
    frame_begin();assert(vk.vtx.begin==0&&vk.vtx.end==64&&resets==1);
    assert(!s_ps_uniform.valid&&!s_frame_uniform.valid);
    VkDeviceSize at=ring_take(&vk.vtx,48,16);assert(at==0);memset(vk.vtx.map+at,0x11,48);
    retire(H(VkImage,1),H(VkDeviceMemory,1),H(VkImageView,1));
    assert(frame_submit_commands(vk.acquired,H(VkSemaphore,99),0)==VK_SUCCESS);
    assert(waits==0&&g_submits_done==0&&gpu_pending[0]&&g_submits_queued==1&&freed==0);
    frame_advance();frame_begin();assert(vk.vtx.begin==64&&vk.vtx.end==128&&waits==0);
    at=ring_take(&vk.vtx,48,16);assert(at==64);memset(vk.vtx.map+at,0x22,48);
    for(unsigned i=0;i<48;++i)assert(memory[0][i]==0x11);
    retire(H(VkImage,2),H(VkDeviceMemory,2),H(VkImageView,2));
    assert(frame_submit_commands(0,0,0)==VK_SUCCESS&&gpu_pending[1]);
    assert(g_submits_done==0&&freed==0);frame_advance();
    // The third frame waits for slot0, never resetting a pool still in flight.
    frame_begin();assert(waits==1&&g_submits_done==1&&freed==1&&gpu_pending[1]);
    assert(vk.vtx.begin==0&&vk.vtx.at==0&&vk.retired_n==1);
    // Readback completion follows queued serials, rather than an old done count.
    s_rb[0].state=RB_FLIGHT;s_rb[0].after=1;s_rb[0].seq=1;
    s_rb[0].buf.map=memory[0];s_rb[0].buf.coherent=1;
    D3D8Readback rb;assert(d3d8_readback_poll(&rb)==0);
    gpu_pending[1]=0;assert(d3d8_readback_poll(&rb)==1);
    assert(g_submits_done==2&&freed==2);
    // Reserve all draw ranges together: an index overflow flushes BEFORE any copy.
    vk.idx.at=62;draw_reserve(32,8,32);assert(!s_vk_wide_rings&&vk.idx.at==0&&vk.vtx.at==0);
    unsigned before=waits;at=ring_take(&vk.vtx,48,16);memset(memory[0]+at,0x33,48);
    assert(ring_take(&vk.vtx,32,16)==0&&waits>before);
    // A >half-ring upload drains both slots and uses full capacity, synchronously.
    before=waits;at=ring_take(&vk.stage,96,4);
    assert(s_vk_wide_rings&&at==0&&vk.stage.end==128&&vk.stage.at==96&&waits>before);
    assert(frame_submit_commands(0,0,1)==VK_SUCCESS&&!s_vk_frames[s_vk_frame_slot].pending);
    frame_advance();assert(!s_vk_wide_rings);frame_begin();assert(vk.stage.end-vk.stage.begin==64);
    // Failed submission never invents completion or waits on an unsignalled fence.
    uint64_t done=g_submits_done,queued=g_submits_queued;before=waits;submit_failed=1;
    assert(frame_submit_commands(0,0,0)==VK_ERROR_DEVICE_LOST);
    assert(g_submits_done==done&&g_submits_queued==queued&&waits==before);submit_failed=0;
    frame_begin();assert(frame_submit_commands(0,0,0)==VK_SUCCESS);frame_advance();
    frame_begin();assert(frame_submit_commands(0,0,0)==VK_SUCCESS);frame_advance();
    before=resets;device_lost=1;frame_begin();assert(resets==before&&!vk.frame_open);
    device_lost=0;assert(frame_drain_pending());assert(g_submits_done==g_submits_queued);
    // Single-context selection retains original full ring/synchronous path.
    s_vk_frame_count=1;s_vk_frame_slot=0;frame_begin();assert(vk.vtx.end==128);
    assert(frame_submit_commands(0,0,1)==VK_SUCCESS&&!gpu_pending[0]);
    // Opt-out and every partial second-context allocation failure retain slot0.
    VkDescriptorPoolCreateInfo dpci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    assert(!setenv("RECOMP_VK_ASYNC","0",1));allocation_step=0;frame_slots_make(&dpci);
    assert(s_vk_frame_count==1&&allocation_step==0);
    assert(!setenv("RECOMP_VK_ASYNC","1",1));
    for(unsigned failure=1;failure<=5;++failure){
        allocation_step=destroyed_sync=0;fail_allocation=failure;frame_slots_make(&dpci);
        assert(s_vk_frame_count==1&&s_vk_frames[0].pool==H(VkCommandPool,10));
        assert(!s_vk_frames[1].pool&&!s_vk_frames[1].fence&&!s_vk_frames[1].dpool);
        assert(failure<2||destroyed_sync>0);
    }
    fail_allocation=allocation_step=0;frame_slots_make(&dpci);assert(s_vk_frame_count==2);
    // Swap-image semaphores are distinct, and recreated only with no work pending.
    swap_mode=1;allocation_step=0;fail_allocation=2;vk.swap=H(VkSwapchainKHR,1);vk.swap_n=3;
    present_sync_reset();assert(!s_vk_present_ready);
    fail_allocation=0;present_sync_reset();assert(s_vk_present_ready);
    assert(vk.present_done[0]!=vk.present_done[1]&&vk.present_done[1]!=vk.present_done[2]);
    vk.swap=VK_NULL_HANDLE;present_sync_reset();assert(!s_vk_present_ready&&!vk.present_done[0]);
    puts("actual backend: async contexts, ranges, retirement, readback, overflow, errors, sync: PASS");
}
''')
    headers = Path(os.environ.get('DEFJAM_TEST_VK_HEADERS', ROOT / 'android/vendor/Vulkan-Headers/include'))
    includes = [tmp_path, headers, ROOT / 'tools/xboxrecomp/src/d3d', ROOT / 'tools/xboxrecomp/src/platform',
                ROOT / 'tools/xboxrecomp/src', ROOT / 'android/vendor/volk',
                ROOT / 'android/vendor/shaderc/include', ROOT / 'android/vendor/libadrenotools/include']
    out = tmp_path / 'queue'
    command = ['cc','-std=gnu11','-O1','-g','-ffunction-sections','-fdata-sections',
               '-fsanitize=address,undefined', '--param=asan-globals=0',
               *['-I'+str(p) for p in includes], str(fixture),
               '-Wl,--gc-sections','-lm','-ldl','-o',str(out)]
    subprocess.run(command, check=True, capture_output=True, text=True)
    subprocess.run([str(out)], check=True, capture_output=True, text=True, timeout=10,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
