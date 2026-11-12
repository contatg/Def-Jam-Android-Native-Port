/* Layout-specific dependencies, following the approach used by MostWanted.
 * Unknown layouts retain the original broad dependency. Never elide a same-
 * layout barrier: callers can use it to order writes in an unchanged layout. */
static uint64_t s_barrier_precise144,s_barrier_broad144;
static int layout_scope144(VkImageLayout layout,VkPipelineStageFlags *stage,VkAccessFlags *access)
{
    switch(layout){
    case VK_IMAGE_LAYOUT_UNDEFINED:*stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;*access=0;return 1;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:*stage=VK_PIPELINE_STAGE_TRANSFER_BIT;*access=VK_ACCESS_TRANSFER_READ_BIT;return 1;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:*stage=VK_PIPELINE_STAGE_TRANSFER_BIT;*access=VK_ACCESS_TRANSFER_WRITE_BIT;return 1;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;*access=VK_ACCESS_SHADER_READ_BIT;return 1;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;*access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;return 1;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;return 1;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:*stage=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;*access=0;return 1;
    default:return 0;
    }
}
