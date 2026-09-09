/* SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>


static void
fail(const char* operation, VkResult result)
{
	fprintf(stderr, "%s failed: VkResult %d\n", operation, (int)result);
	exit(1);
}


static uint32_t
find_memory_type(VkPhysicalDevice physicalDevice, uint32_t typeBits,
	VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((typeBits & (1U << i)) != 0
			&& (properties.memoryTypes[i].propertyFlags & required) == required)
			return i;
	}
	fprintf(stderr, "no Vulkan memory type with flags 0x%x\n", required);
	exit(1);
}


static void
test_submission(VkPhysicalDevice physicalDevice, VkDevice device,
	uint32_t queueFamily)
{
	const VkDeviceSize size = 4096;
	const uint32_t pattern = 0x49524758; // "IRGX"
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer;
	VkResult result = vkCreateBuffer(device, &bufferInfo, NULL, &buffer);
	if (result != VK_SUCCESS)
		fail("vkCreateBuffer", result);

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	VkMemoryAllocateInfo allocationInfo = {};
	allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize = requirements.size;
	allocationInfo.memoryTypeIndex = find_memory_type(physicalDevice,
		requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory memory;
	result = vkAllocateMemory(device, &allocationInfo, NULL, &memory);
	if (result != VK_SUCCESS)
		fail("vkAllocateMemory", result);
	result = vkBindBufferMemory(device, buffer, memory, 0);
	if (result != VK_SUCCESS)
		fail("vkBindBufferMemory", result);

	void* mapped;
	result = vkMapMemory(device, memory, 0, size, 0, &mapped);
	if (result != VK_SUCCESS)
		fail("vkMapMemory", result);
	memset(mapped, 0, size);
	vkUnmapMemory(device, memory);

	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = queueFamily;
	VkCommandPool pool;
	result = vkCreateCommandPool(device, &poolInfo, NULL, &pool);
	if (result != VK_SUCCESS)
		fail("vkCreateCommandPool", result);
	VkCommandBufferAllocateInfo commandInfo = {};
	commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandInfo.commandPool = pool;
	commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer;
	result = vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer);
	if (result != VK_SUCCESS)
		fail("vkAllocateCommandBuffers", result);
	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
		fail("vkBeginCommandBuffer", result);
	vkCmdFillBuffer(commandBuffer, buffer, 0, size, pattern);
	VkBufferMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer;
	barrier.offset = 0;
	barrier.size = size;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
	result = vkEndCommandBuffer(commandBuffer);
	if (result != VK_SUCCESS)
		fail("vkEndCommandBuffer", result);

	VkQueue queue;
	vkGetDeviceQueue(device, queueFamily, 0, &queue);
	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
	if (result != VK_SUCCESS)
		fail("vkQueueSubmit", result);
	result = vkQueueWaitIdle(queue);
	if (result != VK_SUCCESS)
		fail("vkQueueWaitIdle", result);

	result = vkMapMemory(device, memory, 0, size, 0, &mapped);
	if (result != VK_SUCCESS)
		fail("vkMapMemory(readback)", result);
	const uint32_t* words = (const uint32_t*)mapped;
	for (size_t i = 0; i < size / sizeof(uint32_t); i++) {
		if (words[i] != pattern) {
			fprintf(stderr, "Vulkan GPU readback mismatch at word %zu: 0x%08x\n",
				i, words[i]);
			exit(1);
		}
	}
	vkUnmapMemory(device, memory);
	vkDestroyCommandPool(device, pool, NULL);
	vkDestroyBuffer(device, buffer, NULL);
	vkFreeMemory(device, memory, NULL);
	puts("PASS: Vulkan command submission and host-visible GPU readback.");
}


int
main()
{
	VkApplicationInfo applicationInfo = {};
	applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "IntelGfx Vulkan smoke test";
	applicationInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo instanceInfo = {};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;

	VkInstance instance;
	VkResult result = vkCreateInstance(&instanceInfo, NULL, &instance);
	if (result != VK_SUCCESS)
		fail("vkCreateInstance", result);

	uint32_t deviceCount = 0;
	result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
	if (result != VK_SUCCESS)
		fail("vkEnumeratePhysicalDevices(count)", result);
	if (deviceCount == 0) {
		fprintf(stderr, "no Vulkan physical devices\n");
		return 2;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
	if (result != VK_SUCCESS)
		fail("vkEnumeratePhysicalDevices", result);

	bool foundIntel = false;
	for (VkPhysicalDevice physicalDevice : devices) {
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		printf("%04x:%04x %s Vulkan %u.%u.%u\n",
			properties.vendorID, properties.deviceID, properties.deviceName,
			VK_VERSION_MAJOR(properties.apiVersion),
			VK_VERSION_MINOR(properties.apiVersion),
			VK_VERSION_PATCH(properties.apiVersion));
		if (properties.vendorID != 0x8086)
			continue;

		uint32_t queueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, NULL);
		std::vector<VkQueueFamilyProperties> queues(queueCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount,
			queues.data());
		uint32_t queueFamily = UINT32_MAX;
		for (uint32_t i = 0; i < queueCount; i++) {
			if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
				queueFamily = i;
				break;
			}
		}
		if (queueFamily == UINT32_MAX)
			continue;

		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo = {};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		VkDeviceCreateInfo deviceInfo = {};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		VkDevice device;
		result = vkCreateDevice(physicalDevice, &deviceInfo, NULL, &device);
		if (result != VK_SUCCESS)
			fail("vkCreateDevice(Intel)", result);
		result = vkDeviceWaitIdle(device);
		if (result != VK_SUCCESS)
			fail("vkDeviceWaitIdle(Intel)", result);
		test_submission(physicalDevice, device, queueFamily);
		vkDestroyDevice(device, NULL);
		foundIntel = true;
	}

	vkDestroyInstance(instance, NULL);
	if (!foundIntel) {
		fprintf(stderr, "no usable Intel Vulkan device\n");
		return 3;
	}
	puts("PASS: Intel Vulkan instance, physical device, and logical device.");
	return 0;
}
