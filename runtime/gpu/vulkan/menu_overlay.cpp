// The settings menu, drawn the way Skate 3's port draws its own: a category
// rail on the left, a column of rows with their values, a description panel
// on the right and a legend of buttons along the bottom. Everything is drawn
// with ImGui's draw lists from a copy of the menu's state (menu::Snapshot);
// ImGui's own widgets and input are not used, the controller is the menu's.
#include "menu_overlay.h"

#if defined(MW2_HAVE_VULKAN) && defined(MW2_USE_SDL)
#include "pipeline.h"
#include "../../menu/menu.h"
#include "../../log.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>

// The launcher's fonts, built into the game (CMakeLists.txt).
extern const unsigned char kFontMenu[], kFontHeading[], kFontBody[];
extern const unsigned kFontMenuSize, kFontHeadingSize, kFontBodySize;

namespace
{
    constexpr ImU32 kDim = IM_COL32(4, 10, 10, 200);
    constexpr ImU32 kSelFill = IM_COL32(10, 12, 12, 255);
    constexpr ImU32 kSelText = IM_COL32(250, 252, 252, 255);
    constexpr ImU32 kPanel = IM_COL32(235, 236, 236, 255);
    constexpr ImU32 kRowText = IM_COL32(15, 17, 18, 255);
    constexpr ImU32 kRailPanel = IM_COL32(14, 56, 53, 170);
    constexpr ImU32 kText = IM_COL32(235, 242, 241, 255);
    constexpr ImU32 kTextDim = IM_COL32(224, 235, 234, 191);
    constexpr ImU32 kTextFaint = IM_COL32(150, 158, 158, 255);
    constexpr ImU32 kAccent = IM_COL32(213, 235, 10, 255);
    constexpr ImU32 kAccentDark = IM_COL32(13, 15, 5, 255);
    constexpr ImU32 kWarn = IM_COL32(245, 197, 87, 255);
    constexpr ImU32 kDescPanel = IM_COL32(11, 46, 43, 170);
    constexpr ImU32 kChevOff = IM_COL32(0, 0, 0, 56);
    constexpr ImU32 kChevOffDark = IM_COL32(255, 255, 255, 64);
    constexpr ImU32 kChip = IM_COL32(238, 240, 240, 255);
    constexpr ImU32 kChipText = IM_COL32(15, 18, 20, 255);
    constexpr ImU32 kLegendText = IM_COL32(228, 236, 235, 255);

    struct State
    {
        bool ready = false, failed = false;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::unordered_map<VkImage, VkImageView> views;
        PFN_vkCmdBeginRenderingKHR beginRendering = nullptr;
        PFN_vkCmdEndRenderingKHR endRendering = nullptr;
        ImFont* title = nullptr;
        ImFont* rows = nullptr;
        ImFont* body = nullptr;
        float fontHeight = 0;   // the screen height the fonts were made for
        // The frame counter: presents in the last second.
        std::chrono::steady_clock::time_point second;
        uint32_t counted = 0, shown = 0;
    } o;

    bool Begin(const vk::overlay::Device& d, float height)
    {
        if (o.failed) return false;
        if (o.ready && o.format == d.format && o.fontHeight == height) return true;
        if (o.ready)
        {
            // Another format or another screen: made again for it, once the
            // queue it shares has nothing of it left in flight.
            {
                std::lock_guard queue(vk::pipeline::QueueMutex());
                vkQueueWaitIdle(d.queue);
            }
            ImGui_ImplVulkan_Shutdown();
            ImGui::DestroyContext();
            vk::overlay::ForgetSwapchain(d.device);
            o.ready = false;
        }
        if (!o.pool)
        {
            const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 };
            VkDescriptorPoolCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            info.maxSets = 8;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;
            if (vkCreateDescriptorPool(d.device, &info, nullptr, &o.pool) != VK_SUCCESS)
            {
                LOGW("menu: no descriptor pool; the settings menu cannot be drawn");
                o.failed = true;
                return false;
            }
        }
        o.beginRendering = PFN_vkCmdBeginRenderingKHR(vkGetDeviceProcAddr(d.device, "vkCmdBeginRenderingKHR"));
        o.endRendering = PFN_vkCmdEndRenderingKHR(vkGetDeviceProcAddr(d.device, "vkCmdEndRenderingKHR"));
        if (!o.beginRendering || !o.endRendering) { o.failed = true; return false; }

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // Sized for the screen: a row's text is a thirtieth of its height.
        const float unit = std::max(height / 1080.0f, 0.5f);
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        o.title = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontHeading), int(kFontHeadingSize),
                                                 56.0f * unit, &config);
        o.rows = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontMenu), int(kFontMenuSize),
                                                34.0f * unit, &config);
        o.body = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontBody), int(kFontBodySize),
                                                26.0f * unit, &config);

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_2;
        info.Instance = d.instance;
        info.PhysicalDevice = d.physical;
        info.Device = d.device;
        info.QueueFamily = d.queueFamily;
        info.Queue = d.queue;
        info.DescriptorPool = o.pool;
        info.MinImageCount = 2;
        info.ImageCount = std::max(d.images, 2u);
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.UseDynamicRendering = true;
        o.format = d.format;
        info.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &o.format;
        if (!ImGui_ImplVulkan_Init(&info))
        {
            LOGW("menu: ImGui's Vulkan renderer would not start; the settings menu cannot be drawn");
            ImGui::DestroyContext();
            o.failed = true;
            return false;
        }
        {
            // The font atlas goes up through the queue the renderer shares.
            std::lock_guard queue(vk::pipeline::QueueMutex());
            ImGui_ImplVulkan_CreateFontsTexture();
        }
        o.fontHeight = height;
        o.ready = true;
        LOGI("menu: overlay ready");
        return true;
    }

    VkImageView ViewOf(VkDevice device, VkImage image)
    {
        auto found = o.views.find(image);
        if (found != o.views.end()) return found->second;
        VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = o.format;
        info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        o.views[image] = view;
        return view;
    }

    // ---- drawing -----------------------------------------------------------

    float Snap(float v) { return float(int(v + 0.5f)); }

    void TextIn(ImDrawList* list, ImFont* font, ImVec2 at, ImU32 colour, const std::string& text,
                float wrap = 0.0f)
    {
        list->AddText(font, font->FontSize, at, colour, text.c_str(), nullptr, wrap);
    }

    float WidthOf(ImFont* font, const std::string& text)
    {
        return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text.c_str()).x;
    }

    void Chevron(ImDrawList* list, ImVec2 centre, float size, bool right, ImU32 colour)
    {
        const float h = size * 0.5f, w = size * 0.3f;
        if (right)
            list->AddTriangleFilled({ centre.x - w, centre.y - h }, { centre.x + w, centre.y },
                                    { centre.x - w, centre.y + h }, colour);
        else
            list->AddTriangleFilled({ centre.x + w, centre.y - h }, { centre.x - w, centre.y },
                                    { centre.x + w, centre.y + h }, colour);
    }

    void Legend(ImDrawList* list, float x, float y, float unit, const char* chip, const char* label)
    {
        const float r = Snap(18.0f * unit);
        list->AddCircleFilled({ x + r, y + r }, r, kChip, 24);
        const ImVec2 chipSize = o.body->CalcTextSizeA(o.body->FontSize, FLT_MAX, 0, chip);
        list->AddText(o.body, o.body->FontSize, { x + r - chipSize.x * 0.5f, y + r - chipSize.y * 0.5f },
                      kChipText, chip);
        list->AddText(o.body, o.body->FontSize, { x + 2 * r + 10 * unit, y + r - chipSize.y * 0.5f },
                      kLegendText, label);
    }

    void DrawMenu(const menu::View& view, ImVec2 size)
    {
        ImDrawList* list = ImGui::GetForegroundDrawList();
        const float unit = size.y / 1080.0f;
        list->AddRectFilled({ 0, 0 }, size, kDim);

        // The columns: the rail, the rows, the description, centred.
        const float margin = Snap(60 * unit), gap = Snap(24 * unit);
        const float railW = Snap(280 * unit), descW = Snap(380 * unit);
        const float contentW = std::max(size.x - 2 * margin - railW - descW - 2 * gap, 300 * unit);
        const float total = railW + contentW + descW + 2 * gap;
        const float railX = Snap((size.x - total) * 0.5f);
        const float contentX = railX + railW + gap, descX = contentX + contentW + gap;
        const float top = Snap(70 * unit);

        TextIn(list, o.title, { railX, top }, kText, "SETTINGS");
        list->AddRectFilled({ railX, top + o.title->FontSize + 6 * unit },
                            { railX + 90 * unit, top + o.title->FontSize + 11 * unit }, kAccent);
        const float columnsY = top + o.title->FontSize + 40 * unit;
        const float rowH = Snap(58 * unit), rowGap = Snap(8 * unit);

        // The rail.
        for (size_t i = 0; i < view.categories.size(); i++)
        {
            const float y = columnsY + float(i) * (rowH + rowGap);
            const bool on = int(i) == view.category;
            list->AddRectFilled({ railX, y }, { railX + railW, y + rowH }, on ? kAccent : kRailPanel, 6 * unit);
            TextIn(list, o.rows, { railX + 22 * unit, y + (rowH - o.rows->FontSize) * 0.5f },
                   on ? kAccentDark : kText, view.categories[i]);
        }

        // The rows.
        float y = columnsY;
        std::string description;
        bool restartRow = false;
        for (size_t i = 0; i < view.rows.size(); i++)
        {
            const menu::Row& row = view.rows[i];
            if (row.header)
            {
                TextIn(list, o.body, { contentX + 4 * unit, y + 8 * unit }, kTextFaint, row.label);
                y += o.body->FontSize + 18 * unit;
                continue;
            }
            const bool on = int(i) == view.row;
            if (on) { description = row.description; restartRow = row.restart; }
            list->AddRectFilled({ contentX, y }, { contentX + contentW, y + rowH }, on ? kSelFill : kPanel,
                                6 * unit);
            const ImU32 text = on ? kSelText : kRowText;
            const float textY = y + (rowH - o.rows->FontSize) * 0.5f;
            TextIn(list, o.rows, { contentX + 22 * unit, textY }, text, row.label);
            if (!row.action)
            {
                const float right = contentX + contentW - 22 * unit;
                const float valueW = WidthOf(o.rows, row.value);
                const float chev = 22 * unit;
                const ImU32 lit = on ? kAccent : kRowText, unlit = on ? kChevOffDark : kChevOff;
                Chevron(list, { right - chev * 0.5f, y + rowH * 0.5f }, chev, true, row.canRight ? lit : unlit);
                const float valueX = right - chev - 14 * unit - valueW;
                TextIn(list, o.rows, { valueX, textY }, text, row.value);
                Chevron(list, { valueX - 14 * unit - chev * 0.5f, y + rowH * 0.5f }, chev, false,
                        row.canLeft ? lit : unlit);
            }
            y += rowH + rowGap;
        }

        // The description.
        const float descTop = columnsY;
        const float descH = std::max(y - columnsY - rowGap, 320 * unit);
        list->AddRectFilled({ descX, descTop }, { descX + descW, descTop + descH }, kDescPanel, 6 * unit);
        const float pad = 22 * unit;
        float dy = descTop + pad;
        TextIn(list, o.body, { descX + pad, dy }, kTextDim,
               description.empty() ? view.categoryDescription : description, descW - 2 * pad);
        if (restartRow)
        {
            const char* note = "Takes effect the next time the game starts.";
            TextIn(list, o.body, { descX + pad, descTop + descH - pad - o.body->FontSize }, kWarn, note,
                   descW - 2 * pad);
        }

        // The notice, and the legend.
        const float legendY = size.y - Snap(80 * unit);
        if (!view.notice.empty())
            TextIn(list, o.rows, { railX, legendY - o.rows->FontSize - 24 * unit }, kAccent, view.notice);
        else if (view.restartPending)
            TextIn(list, o.body, { railX, legendY - o.body->FontSize - 24 * unit }, kWarn,
                   "Some changes wait for the next start: System > Apply and Quit.");
        float lx = railX;
        const struct { const char* chip; const char* label; } legend[] = {
            { "A", "Select" }, { "B", "Close" }, { "<>", "Change" }, { "LB", "" }, { "RB", "Category" },
        };
        for (const auto& item : legend)
        {
            Legend(list, lx, legendY, unit, item.chip, item.label);
            lx += 36 * unit + 10 * unit + WidthOf(o.body, item.label) + (item.label[0] ? 34 * unit : 6 * unit);
        }
    }

    void DrawCounter(ImVec2 size)
    {
        const auto now = std::chrono::steady_clock::now();
        o.counted++;
        if (now - o.second >= std::chrono::seconds(1))
        {
            o.shown = o.counted;
            o.counted = 0;
            o.second = now;
        }
        char text[32];
        std::snprintf(text, sizeof text, "%u FPS", o.shown);
        ImDrawList* list = ImGui::GetForegroundDrawList();
        const float unit = size.y / 1080.0f;
        const ImVec2 at{ 24 * unit, 20 * unit };
        const ImVec2 extent = o.body->CalcTextSizeA(o.body->FontSize, FLT_MAX, 0, text);
        list->AddRectFilled({ at.x - 10 * unit, at.y - 4 * unit },
                            { at.x + extent.x + 10 * unit, at.y + extent.y + 4 * unit }, IM_COL32(0, 0, 0, 150),
                            4 * unit);
        list->AddText(o.body, o.body->FontSize, at, o.shown >= 58 ? kAccent : kWarn, text);
    }
}

bool vk::overlay::Wanted() { return menu::Open() || menu::FrameCounter(); }

bool vk::overlay::Draw(const Device& d, VkCommandBuffer commands, VkImage image, VkExtent2D extent)
{
    const menu::View view = menu::Snapshot();
    const bool counter = menu::FrameCounter() && !view.open;
    if (!view.open && !counter) return false;
    if (!Begin(d, float(extent.height))) return false;
    const VkImageView target = ViewOf(d.device, image);
    if (!target) return false;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (view.open) DrawMenu(view, io.DisplaySize);
    else DrawCounter(io.DisplaySize);
    ImGui::Render();

    VkImageMemoryBarrier toAttachment{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = image;
    toAttachment.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttachment);

    VkRenderingAttachmentInfoKHR colour{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    colour.imageView = target;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfoKHR rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    rendering.renderArea = { { 0, 0 }, extent };
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;
    o.beginRendering(commands, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commands);
    o.endRendering(commands);
    return true;
}

void vk::overlay::ForgetSwapchain(VkDevice device)
{
    for (auto& [image, view] : o.views) vkDestroyImageView(device, view, nullptr);
    o.views.clear();
}

void vk::overlay::Shutdown(VkDevice device)
{
    if (o.ready)
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();
        o.ready = false;
    }
    ForgetSwapchain(device);
    if (o.pool) vkDestroyDescriptorPool(device, o.pool, nullptr);
    o.pool = VK_NULL_HANDLE;
}

#elif defined(MW2_HAVE_VULKAN)
bool vk::overlay::Wanted() { return false; }
bool vk::overlay::Draw(const Device&, VkCommandBuffer, VkImage, VkExtent2D) { return false; }
void vk::overlay::ForgetSwapchain(VkDevice) {}
void vk::overlay::Shutdown(VkDevice) {}
#endif
