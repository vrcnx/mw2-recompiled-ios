#pragma once
// The settings menu (menu/menu.h) and the frame counter, drawn by Dear ImGui
// over the presented frame, in the presenter's own command buffer.
#ifdef MW2_HAVE_VULKAN
#include <vulkan/vulkan.h>
#include <cstdint>

namespace vk::overlay
{
    struct Device
    {
        VkInstance instance;
        VkPhysicalDevice physical;
        VkDevice device;
        VkQueue queue;
        uint32_t queueFamily;
        VkFormat format;       // the swapchain's
        uint32_t images;       // how many it has
    };

    // Whether there is anything to draw this frame.
    bool Wanted();
    // Records the overlay into `commands` over `image`, which holds the frame
    // in TRANSFER_DST_OPTIMAL. Leaves it in COLOR_ATTACHMENT_OPTIMAL and
    // returns true when it drew; leaves it alone and returns false otherwise.
    bool Draw(const Device& device, VkCommandBuffer commands, VkImage image, VkExtent2D extent);
    // The swapchain was replaced (after the device is idle): the views of its
    // images go with it.
    void ForgetSwapchain(VkDevice device);
    void Shutdown(VkDevice device);
}
#endif
