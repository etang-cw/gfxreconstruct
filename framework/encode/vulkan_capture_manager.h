/*
** Copyright (c) 2018-2020 Valve Corporation
** Copyright (c) 2018-2021 LunarG, Inc.
** Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#ifndef GFXRECON_ENCODE_VULKAN_CAPTURE_MANAGER_H
#define GFXRECON_ENCODE_VULKAN_CAPTURE_MANAGER_H

#include "encode/capture_settings.h"
#include "encode/descriptor_update_template_info.h"
#include "encode/handle_unwrap_memory.h"
#include "encode/parameter_encoder.h"
#include "encode/vulkan_handle_wrapper_util.h"
#include "encode/vulkan_handle_wrappers.h"
#include "encode/vulkan_state_tracker.h"
#include "format/api_call_id.h"
#include "format/format.h"
#include "format/platform_types.h"
#include "generated/generated_vulkan_dispatch_table.h"
#include "generated/generated_vulkan_command_buffer_util.h"
#include "util/compressor.h"
#include "util/defines.h"
#include "util/file_output_stream.h"
#include "util/keyboard.h"
#include "util/memory_output_stream.h"

#include "vulkan/vulkan.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// TODO (GH #9): Split CaptureManager into separate Vulkan and D3D12 class implementations, with a common base class.
#if defined(WIN32)
#include "encode/d3d12_dispatch_table.h"
#include "encode/dxgi_dispatch_table.h"
#include "generated/generated_dx12_wrappers.h"
#endif

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(encode)

class VulkanCaptureManager
{
  public:
    // Register special layer provided functions, which perform layer specific initialization.
    // These must be set before the application calls vkCreateInstance.
    static void SetLayerFuncs(PFN_vkCreateInstance create_instance, PFN_vkCreateDevice create_device);

    // Creates the capture manager instance if none exists, or increments a reference count if an instance already
    // exists. Intended to be called by the layer's vkCreateInstance function, before the driver's vkCreateInstance has
    // been called, to initialize capture resources.
    static bool CreateInstance();

    // Called by the layer's vkCreateInstance function, after the driver's vkCreateInstance function has been called, to
    // check for failure.  If vkCreateInstance failed, the reference count will be decremented and resources will be
    // released as necessry.  Allows a failed vkCreateInstance call to be logged to the capture file while performing
    // the appropriate resource cleanup.
    static void CheckCreateInstanceStatus(VkResult result);

    // Dectement the instance reference count, releasing resources when the count reaches zero.  Ignored if the count is
    // already zero.
    static void DestroyInstance();

    static VulkanCaptureManager* Get() { return instance_; }

    static format::HandleId GetUniqueId() { return ++unique_id_counter_; }

    static const LayerTable* GetLayerTable() { return &layer_table_; }

    void InitInstance(VkInstance* instance, PFN_vkGetInstanceProcAddr gpa);

    void InitDevice(VkDevice* device, PFN_vkGetDeviceProcAddr gpa);

// TODO (GH #9): Split CaptureManager into separate Vulkan and D3D12 class implementations, with a common base class.
#if defined(WIN32)
    //----------------------------------------------------------------------------
    /// \brief Initializes the DXGI dispatch table.
    ///
    /// Initializes the CaptureManager's internal DXGI dispatch table with functions
    /// loaded from the DXGI system DLL.  This dispatch table will be used by the
    /// 'wrapper' functions to invoke the 'real' DXGI function prior to processing
    /// the function parameters for encoding.
    ///
    /// \param dispatch_table A DxgiDispatchTable object contiaining the DXGI
    ///                       function pointers to be used for initialization.
    //----------------------------------------------------------------------------
    void InitDxgiDispatchTable(const DxgiDispatchTable& dispatch_table) { dxgi_dispatch_table_ = dispatch_table; }

    //----------------------------------------------------------------------------
    /// \brief Initializes the D3D12 dispatch table.
    ///
    /// Initializes the CaptureManager's internal D3D12 dispatch table with
    /// functions loaded from the D3D12 system DLL.  This dispatch table will be
    /// used by the 'wrapper' functions to invoke the 'real' D3D12 function prior
    /// to processing the function parameters for encoding.
    ///
    /// \param dispatch_table A D3D12DispatchTable object contiaining the DXGI
    ///                       function pointers to be used for initialization.
    //----------------------------------------------------------------------------
    void InitD3D12DispatchTable(const D3D12DispatchTable& dispatch_table) { d3d12_dispatch_table_ = dispatch_table; }

    //----------------------------------------------------------------------------
    /// \brief Retrieves the DXGI dispatch table.
    ///
    /// Retrieves the CaptureManager's internal DXGI dispatch table. Intended to be
    /// used by the 'wrapper' functions when invoking the 'real' DXGI functions.
    ///
    /// \return A DxgiDispatchTable object contiaining DXGI function pointers
    ///         retrieved from the system DLL.
    //----------------------------------------------------------------------------
    const DxgiDispatchTable& GetDxgiDispatchTable() const { return dxgi_dispatch_table_; }

    //----------------------------------------------------------------------------
    /// \brief Retrieves the D3D12 dispatch table.
    ///
    /// Retrieves the CaptureManager's internal D3D12 dispatch table. Intended to be
    /// used by the 'wrapper' functions when invoking the 'real' D3D12 functions.
    ///
    /// \return A D3D12DispatchTable object contiaining D3D12 function pointers
    ///         retrieved from the system DLL.
    //----------------------------------------------------------------------------
    const D3D12DispatchTable& GetD3D12DispatchTable() const { return d3d12_dispatch_table_; }

    //----------------------------------------------------------------------------
    /// \brief Increments the scope count for the current thread.
    ///
    /// Increments a per-thread scope count that is intended to indicate if an
    /// intercepted API call is being made directly by the application (count is
    /// equal to 1) or by another API call (count is greater than 1).
    ///
    /// \return The scope count for the current thread.
    //----------------------------------------------------------------------------
    uint32_t IncrementCallScope() { return ++call_scope_; }

    //----------------------------------------------------------------------------
    /// \brief Decrements the scope count for the current thread.
    ///
    /// Decrements a per-thread scope count that is intended to indicate if an
    /// intercepted API call is being made directly by the application (count is
    /// equal to 1) or by another API call (count is greater than 1).
    ///
    /// \return The scope count for the current thread.
    //----------------------------------------------------------------------------
    uint32_t DecrementCallScope() { return --call_scope_; }
#endif

    HandleUnwrapMemory* GetHandleUnwrapMemory()
    {
        auto thread_data = GetThreadData();
        assert(thread_data != nullptr);
        thread_data->handle_unwrap_memory_.Reset();
        return &thread_data->handle_unwrap_memory_;
    }

    ParameterEncoder* BeginTrackedApiCallTrace(format::ApiCallId call_id)
    {
        if (capture_mode_ != kModeDisabled)
        {
            return InitApiCallTrace(call_id);
        }

        return nullptr;
    }

    ParameterEncoder* BeginApiCallTrace(format::ApiCallId call_id)
    {
        if ((capture_mode_ & kModeWrite) == kModeWrite)
        {
            return InitApiCallTrace(call_id);
        }

        return nullptr;
    }

    ParameterEncoder* BeginMethodCallTrace(format::ApiCallId call_id, format::HandleId object_id)
    {
        if ((capture_mode_ & kModeWrite) == kModeWrite)
        {
            return InitMethodCallTrace(call_id, object_id);
        }

        return nullptr;
    }

    // Single object creation.
    template <typename ParentHandle, typename Wrapper, typename CreateInfo>
    void EndCreateApiCallTrace(VkResult                      result,
                               ParentHandle                  parent_handle,
                               typename Wrapper::HandleType* handle,
                               const CreateInfo*             create_info,
                               ParameterEncoder*             encoder)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && result == VK_SUCCESS)
        {
            assert(state_tracker_ != nullptr);

            auto thread_data = GetThreadData();
            assert(thread_data != nullptr);

            state_tracker_->AddEntry<ParentHandle, Wrapper, CreateInfo>(
                parent_handle, handle, create_info, thread_data->call_id_, thread_data->parameter_buffer_.get());
        }

        EndApiCallTrace(encoder);
    }

    // Pool allocation.
    template <typename ParentHandle, typename Wrapper, typename AllocateInfo>
    void EndPoolCreateApiCallTrace(VkResult                      result,
                                   ParentHandle                  parent_handle,
                                   uint32_t                      count,
                                   typename Wrapper::HandleType* handles,
                                   const AllocateInfo*           alloc_info,
                                   ParameterEncoder*             encoder)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS) && (handles != nullptr))
        {
            assert(state_tracker_ != nullptr);

            auto thread_data = GetThreadData();
            assert(thread_data != nullptr);

            state_tracker_->AddPoolEntry<ParentHandle, Wrapper, AllocateInfo>(
                parent_handle, count, handles, alloc_info, thread_data->call_id_, thread_data->parameter_buffer_.get());
        }

        EndApiCallTrace(encoder);
    }

    // Multiple object creation.
    template <typename ParentHandle, typename SecondaryHandle, typename Wrapper, typename CreateInfo>
    void EndGroupCreateApiCallTrace(VkResult                      result,
                                    ParentHandle                  parent_handle,
                                    SecondaryHandle               secondary_handle,
                                    uint32_t                      count,
                                    typename Wrapper::HandleType* handles,
                                    const CreateInfo*             create_infos,
                                    ParameterEncoder*             encoder)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && ((result == VK_SUCCESS) || (result == VK_INCOMPLETE)) &&
            (handles != nullptr))
        {
            assert(state_tracker_ != nullptr);

            auto thread_data = GetThreadData();
            assert(thread_data != nullptr);

            state_tracker_->AddGroupEntry<ParentHandle, SecondaryHandle, Wrapper, CreateInfo>(
                parent_handle,
                secondary_handle,
                count,
                handles,
                create_infos,
                thread_data->call_id_,
                thread_data->parameter_buffer_.get());
        }

        EndApiCallTrace(encoder);
    }

    // Multiple implicit object creation inside output struct.
    template <typename ParentHandle, typename Wrapper, typename HandleStruct>
    void EndStructGroupCreateApiCallTrace(VkResult                               result,
                                          ParentHandle                           parent_handle,
                                          uint32_t                               count,
                                          HandleStruct*                          handle_structs,
                                          std::function<Wrapper*(HandleStruct*)> unwrap_struct_handle,
                                          ParameterEncoder*                      encoder)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && ((result == VK_SUCCESS) || (result == VK_INCOMPLETE)) &&
            (handle_structs != nullptr))
        {
            assert(state_tracker_ != nullptr);

            auto thread_data = GetThreadData();
            assert(thread_data != nullptr);

            state_tracker_->AddStructGroupEntry(parent_handle,
                                                count,
                                                handle_structs,
                                                unwrap_struct_handle,
                                                thread_data->call_id_,
                                                thread_data->parameter_buffer_.get());
        }

        EndApiCallTrace(encoder);
    }

    // Single object destruction.
    template <typename Wrapper>
    void EndDestroyApiCallTrace(typename Wrapper::HandleType handle, ParameterEncoder* encoder)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->RemoveEntry<Wrapper>(handle);
        }

        EndApiCallTrace(encoder);
    }

    // Multiple object destruction.
    template <typename Wrapper>
    void EndDestroyApiCallTrace(uint32_t count, const typename Wrapper::HandleType* handles, ParameterEncoder* encoder)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (handles != nullptr))
        {
            assert(state_tracker_ != nullptr);

            for (uint32_t i = 0; i < count; ++i)
            {
                state_tracker_->RemoveEntry<Wrapper>(handles[i]);
            }
        }

        EndApiCallTrace(encoder);
    }

    void EndCommandApiCallTrace(VkCommandBuffer command_buffer, ParameterEncoder* encoder)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);

            auto thread_data = GetThreadData();
            assert(thread_data != nullptr);

            state_tracker_->TrackCommand(command_buffer, thread_data->call_id_, thread_data->parameter_buffer_.get());
        }

        EndApiCallTrace(encoder);
    }

    template <typename GetHandlesFunc, typename... GetHandlesArgs>
    void EndCommandApiCallTrace(VkCommandBuffer   command_buffer,
                                ParameterEncoder* encoder,
                                GetHandlesFunc    func,
                                GetHandlesArgs... args)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);

            auto thread_data = GetThreadData();
            assert(thread_data != nullptr);

            state_tracker_->TrackCommand(
                command_buffer, thread_data->call_id_, thread_data->parameter_buffer_.get(), func, args...);
        }

        EndApiCallTrace(encoder);
    }

    void EndApiCallTrace(ParameterEncoder* encoder);

    void EndMethodCallTrace(ParameterEncoder* encoder);

    void EndFrame();

    void CheckContinueCaptureForWriteMode();

    void CheckStartCaptureForTrackMode();

    bool IsTrimHotkeyPressed();

    void WriteDisplayMessageCmd(const char* message);

    bool GetDescriptorUpdateTemplateInfo(VkDescriptorUpdateTemplate update_template,
                                         const UpdateTemplateInfo** info) const;

    static VkResult OverrideCreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkInstance*                  pInstance);

    VkResult OverrideCreateDevice(VkPhysicalDevice             physicalDevice,
                                  const VkDeviceCreateInfo*    pCreateInfo,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkDevice*                    pDevice);

    VkResult OverrideCreateBuffer(VkDevice                     device,
                                  const VkBufferCreateInfo*    pCreateInfo,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkBuffer*                    pBuffer);

    VkResult OverrideAllocateMemory(VkDevice                     device,
                                    const VkMemoryAllocateInfo*  pAllocateInfo,
                                    const VkAllocationCallbacks* pAllocator,
                                    VkDeviceMemory*              pMemory);

    VkResult OverrideGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice                   physicalDevice,
                                                        uint32_t*                          pToolCount,
                                                        VkPhysicalDeviceToolPropertiesEXT* pToolProperties);

    void PostProcess_vkEnumeratePhysicalDevices(VkResult          result,
                                                VkInstance        instance,
                                                uint32_t*         pPhysicalDeviceCount,
                                                VkPhysicalDevice* pPhysicalDevices);

    void PostProcess_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice         physicalDevice,
                                                              uint32_t*                pQueueFamilyPropertyCount,
                                                              VkQueueFamilyProperties* pQueueFamilyProperties)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (pQueueFamilyPropertyCount != nullptr) &&
            (pQueueFamilyProperties != nullptr))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackPhysicalDeviceQueueFamilyProperties(
                physicalDevice, *pQueueFamilyPropertyCount, pQueueFamilyProperties);
        }
    }

    void PostProcess_vkGetPhysicalDeviceQueueFamilyProperties2(format::ApiCallId         call_id,
                                                               VkPhysicalDevice          physicalDevice,
                                                               uint32_t*                 pQueueFamilyPropertyCount,
                                                               VkQueueFamilyProperties2* pQueueFamilyProperties)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (pQueueFamilyPropertyCount != nullptr) &&
            (pQueueFamilyProperties != nullptr))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackPhysicalDeviceQueueFamilyProperties2(
                call_id, physicalDevice, *pQueueFamilyPropertyCount, pQueueFamilyProperties);
        }
    }

    void PostProcess_vkGetPhysicalDeviceSurfaceSupportKHR(VkResult         result,
                                                          VkPhysicalDevice physicalDevice,
                                                          uint32_t         queueFamilyIndex,
                                                          VkSurfaceKHR     surface,
                                                          VkBool32*        pSupported)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS) && (pSupported != nullptr))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackPhysicalDeviceSurfaceSupport(physicalDevice, queueFamilyIndex, surface, *pSupported);
        }
    }

    void PostProcess_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkResult                  result,
                                                               VkPhysicalDevice          physicalDevice,
                                                               VkSurfaceKHR              surface,
                                                               VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS) && (pSurfaceCapabilities != nullptr))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackPhysicalDeviceSurfaceCapabilities(physicalDevice, surface, *pSurfaceCapabilities);
        }
    }

    void PostProcess_vkGetPhysicalDeviceSurfaceFormatsKHR(VkResult            result,
                                                          VkPhysicalDevice    physicalDevice,
                                                          VkSurfaceKHR        surface,
                                                          uint32_t*           pSurfaceFormatCount,
                                                          VkSurfaceFormatKHR* pSurfaceFormats)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS) &&
            (pSurfaceFormatCount != nullptr) && (pSurfaceFormats != nullptr))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackPhysicalDeviceSurfaceFormats(
                physicalDevice, surface, *pSurfaceFormatCount, pSurfaceFormats);
        }
    }

    void PostProcess_vkGetPhysicalDeviceSurfacePresentModesKHR(VkResult          result,
                                                               VkPhysicalDevice  physicalDevice,
                                                               VkSurfaceKHR      surface,
                                                               uint32_t*         pPresentModeCount,
                                                               VkPresentModeKHR* pPresentModes)
    {
        if ((pPresentModeCount != nullptr) && (pPresentModes != nullptr))
        {
            if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS))
            {
                assert(state_tracker_ != nullptr);
                state_tracker_->TrackPhysicalDeviceSurfacePresentModes(
                    physicalDevice, surface, *pPresentModeCount, pPresentModes);
            }

#if defined(__ANDROID__)
            OverrideGetPhysicalDeviceSurfacePresentModesKHR(pPresentModeCount, pPresentModes);
#endif
        }
    }

    void PreProcess_vkCreateXlibSurfaceKHR(VkInstance                        instance,
                                           const VkXlibSurfaceCreateInfoKHR* pCreateInfo,
                                           const VkAllocationCallbacks*      pAllocator,
                                           VkSurfaceKHR*                     pSurface);

    void PreProcess_vkCreateXcbSurfaceKHR(VkInstance                       instance,
                                          const VkXcbSurfaceCreateInfoKHR* pCreateInfo,
                                          const VkAllocationCallbacks*     pAllocator,
                                          VkSurfaceKHR*                    pSurface);

    void PreProcess_vkCreateWaylandSurfaceKHR(VkInstance                           instance,
                                              const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                                              const VkAllocationCallbacks*         pAllocator,
                                              VkSurfaceKHR*                        pSurface);

    void PreProcess_vkCreateSwapchain(VkDevice                        device,
                                      const VkSwapchainCreateInfoKHR* pCreateInfo,
                                      const VkAllocationCallbacks*    pAllocator,
                                      VkSwapchainKHR*                 pSwapchain);

    void PostProcess_vkAcquireNextImageKHR(VkResult result,
                                           VkDevice,
                                           VkSwapchainKHR swapchain,
                                           uint64_t,
                                           VkSemaphore semaphore,
                                           VkFence     fence,
                                           uint32_t*   index)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR)))
        {
            assert((state_tracker_ != nullptr) && (index != nullptr));
            state_tracker_->TrackSemaphoreSignalState(semaphore);
            state_tracker_->TrackAcquireImage(*index, swapchain, semaphore, fence, 0);
        }
    }

    void PostProcess_vkAcquireNextImage2KHR(VkResult result,
                                            VkDevice,
                                            const VkAcquireNextImageInfoKHR* pAcquireInfo,
                                            uint32_t*                        index)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR)))
        {
            assert((state_tracker_ != nullptr) && (pAcquireInfo != nullptr) && (index != nullptr));
            state_tracker_->TrackSemaphoreSignalState(pAcquireInfo->semaphore);
            state_tracker_->TrackAcquireImage(*index,
                                              pAcquireInfo->swapchain,
                                              pAcquireInfo->semaphore,
                                              pAcquireInfo->fence,
                                              pAcquireInfo->deviceMask);
        }
    }

    void PostProcess_vkQueuePresentKHR(VkResult result, VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR)))
        {
            assert((state_tracker_ != nullptr) && (pPresentInfo != nullptr));
            state_tracker_->TrackSemaphoreSignalState(
                pPresentInfo->waitSemaphoreCount, pPresentInfo->pWaitSemaphores, 0, nullptr);
            state_tracker_->TrackPresentedImages(
                pPresentInfo->swapchainCount, pPresentInfo->pSwapchains, pPresentInfo->pImageIndices, queue);
        }

        EndFrame();
    }

    void PostProcess_vkQueueBindSparse(
        VkResult result, VkQueue, uint32_t bindInfoCount, const VkBindSparseInfo* pBindInfo, VkFence)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS))
        {
            assert((state_tracker_ != nullptr) && ((bindInfoCount == 0) || (pBindInfo != nullptr)));
            for (uint32_t i = 0; i < bindInfoCount; ++i)
            {
                state_tracker_->TrackSemaphoreSignalState(pBindInfo[i].waitSemaphoreCount,
                                                          pBindInfo[i].pWaitSemaphores,
                                                          pBindInfo[i].signalSemaphoreCount,
                                                          pBindInfo[i].pSignalSemaphores);
            }
        }
    }

    void PostProcess_vkGetBufferMemoryRequirements(VkDevice              device,
                                                   VkBuffer              buffer,
                                                   VkMemoryRequirements* pMemoryRequirements)
    {
        GFXRECON_UNREFERENCED_PARAMETER(device);
        GFXRECON_UNREFERENCED_PARAMETER(buffer);

        if ((memory_tracking_mode_ == CaptureSettings::MemoryTrackingMode::kPageGuard) &&
            page_guard_align_buffer_sizes_ && (pMemoryRequirements != nullptr))
        {
            util::PageGuardManager* manager = util::PageGuardManager::Get();
            assert(manager != nullptr);

            GFXRECON_CHECK_CONVERSION_DATA_LOSS(size_t, pMemoryRequirements->size);
            GFXRECON_CHECK_CONVERSION_DATA_LOSS(size_t, pMemoryRequirements->alignment);

            pMemoryRequirements->size = manager->GetAlignedSize(static_cast<size_t>(pMemoryRequirements->size));
            pMemoryRequirements->alignment =
                manager->GetAlignedSize(static_cast<size_t>(pMemoryRequirements->alignment));
        }
    }

    void PostProcess_vkGetBufferMemoryRequirements2(VkDevice                               device,
                                                    const VkBufferMemoryRequirementsInfo2* pInfo,
                                                    VkMemoryRequirements2*                 pMemoryRequirements)
    {
        GFXRECON_UNREFERENCED_PARAMETER(device);
        GFXRECON_UNREFERENCED_PARAMETER(pInfo);

        if ((memory_tracking_mode_ == CaptureSettings::MemoryTrackingMode::kPageGuard) &&
            page_guard_align_buffer_sizes_ && (pMemoryRequirements != nullptr))
        {
            util::PageGuardManager* manager = util::PageGuardManager::Get();
            assert(manager != nullptr);

            GFXRECON_CHECK_CONVERSION_DATA_LOSS(size_t, pMemoryRequirements->memoryRequirements.size);
            GFXRECON_CHECK_CONVERSION_DATA_LOSS(size_t, pMemoryRequirements->memoryRequirements.alignment);

            pMemoryRequirements->memoryRequirements.size =
                manager->GetAlignedSize(static_cast<size_t>(pMemoryRequirements->memoryRequirements.size));
            pMemoryRequirements->memoryRequirements.alignment =
                manager->GetAlignedSize(static_cast<size_t>(pMemoryRequirements->memoryRequirements.alignment));
        }
    }

    void PostProcess_vkBindBufferMemory(
        VkResult result, VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackBufferMemoryBinding(device, buffer, memory, memoryOffset);
        }
    }

    void PostProcess_vkBindBufferMemory2(VkResult                      result,
                                         VkDevice                      device,
                                         uint32_t                      bindInfoCount,
                                         const VkBindBufferMemoryInfo* pBindInfos)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS) && (pBindInfos != nullptr))
        {
            assert(state_tracker_ != nullptr);

            for (uint32_t i = 0; i < bindInfoCount; ++i)
            {
                state_tracker_->TrackBufferMemoryBinding(
                    device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            }
        }
    }

    void PostProcess_vkBindImageMemory(
        VkResult result, VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackImageMemoryBinding(device, image, memory, memoryOffset);
        }
    }

    void PostProcess_vkBindImageMemory2(VkResult                     result,
                                        VkDevice                     device,
                                        uint32_t                     bindInfoCount,
                                        const VkBindImageMemoryInfo* pBindInfos)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS) && (pBindInfos != nullptr))
        {
            assert(state_tracker_ != nullptr);

            for (uint32_t i = 0; i < bindInfoCount; ++i)
            {
                state_tracker_->TrackImageMemoryBinding(
                    device, pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            }
        }
    }

    void PostProcess_vkCmdBeginRenderPass(VkCommandBuffer              commandBuffer,
                                          const VkRenderPassBeginInfo* pRenderPassBegin,
                                          VkSubpassContents)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackBeginRenderPass(commandBuffer, pRenderPassBegin);
        }
    }

    void PostProcess_vkCmdBeginRenderPass2(VkCommandBuffer              commandBuffer,
                                           const VkRenderPassBeginInfo* pRenderPassBegin,
                                           const VkSubpassBeginInfoKHR*)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackBeginRenderPass(commandBuffer, pRenderPassBegin);
        }
    }

    void PostProcess_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackEndRenderPass(commandBuffer);
        }
    }

    void PostProcess_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfoKHR*)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackEndRenderPass(commandBuffer);
        }
    }

    void PostProcess_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer,
                                          VkPipelineStageFlags,
                                          VkPipelineStageFlags,
                                          VkDependencyFlags,
                                          uint32_t,
                                          const VkMemoryBarrier*,
                                          uint32_t,
                                          const VkBufferMemoryBarrier*,
                                          uint32_t                    imageMemoryBarrierCount,
                                          const VkImageMemoryBarrier* pImageMemoryBarriers)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackImageBarriers(commandBuffer, imageMemoryBarrierCount, pImageMemoryBarriers);
        }
    }

    void PostProcess_vkCmdExecuteCommands(VkCommandBuffer        commandBuffer,
                                          uint32_t               commandBufferCount,
                                          const VkCommandBuffer* pCommandBuffers)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
        }
    }

    void PostProcess_vkResetCommandPool(VkResult result, VkDevice, VkCommandPool commandPool, VkCommandPoolResetFlags)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS))
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackResetCommandPool(commandPool);
        }
    }

    void
    PostProcess_vkQueueSubmit(VkResult result, VkQueue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence)
    {
        if (((capture_mode_ & kModeTrack) == kModeTrack) && (result == VK_SUCCESS))
        {
            assert((state_tracker_ != nullptr) && ((submitCount == 0) || (pSubmits != nullptr)));

            state_tracker_->TrackCommandBufferSubmissions(submitCount, pSubmits);

            for (uint32_t i = 0; i < submitCount; ++i)
            {
                state_tracker_->TrackSemaphoreSignalState(pSubmits[i].waitSemaphoreCount,
                                                          pSubmits[i].pWaitSemaphores,
                                                          pSubmits[i].signalSemaphoreCount,
                                                          pSubmits[i].pSignalSemaphores);
            }
        }
    }

    void PostProcess_vkUpdateDescriptorSets(VkDevice,
                                            uint32_t                    descriptorWriteCount,
                                            const VkWriteDescriptorSet* pDescriptorWrites,
                                            uint32_t                    descriptorCopyCount,
                                            const VkCopyDescriptorSet*  pDescriptorCopies)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackUpdateDescriptorSets(
                descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
        }
    }

    void PostProcess_vkUpdateDescriptorSetWithTemplate(VkDevice,
                                                       VkDescriptorSet            descriptorSet,
                                                       VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                       const void*                pData)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            TrackUpdateDescriptorSetWithTemplate(descriptorSet, descriptorUpdateTemplate, pData);
        }
    }

    void PostProcess_vkUpdateDescriptorSetWithTemplateKHR(VkDevice,
                                                          VkDescriptorSet            descriptorSet,
                                                          VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                          const void*                pData)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            TrackUpdateDescriptorSetWithTemplate(descriptorSet, descriptorUpdateTemplate, pData);
        }
    }

    void PostProcess_vkCmdPushDescriptorSetKHR(VkCommandBuffer,
                                               VkPipelineBindPoint,
                                               VkPipelineLayout            layout,
                                               uint32_t                    set,
                                               uint32_t                    descriptorWriteCount,
                                               const VkWriteDescriptorSet* pDescriptorWrites)
    {
        GFXRECON_UNREFERENCED_PARAMETER(layout);
        GFXRECON_UNREFERENCED_PARAMETER(set);
        GFXRECON_UNREFERENCED_PARAMETER(descriptorWriteCount);
        GFXRECON_UNREFERENCED_PARAMETER(pDescriptorWrites);
        // TODO: Need to be able to map layout + set to a VkDescriptorSet handle.
    }

    void PostProcess_vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer,
                                                           VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                           VkPipelineLayout           layout,
                                                           uint32_t                   set,
                                                           const void*                pData)
    {
        GFXRECON_UNREFERENCED_PARAMETER(descriptorUpdateTemplate);
        GFXRECON_UNREFERENCED_PARAMETER(layout);
        GFXRECON_UNREFERENCED_PARAMETER(set);
        GFXRECON_UNREFERENCED_PARAMETER(pData);
        // TODO: Need to be able to map layout + set to a VkDescriptorSet handle.
    }

    void PostProcess_vkResetDescriptorPool(VkResult result,
                                           VkDevice,
                                           VkDescriptorPool descriptorPool,
                                           VkDescriptorPoolResetFlags)
    {
        if (result == VK_SUCCESS)
        {
            if ((capture_mode_ & kModeTrack) == kModeTrack)
            {
                assert(state_tracker_ != nullptr);
                state_tracker_->TrackResetDescriptorPool(descriptorPool);
            }

            ResetDescriptorPoolWrapper(descriptorPool);
        }
    }

    void PostProcess_vkCmdBeginQuery(VkCommandBuffer     commandBuffer,
                                     VkQueryPool         queryPool,
                                     uint32_t            query,
                                     VkQueryControlFlags flags)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackQueryActivation(commandBuffer, queryPool, query, flags, QueryInfo::kInvalidIndex);
        }
    }

    void PostProcess_vkCmdBeginQueryIndexedEXT(
        VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags, uint32_t index)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackQueryActivation(commandBuffer, queryPool, query, flags, index);
        }
    }

    void PostProcess_vkCmdWriteTimestamp(VkCommandBuffer         commandBuffer,
                                         VkPipelineStageFlagBits pipelineStage,
                                         VkQueryPool             queryPool,
                                         uint32_t                query)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackQueryActivation(commandBuffer, queryPool, query, 0, QueryInfo::kInvalidIndex);
        }
    }

    void
    PostProcess_vkCmdWriteAccelerationStructuresPropertiesNV(VkCommandBuffer commandBuffer,
                                                             uint32_t        accelerationStructureCount,
                                                             const VkAccelerationStructureNV* pAccelerationStructures,
                                                             VkQueryType                      queryType,
                                                             VkQueryPool                      queryPool,
                                                             uint32_t                         firstQuery)
    {
        GFXRECON_UNREFERENCED_PARAMETER(commandBuffer);
        GFXRECON_UNREFERENCED_PARAMETER(accelerationStructureCount);
        GFXRECON_UNREFERENCED_PARAMETER(pAccelerationStructures);
        GFXRECON_UNREFERENCED_PARAMETER(queryType);
        GFXRECON_UNREFERENCED_PARAMETER(queryPool);
        GFXRECON_UNREFERENCED_PARAMETER(firstQuery);
        // TODO
    }

    void PostProcess_vkCmdResetQueryPool(VkCommandBuffer commandBuffer,
                                         VkQueryPool     queryPool,
                                         uint32_t        firstQuery,
                                         uint32_t        queryCount)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackQueryReset(commandBuffer, queryPool, firstQuery, queryCount);
        }
    }

    void PostProcess_vkResetQueryPool(VkDevice, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
    {
        if ((capture_mode_ & kModeTrack) == kModeTrack)
        {
            assert(state_tracker_ != nullptr);
            state_tracker_->TrackQueryReset(queryPool, firstQuery, queryCount);
        }
    }

    void PostProcess_vkMapMemory(VkResult         result,
                                 VkDevice         device,
                                 VkDeviceMemory   memory,
                                 VkDeviceSize     offset,
                                 VkDeviceSize     size,
                                 VkMemoryMapFlags flags,
                                 void**           ppData);

    void PreProcess_vkFlushMappedMemoryRanges(VkDevice                   device,
                                              uint32_t                   memoryRangeCount,
                                              const VkMappedMemoryRange* pMemoryRanges);

    void PreProcess_vkUnmapMemory(VkDevice device, VkDeviceMemory memory);

    void PreProcess_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator);

    void PostProcess_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator);

    void PreProcess_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);

    void PreProcess_vkCreateDescriptorUpdateTemplate(VkResult                                    result,
                                                     VkDevice                                    device,
                                                     const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks*                pAllocator,
                                                     VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);

    void PreProcess_vkCreateDescriptorUpdateTemplateKHR(VkResult                                    result,
                                                        VkDevice                                    device,
                                                        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks*                pAllocator,
                                                        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);

// TODO (GH #9): Split TraceManager into separate Vulkan and D3D12 class implementations, with a common base class.
#if defined(WIN32)
    void PostProcess_ID3D12Device_CreateCommittedResource(ID3D12Device_Wrapper*        wrapper,
                                                          HRESULT                      result,
                                                          const D3D12_HEAP_PROPERTIES* heap_properties,
                                                          D3D12_HEAP_FLAGS             heap_flags,
                                                          const D3D12_RESOURCE_DESC*   desc,
                                                          D3D12_RESOURCE_STATES        initial_resource_state,
                                                          const D3D12_CLEAR_VALUE*     optimized_clear_value,
                                                          REFIID                       riid,
                                                          void**                       resource);

    void PostProcess_ID3D12Device_CreatePlacedResource(ID3D12Device_Wrapper*      wrapper,
                                                       HRESULT                    result,
                                                       ID3D12Heap*                heap,
                                                       UINT64                     heap_offset,
                                                       const D3D12_RESOURCE_DESC* desc,
                                                       D3D12_RESOURCE_STATES      initial_state,
                                                       const D3D12_CLEAR_VALUE*   optimized_clear_value,
                                                       REFIID                     riid,
                                                       void**                     resource);

    void PostProcess_ID3D12Resource_Map(
        ID3D12Resource_Wrapper* wrapper, HRESULT result, UINT subresource, const D3D12_RANGE* read_range, void** data);

    void PreProcess_ID3D12Resource_Unmap(ID3D12Resource_Wrapper* wrapper,
                                         UINT                    subresource,
                                         const D3D12_RANGE*      written_range);

    void Destroy_ID3D12Resource(ID3D12Resource_Wrapper* wrapper);

    void PreProcess_ID3D12CommandQueue_ExecuteCommandLists(ID3D12CommandQueue_Wrapper* wrapper,
                                                           UINT                        num_lists,
                                                           ID3D12CommandList* const*   lists);
#endif

#if defined(__ANDROID__)
    void OverrideGetPhysicalDeviceSurfacePresentModesKHR(uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes);
#endif

  protected:
    VulkanCaptureManager();

    ~VulkanCaptureManager();

    bool Initialize(std::string base_filename, const CaptureSettings::TraceSettings& trace_settings);

  private:
    enum CaptureModeFlags : uint32_t
    {
        kModeDisabled      = 0x0,
        kModeWrite         = 0x01,
        kModeTrack         = 0x02,
        kModeWriteAndTrack = (kModeWrite | kModeTrack)
    };

    enum PageGuardMemoryMode : uint32_t
    {
        kMemoryModeDisabled,
        kMemoryModeShadowInternal,   // Internally managed shadow memory allocations.
        kMemoryModeShadowPersistent, // Externally managed shadow memory allocations.
        kMemoryModeExternal          // Imported host memory without shadow allocations.
    };

    typedef uint32_t CaptureMode;

    class ThreadData
    {
      public:
        ThreadData();

        ~ThreadData() {}

      public:
        const format::ThreadId                    thread_id_;
        format::ApiCallId                         call_id_;
        format::HandleId                          object_id_;
        std::unique_ptr<util::MemoryOutputStream> parameter_buffer_;
        std::unique_ptr<ParameterEncoder>         parameter_encoder_;
        std::vector<uint8_t>                      compressed_buffer_;
        HandleUnwrapMemory                        handle_unwrap_memory_;

      private:
        static format::ThreadId GetThreadId();

      private:
        static std::mutex                                     count_lock_;
        static format::ThreadId                               thread_count_;
        static std::unordered_map<uint64_t, format::ThreadId> id_map_;
    };

    struct HardwareBufferInfo
    {
        format::HandleId      memory_id;
        std::atomic<uint32_t> reference_count;
    };

    typedef std::unordered_map<AHardwareBuffer*, HardwareBufferInfo> HardwareBufferMap;

  private:
    ThreadData* GetThreadData()
    {
        if (!thread_data_)
        {
            thread_data_ = std::make_unique<ThreadData>();
        }
        return thread_data_.get();
    }

    std::string CreateTrimFilename(const std::string& base_filename, const CaptureSettings::TrimRange& trim_range);
    bool        CreateCaptureFile(const std::string& base_filename);
    void        ActivateTrimming();

    void WriteFileHeader();
    void BuildOptionList(const format::EnabledOptions&        enabled_options,
                         std::vector<format::FileOptionPair>* option_list);

    ParameterEncoder* InitApiCallTrace(format::ApiCallId call_id);

    ParameterEncoder* InitMethodCallTrace(format::ApiCallId call_id, format::HandleId object_id);

    void WriteResizeWindowCmd(format::HandleId surface_id, uint32_t width, uint32_t height);
    void WriteResizeWindowCmd2(format::HandleId              surface_id,
                               uint32_t                      width,
                               uint32_t                      height,
                               VkSurfaceTransformFlagBitsKHR pre_transform);
    void WriteFillMemoryCmd(format::HandleId memory_id, VkDeviceSize offset, VkDeviceSize size, const void* data);
    void WriteCreateHardwareBufferCmd(format::HandleId                                    memory_id,
                                      AHardwareBuffer*                                    buffer,
                                      const std::vector<format::HardwareBufferPlaneInfo>& plane_info);
    void WriteDestroyHardwareBufferCmd(AHardwareBuffer* buffer);
    void WriteSetDevicePropertiesCommand(format::HandleId                  physical_device_id,
                                         const VkPhysicalDeviceProperties& properties);
    void WriteSetDeviceMemoryPropertiesCommand(format::HandleId                        physical_device_id,
                                               const VkPhysicalDeviceMemoryProperties& memory_properties);
    void WriteSetBufferAddressCommand(format::HandleId device_id, format::HandleId buffer_id, uint64_t address);

    void SetDescriptorUpdateTemplateInfo(VkDescriptorUpdateTemplate                  update_template,
                                         const VkDescriptorUpdateTemplateCreateInfo* create_info);

    void TrackUpdateDescriptorSetWithTemplate(VkDescriptorSet            set,
                                              VkDescriptorUpdateTemplate update_templat,
                                              const void*                data);

    VkMemoryPropertyFlags GetMemoryProperties(DeviceWrapper* device_wrapper, uint32_t memory_type_index);

    const VkImportAndroidHardwareBufferInfoANDROID*
    FindAllocateMemoryExtensions(const VkMemoryAllocateInfo* allocate_info);

    void ProcessImportAndroidHardwareBuffer(VkDevice device, VkDeviceMemory memory, AHardwareBuffer* hardware_buffer);
    void ReleaseAndroidHardwareBuffer(AHardwareBuffer* hardware_buffer);

// TODO (GH #9): Split TraceManager into separate Vulkan and D3D12 class implementations, with a common base class.
#if defined(WIN32)
    void InitializeID3D12ResourceInfo(ID3D12Device_Wrapper*      device_wrapper,
                                      ID3D12Resource_Wrapper*    resource_wrapper,
                                      const D3D12_RESOURCE_DESC* desc);
#endif

  private:
    static VulkanCaptureManager*                    instance_;
    static uint32_t                                 instance_count_;
    static std::mutex                               instance_lock_;
    static thread_local std::unique_ptr<ThreadData> thread_data_;
    static LayerTable                               layer_table_;
    static std::atomic<format::HandleId>            unique_id_counter_;
    format::EnabledOptions                          file_options_;
    std::unique_ptr<util::FileOutputStream>         file_stream_;
    std::string                                     base_filename_;
    std::mutex                                      file_lock_;
    bool                                            timestamp_filename_;
    bool                                            force_file_flush_;
    std::unique_ptr<util::Compressor>               compressor_;
    CaptureSettings::MemoryTrackingMode             memory_tracking_mode_;
    bool                                            page_guard_align_buffer_sizes_;
    bool                                            page_guard_track_ahb_memory_;
    PageGuardMemoryMode                             page_guard_memory_mode_;
    std::mutex                                      mapped_memory_lock_;
    std::set<DeviceMemoryWrapper*>                  mapped_memory_; // Track mapped memory for unassisted tracking mode.
    bool                                            trim_enabled_;
    std::vector<CaptureSettings::TrimRange>         trim_ranges_;
    std::string                                     trim_key_;
    size_t                                          trim_current_range_;
    uint32_t                                        current_frame_;
    std::unique_ptr<VulkanStateTracker>             state_tracker_;
    CaptureMode                                     capture_mode_;
    HardwareBufferMap                               hardware_buffers_;
    util::Keyboard                                  keyboard_;
    bool                                            previous_hotkey_state_;

// TODO (GH #9): Split CaptureManager into separate Vulkan and D3D12 class implementations, with a common base class.
#if defined(WIN32)
    std::set<ID3D12Resource_Wrapper*> mapped_resources_; ///< Track mapped resources for unassisted tracking mode.
    DxgiDispatchTable  dxgi_dispatch_table_;  ///< DXGI dispatch table for functions retrieved from the DXGI DLL.
    D3D12DispatchTable d3d12_dispatch_table_; ///< D3D12 dispatch table for functions retrieved from the D3D12 DLL.
    static thread_local uint32_t call_scope_; ///< Per-thread scope count to determine whan an intercepted API call is
                                              ///< being made directly by the application.
#endif
};

GFXRECON_END_NAMESPACE(encode)
GFXRECON_END_NAMESPACE(gfxrecon)

#endif // GFXRECON_ENCODE_VULKAN_CAPTURE_MANAGER_H