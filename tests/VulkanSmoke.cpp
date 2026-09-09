/* SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <vector>


static void
fail(const char* operation, VkResult result)
{
	fprintf(stderr, "%s failed: VkResult %d\n", operation, (int)result);
	exit(1);
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
