#ifndef DEFJAM_FG34_H
#define DEFJAM_FG34_H
#include <vulkan/vulkan_core.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__GNUC__)
#define FG34_API __attribute__((visibility("default")))
#else
#define FG34_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Borrowed handles. This module never creates/destroys the game's device,
 * starts a second loader, submits a queue, presents, or advances the guest. */
typedef struct {
    uint32_t abi, size;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandBuffer command;
    uint32_t family, width, height;
    float flow_scale; /* user fraction .25..1, converted to reciprocal inside */
    const char *shader_directory; /* importer output, fp32/ or fp16/ appended */
    int fp16;
    unsigned multiplier; /* ABI2: 2..4 */
} FG34Info;
FG34_API void *defjam_fg34_create(const FG34Info *,char *error,size_t capacity);
/* Caller has begun command and waited for the preceding presenter fence.
 * Inputs are immutable snapshots in SHADER_READ_ONLY_OPTIMAL; restored on exit.
 * Output is valid in TRANSFER_SRC_OPTIMAL; unique=0 means cached phase. */
FG34_API int defjam_fg34_record(void *,VkImage previous,VkImage current,
    uint64_t previous_serial,uint64_t current_serial,unsigned phase,VkImage *output,int *unique,
    char *error,size_t capacity);
/* Caller first waits for all commands using these private resources. */
FG34_API void defjam_fg34_destroy(void *);
#ifdef __cplusplus
}
#endif
#endif
