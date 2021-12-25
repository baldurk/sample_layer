#include "vulkan.h"
#include "vk_layer.h"

#include <assert.h>
#include <string.h>

#include <mutex>
#include <map>

#undef VK_LAYER_EXPORT
#if defined(WIN32)
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C"
#endif

// single global lock, for simplicity
std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;

// use the loader's dispatch table pointer as a key for dispatch map lookups
template<typename DispatchableType>
void *GetKey(DispatchableType inst)
{
  return *(void **)inst;
}

// layer book-keeping information, to store dispatch tables by key
std::map<void *, VkLayerInstanceDispatchTable> instance_dispatch;
std::map<void *, VkLayerDispatchTable> device_dispatch;

// actual data we're recording in this layer
struct CommandStats
{
  uint32_t drawCount = 0, instanceCount = 0, vertCount = 0;
};

std::map<VkCommandBuffer, CommandStats> commandbuffer_stats;

///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

  VkResult ret = createFunc(pCreateInfo, pAllocator, pInstance);
  if (ret != VK_SUCCESS) return ret;

  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerInstanceDispatchTable dispatchTable;
  dispatchTable.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gpa(*pInstance, "vkGetInstanceProcAddr");
  dispatchTable.DestroyInstance = (PFN_vkDestroyInstance)gpa(*pInstance, "vkDestroyInstance");
  dispatchTable.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gpa(*pInstance, "vkEnumerateDeviceExtensionProperties");

  // store the table by key
  {
    scoped_lock l(global_lock);
    instance_dispatch[GetKey(*pInstance)] = dispatchTable;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL SampleLayer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  instance_dispatch.erase(GetKey(instance));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  
  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
  if (ret != VK_SUCCESS) return ret;
  
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerDispatchTable dispatchTable;
  dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
  dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
  dispatchTable.BeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(*pDevice, "vkBeginCommandBuffer");
  dispatchTable.CmdDraw = (PFN_vkCmdDraw)gdpa(*pDevice, "vkCmdDraw");
  dispatchTable.CmdDrawIndexed = (PFN_vkCmdDrawIndexed)gdpa(*pDevice, "vkCmdDrawIndexed");
  dispatchTable.EndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(*pDevice, "vkEndCommandBuffer");
  
  // store the table by key
  {
    scoped_lock l(global_lock);
    device_dispatch[GetKey(*pDevice)] = dispatchTable;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL SampleLayer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  device_dispatch.erase(GetKey(device));
}

///////////////////////////////////////////////////////////////////////////////////////////
// Actual layer implementation

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo)
{
  scoped_lock l(global_lock);
  commandbuffer_stats[commandBuffer] = CommandStats();
  return device_dispatch[GetKey(commandBuffer)].BeginCommandBuffer(commandBuffer, pBeginInfo);
}

VK_LAYER_EXPORT void VKAPI_CALL SampleLayer_CmdDraw(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    vertexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstVertex,
    uint32_t                                    firstInstance)
{
  scoped_lock l(global_lock);

  commandbuffer_stats[commandBuffer].drawCount++;
  commandbuffer_stats[commandBuffer].instanceCount += instanceCount;
  commandbuffer_stats[commandBuffer].vertCount += instanceCount*vertexCount;

  device_dispatch[GetKey(commandBuffer)].CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

VK_LAYER_EXPORT void VKAPI_CALL SampleLayer_CmdDrawIndexed(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    indexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstIndex,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance)
{
  scoped_lock l(global_lock);

  commandbuffer_stats[commandBuffer].drawCount++;
  commandbuffer_stats[commandBuffer].instanceCount += instanceCount;
  commandbuffer_stats[commandBuffer].vertCount += instanceCount*indexCount;

  device_dispatch[GetKey(commandBuffer)].CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
  scoped_lock l(global_lock);

  CommandStats &s = commandbuffer_stats[commandBuffer];
  printf("Command buffer %p ended with %u draws, %u instances and %u vertices", commandBuffer, s.drawCount, s.instanceCount, s.vertCount);

  return device_dispatch[GetKey(commandBuffer)].EndCommandBuffer(commandBuffer);
}

///////////////////////////////////////////////////////////////////////////////////////////
// Enumeration function

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                                       VkLayerProperties *pProperties)
{
  if(pPropertyCount) *pPropertyCount = 1;

  if(pProperties)
  {
    strcpy(pProperties->layerName, "VK_LAYER_SAMPLE_SampleLayer");
    strcpy(pProperties->description, "Sample layer - https://renderdoc.org/vulkan-layer-guide.html");
    pProperties->implementationVersion = 1;
    pProperties->specVersion = VK_API_VERSION_1_0;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_EnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
  return SampleLayer_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_EnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_SAMPLE_SampleLayer"))
    return VK_ERROR_LAYER_NOT_PRESENT;

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL SampleLayer_EnumerateDeviceExtensionProperties(
                                     VkPhysicalDevice physicalDevice, const char *pLayerName,
                                     uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  // pass through any queries that aren't to us
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_SAMPLE_SampleLayer"))
  {
    if(physicalDevice == VK_NULL_HANDLE)
      return VK_SUCCESS;

    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions, entry points of the layer

#define GETPROCADDR(func) if(!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&SampleLayer_##func;

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL SampleLayer_GetDeviceProcAddr(VkDevice device, const char *pName)
{
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(BeginCommandBuffer);
  GETPROCADDR(CmdDraw);
  GETPROCADDR(CmdDrawIndexed);
  GETPROCADDR(EndCommandBuffer);
  
  {
    scoped_lock l(global_lock);
    return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
  }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL SampleLayer_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
  // instance chain functions we intercept
  GETPROCADDR(GetInstanceProcAddr);
  GETPROCADDR(EnumerateInstanceLayerProperties);
  GETPROCADDR(EnumerateInstanceExtensionProperties);
  GETPROCADDR(CreateInstance);
  GETPROCADDR(DestroyInstance);
  
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(BeginCommandBuffer);
  GETPROCADDR(CmdDraw);
  GETPROCADDR(CmdDrawIndexed);
  GETPROCADDR(EndCommandBuffer);

  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}

