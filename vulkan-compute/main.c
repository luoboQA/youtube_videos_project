#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <vulkan/vulkan.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t b32;
typedef float f32;

#define ALIGN_UP(n, a) (((n) + (a) - 1) - ((n) + (a) - 1) % (a))

// 辅助函数：检查Vulkan调用结果
static void check_vk_result(VkResult result, const char* msg) {
    if (result != VK_SUCCESS) {
        fprintf(stderr, "ERROR: %s (VkResult: %d)\n", msg, result);
        exit(1);
    }
}

int main(void) {
    const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";
    b32 use_validation_layer = false;

    VkInstance instance = NULL;
    {
        VkInstanceCreateInfo instance_create_info = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &(VkApplicationInfo){
                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .apiVersion = VK_API_VERSION_1_0
            }
        };

        #ifndef NDEBUG
        u32 num_layers = 0;
        vkEnumerateInstanceLayerProperties(&num_layers, NULL);

        VkLayerProperties* layer_props = (VkLayerProperties*)malloc(sizeof(VkLayerProperties) * num_layers);
        if (layer_props) {
            vkEnumerateInstanceLayerProperties(&num_layers, layer_props);

            for (u32 i = 0; i < num_layers; i++) {
                if (strcmp(layer_props[i].layerName, validation_layer_name) == 0) {
                    use_validation_layer = true;
                    break;
                }
            }

            free(layer_props);  // 修复内存泄漏
        }

        if (use_validation_layer) {
            instance_create_info.enabledLayerCount = 1;
            instance_create_info.ppEnabledLayerNames = &validation_layer_name;
        }
        #endif

        VkResult result = vkCreateInstance(&instance_create_info, NULL, &instance);
        check_vk_result(result, "Failed to create Vulkan instance");
    }

    VkPhysicalDevice physical_device = NULL;
    {
        u32 num_physical_devices = 0;
        vkEnumeratePhysicalDevices(instance, &num_physical_devices, NULL);
        
        if (num_physical_devices == 0) {
            fprintf(stderr, "ERROR: No Vulkan-capable devices found\n");
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        // 获取所有设备，优先选择独立GPU
        VkPhysicalDevice* devices = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * num_physical_devices);
        vkEnumeratePhysicalDevices(instance, &num_physical_devices, devices);

        // 优先选择独立GPU
        for (u32 i = 0; i < num_physical_devices; i++) {
            VkPhysicalDeviceProperties props = {0};
            vkGetPhysicalDeviceProperties(devices[i], &props);
            
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical_device = devices[i];
                printf("Selected discrete GPU: %s\n", props.deviceName);
                break;
            }
        }

        // 如果没有独立GPU，使用第一个可用设备
        if (!physical_device) {
            physical_device = devices[0];
            VkPhysicalDeviceProperties props = {0};
            vkGetPhysicalDeviceProperties(physical_device, &props);
            printf("Using physical device: %s\n", props.deviceName);
        }

        free(devices);
    }

    u32 queue_family_index = 0;
    VkDevice device = NULL;
    {
        u32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);

        VkQueueFamilyProperties* queue_props = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_props);

        b32 found = false;
        for (u32 i = 0; i < queue_family_count; i++) {
            if (queue_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queue_family_index = i;
                found = true;
                break;
            }
        }

        free(queue_props);

        if (!found) {
            fprintf(stderr, "ERROR: No compute queue family found\n");
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        f32 priority = 1.0f;

        VkDeviceCreateInfo device_create_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = queue_family_index,
                .queueCount = 1,
                .pQueuePriorities = &priority,
            },
        };

        #ifndef NDEBUG
        if (use_validation_layer) {
            device_create_info.enabledLayerCount = 1;
            device_create_info.ppEnabledLayerNames = &validation_layer_name;
        }
        #endif

        VkResult result = vkCreateDevice(physical_device, &device_create_info, NULL, &device);
        check_vk_result(result, "Failed to create Vulkan device");
    }

    u32 vector_size = 16;
    u32 buffer_size = sizeof(f32) * vector_size;

    VkBuffer in_buffer = NULL, out_buffer = NULL;
    {
        VkBufferCreateInfo buffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &queue_family_index
        };

        VkResult result = vkCreateBuffer(device, &buffer_create_info, NULL, &in_buffer);
        check_vk_result(result, "Failed to create input buffer");
        
        result = vkCreateBuffer(device, &buffer_create_info, NULL, &out_buffer);
        check_vk_result(result, "Failed to create output buffer");
    }

    VkMemoryRequirements in_mem_reqs = { 0 }, out_mem_reqs = { 0 };
    vkGetBufferMemoryRequirements(device, in_buffer, &in_mem_reqs);
    vkGetBufferMemoryRequirements(device, out_buffer, &out_mem_reqs);

    // 计算内存布局（使用对齐）
    u64 out_buffer_offset = ALIGN_UP(in_mem_reqs.size, out_mem_reqs.alignment);
    u64 total_memory_size = out_buffer_offset + out_mem_reqs.size;

    VkPhysicalDeviceMemoryProperties mem_props = { 0 };
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    u32 mem_type_index = UINT32_MAX;
    for (u32 i = 0; i < mem_props.memoryTypeCount; i++) {
        if (
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && 
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        ) {
            mem_type_index = i;
            break;
        }
    }

    if (mem_type_index == UINT32_MAX) {
        fprintf(stderr, "ERROR: No suitable memory type found\n");
        vkDestroyBuffer(device, out_buffer, NULL);
        vkDestroyBuffer(device, in_buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    VkDeviceMemory memory = NULL;
    {
        VkResult result = vkAllocateMemory(
            device, &(VkMemoryAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .memoryTypeIndex = mem_type_index,
                .allocationSize = total_memory_size
            }, NULL, &memory
        );
        check_vk_result(result, "Failed to allocate device memory");
    }

    // 初始化数据
    {
        f32* data = NULL;
        vkMapMemory(device, memory, 0, total_memory_size, 0, (void**)(&data));

        f32* in_data = data;
        f32* out_data = data + (out_buffer_offset / sizeof(f32));

        for (u32 i = 0; i < vector_size; i++) {
            in_data[i] = (f32)i;
            out_data[i] = (f32)(vector_size - i - 1);
        }

        vkUnmapMemory(device, memory);
    }

    vkBindBufferMemory(device, in_buffer, memory, 0);
    vkBindBufferMemory(device, out_buffer, memory, out_buffer_offset);

    printf("Initial Memory\n");
    {
        f32* data = NULL;
        vkMapMemory(device, memory, 0, total_memory_size, 0, (void**)(&data));

        f32* in_data = data;
        f32* out_data = data + (out_buffer_offset / sizeof(f32));

        printf("In Data : [ "); 
        for (u32 i = 0; i < vector_size; i++) {
            printf("%2.0f ", in_data[i]);
        }
        printf("]\n");

        printf("Out Data: [ "); 
        for (u32 i = 0; i < vector_size; i++) {
            printf("%2.0f ", out_data[i]);
        }
        printf("]\n");

        vkUnmapMemory(device, memory);
    }

    // 加载着色器
    VkShaderModule shader_module = NULL;
    {
        const char* shader_path = "add.spv";
        FILE* f = fopen(shader_path, "rb");
        
        if (!f) {
            fprintf(stderr, "ERROR: Failed to open shader file '%s'\n", shader_path);
            fprintf(stderr, "Compile with: glslangValidator -V add.comp.glsl -o add.spv\n");
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, out_buffer, NULL);
            vkDestroyBuffer(device, in_buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        fseek(f, 0, SEEK_END);
        u64 size = ftell(f);
        fseek(f, 0, SEEK_SET);

        u8* shader_file = (u8*)malloc(size);
        if (!shader_file) {
            fprintf(stderr, "ERROR: Failed to allocate memory for shader\n");
            fclose(f);
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, out_buffer, NULL);
            vkDestroyBuffer(device, in_buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        size_t read_size = fread(shader_file, 1, size, f);
        fclose(f);

        if (read_size != size) {
            fprintf(stderr, "ERROR: Failed to read shader file (read %zu, expected %llu)\n", 
                    read_size, (unsigned long long)size);
            free(shader_file);
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, out_buffer, NULL);
            vkDestroyBuffer(device, in_buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        VkResult result = vkCreateShaderModule(
            device, &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = size,
                .pCode = (u32*)shader_file,
            }, NULL, &shader_module
        );
        
        free(shader_file);
        check_vk_result(result, "Failed to create shader module");
    }

    // 创建描述符集布局
    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };

    VkDescriptorSetLayout descriptor_set_layout = NULL;
    {
        VkResult result = vkCreateDescriptorSetLayout(
            device, &(VkDescriptorSetLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
                .pBindings = bindings,
            }, NULL, &descriptor_set_layout
        );
        check_vk_result(result, "Failed to create descriptor set layout");
    }

    // 创建描述符池
    VkDescriptorPool descriptor_pool = NULL;
    {
        VkResult result = vkCreateDescriptorPool(
            device, &(VkDescriptorPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &(VkDescriptorPoolSize){
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = sizeof(bindings) / sizeof(bindings[0])
                }
            }, NULL, &descriptor_pool
        );
        check_vk_result(result, "Failed to create descriptor pool");
    }

    // 分配描述符集
    VkDescriptorSet descriptor_set = NULL;
    {
        VkResult result = vkAllocateDescriptorSets(
            device, &(VkDescriptorSetAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = descriptor_pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &descriptor_set_layout
            }, &descriptor_set
        );
        check_vk_result(result, "Failed to allocate descriptor sets");
    }

    // 更新描述符集
    VkWriteDescriptorSet write_sets[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &(VkDescriptorBufferInfo){
                .buffer = in_buffer,
                .offset = 0,
                .range = VK_WHOLE_SIZE
            }
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &(VkDescriptorBufferInfo){
                .buffer = out_buffer,
                .offset = 0,
                .range = VK_WHOLE_SIZE
            }
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &(VkDescriptorBufferInfo){
                .buffer = out_buffer,
                .offset = 0,
                .range = VK_WHOLE_SIZE
            }
        },
    };

    vkUpdateDescriptorSets(
        device, sizeof(write_sets) / sizeof(write_sets[0]),
        write_sets, 0, NULL
    );

    // 创建管线布局
    VkPipelineLayout pipeline_layout = NULL;
    {
        VkResult result = vkCreatePipelineLayout(
            device, &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &descriptor_set_layout,
            }, NULL, &pipeline_layout
        );
        check_vk_result(result, "Failed to create pipeline layout");
    }

    // 创建计算管线
    VkPipeline pipeline = NULL;
    {
        VkResult result = vkCreateComputePipelines(
            device, NULL, 1, &(VkComputePipelineCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .layout = pipeline_layout,
                .stage = (VkPipelineShaderStageCreateInfo){
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = shader_module,
                    .pName = "main"
                },
            }, NULL, &pipeline
        );
        check_vk_result(result, "Failed to create compute pipeline");
    }

    // 创建命令池
    VkCommandPool cmd_pool = NULL;
    {
        VkResult result = vkCreateCommandPool(
            device, &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = queue_family_index
            }, NULL, &cmd_pool
        );
        check_vk_result(result, "Failed to create command pool");
    }

    // 分配命令缓冲区
    VkCommandBuffer cmd_buffer = NULL;
    {
        VkResult result = vkAllocateCommandBuffers(
            device, &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = cmd_pool,
                .commandBufferCount = 1,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
            }, &cmd_buffer
        );
        check_vk_result(result, "Failed to allocate command buffers");
    }

    // 记录命令
    {
        VkResult result = vkBeginCommandBuffer(
            cmd_buffer, &(VkCommandBufferBeginInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
            }
        );
        check_vk_result(result, "Failed to begin command buffer");

        vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(
            cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout, 0, 1, &descriptor_set,
            0, NULL
        );
        vkCmdDispatch(cmd_buffer, vector_size, 1, 1);

        result = vkEndCommandBuffer(cmd_buffer);
        check_vk_result(result, "Failed to end command buffer");
    }

    // 获取队列
    VkQueue queue = NULL;
    vkGetDeviceQueue(device, queue_family_index, 0, &queue);

    // 创建Fence
    VkFence fence = NULL;
    {
        VkResult result = vkCreateFence(
            device, &(VkFenceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            }, NULL, &fence
        );
        check_vk_result(result, "Failed to create fence");
    }

    // 提交命令
    {
        VkResult result = vkQueueSubmit(
            queue, 1, &(VkSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &cmd_buffer
            }, fence
        );
        check_vk_result(result, "Failed to submit queue");
    }

    // 等待完成
    {
        VkResult result = vkWaitForFences(device, 1, &fence, true, ~(u64)(0));
        check_vk_result(result, "Failed to wait for fence");
    }

    printf("Final Memory\n");
    {
        f32* data = NULL;
        vkMapMemory(device, memory, 0, total_memory_size, 0, (void**)(&data));

        f32* in_data = data;
        f32* out_data = data + (out_buffer_offset / sizeof(f32));

        printf("In Data : [ "); 
        for (u32 i = 0; i < vector_size; i++) {
            printf("%2.0f ", in_data[i]);
        }
        printf("]\n");

        printf("Out Data: [ "); 
        for (u32 i = 0; i < vector_size; i++) {
            printf("%2.0f ", out_data[i]);
        }
        printf("]\n");

        vkUnmapMemory(device, memory);
    }

    // 清理资源
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, descriptor_set_layout, NULL);
    vkDestroyShaderModule(device, shader_module, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyBuffer(device, in_buffer, NULL);
    vkDestroyBuffer(device, out_buffer, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    printf("Success!\n");
    return 0;
}