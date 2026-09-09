/* SPDX-License-Identifier: MIT */

#include "VulkanCubeView.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include <Application.h>
#include <Autolock.h>
#include <Font.h>
#include <Window.h>

#include "CubeMessages.h"
#include "CubeShaders.h"


static const bigtime_t kReportInterval = 1000000;


static void
multiply(float* result, const float* left, const float* right)
{
	float value[16];
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			value[column * 4 + row] = 0;
			for (int i = 0; i < 4; i++)
				value[column * 4 + row] += left[i * 4 + row]
					* right[column * 4 + i];
		}
	}
	memcpy(result, value, sizeof(value));
}


static void
cube_transform(float* result, float angle, float aspect)
{
	const float radians = angle * (float)M_PI / 180.0f;
	const float radiansY = radians * 0.7f;
	const float cosineX = cosf(radians);
	const float sineX = sinf(radians);
	const float cosineY = cosf(radiansY);
	const float sineY = sinf(radiansY);
	const float rotateX[16] = {
		1, 0, 0, 0,
		0, cosineX, sineX, 0,
		0, -sineX, cosineX, 0,
		0, 0, 0, 1
	};
	const float rotateY[16] = {
		cosineY, 0, -sineY, 0,
		0, 1, 0, 0,
		sineY, 0, cosineY, 0,
		0, 0, 0, 1
	};
	const float translation[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, -4.5f, 1
	};
	const float nearPlane = 1.0f;
	const float farPlane = 20.0f;
	const float focal = 1.0f / tanf(45.0f * (float)M_PI / 360.0f);
	const float projection[16] = {
		focal / aspect, 0, 0, 0,
		0, focal, 0, 0,
		0, 0, farPlane / (nearPlane - farPlane), -1,
		0, 0, nearPlane * farPlane / (nearPlane - farPlane), 0
	};
	float rotation[16];
	float model[16];
	multiply(rotation, rotateY, rotateX);
	multiply(model, translation, rotation);
	multiply(result, projection, model);
}


VulkanCubeView::VulkanCubeView(bool requireHardware, uint32 frameLimit,
	std::atomic<int>* exitCode)
	:
	BView("vulkan cube", B_WILL_DRAW | B_FRAME_EVENTS),
	fThread(-1),
	fRunning(false),
	fWidth(1),
	fHeight(1),
	fRequireHardware(requireHardware),
	fFrameLimit(frameLimit),
	fExitCode(exitCode),
	fBitmapLock("Vulkan cube bitmap"),
	fBitmap(NULL),
	fInstance(VK_NULL_HANDLE),
	fPhysicalDevice(VK_NULL_HANDLE),
	fDevice(VK_NULL_HANDLE),
	fQueueFamily(UINT32_MAX),
	fQueue(VK_NULL_HANDLE),
	fCommandPool(VK_NULL_HANDLE),
	fCommandBuffer(VK_NULL_HANDLE),
	fRenderPass(VK_NULL_HANDLE),
	fPipelineLayout(VK_NULL_HANDLE),
	fPipeline(VK_NULL_HANDLE),
	fRenderWidth(0),
	fRenderHeight(0),
	fColorImage(VK_NULL_HANDLE),
	fColorMemory(VK_NULL_HANDLE),
	fColorView(VK_NULL_HANDLE),
	fDepthImage(VK_NULL_HANDLE),
	fDepthMemory(VK_NULL_HANDLE),
	fDepthView(VK_NULL_HANDLE),
	fFramebuffer(VK_NULL_HANDLE),
	fReadbackBuffer(VK_NULL_HANDLE),
	fReadbackMemory(VK_NULL_HANDLE),
	fReadback(NULL)
{
	SetExplicitMinSize(BSize(320, 240));
	SetViewColor(20, 23, 28);
}


VulkanCubeView::~VulkanCubeView()
{
	Stop();
	_Destroy();
}


void
VulkanCubeView::AttachedToWindow()
{
	BView::AttachedToWindow();
	BRect bounds = Bounds();
	fWidth = std::max(1, (int)bounds.IntegerWidth() + 1);
	fHeight = std::max(1, (int)bounds.IntegerHeight() + 1);
	fWindow = BMessenger(NULL, Window());
	fSelf = BMessenger(this);
	fRunning = true;
	fThread = spawn_thread(_ThreadEntry, "Vulkan cube", B_NORMAL_PRIORITY, this);
	if (fThread < 0 || resume_thread(fThread) != B_OK) {
		if (fThread >= 0)
			kill_thread(fThread);
		fThread = -1;
		fRunning = false;
		_SetError("spawn_thread", VK_ERROR_INITIALIZATION_FAILED);
	}
}


void
VulkanCubeView::DetachedFromWindow()
{
	Stop();
	BView::DetachedFromWindow();
}


void
VulkanCubeView::Draw(BRect updateRect)
{
	BAutolock lock(&fBitmapLock);
	if (fBitmap != NULL) {
		DrawBitmap(fBitmap, fBitmap->Bounds(), Bounds(), B_FILTER_BITMAP_BILINEAR);
		return;
	}
	SetHighColor(235, 235, 235);
	BString text = fError.IsEmpty() ? "Starting Vulkan…" : fError;
	DrawString(text.String(), BPoint(12, 12 + be_plain_font->Size()));
}


void
VulkanCubeView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	fWidth = std::max(1, (int)width + 1);
	fHeight = std::max(1, (int)height + 1);
}


void
VulkanCubeView::MessageReceived(BMessage* message)
{
	if (message->what == kVulkanFrameReady) {
		Invalidate();
		return;
	}
	BView::MessageReceived(message);
}


int32
VulkanCubeView::_ThreadEntry(void* data)
{
	((VulkanCubeView*)data)->_Run();
	return 0;
}


void
VulkanCubeView::Stop()
{
	fRunning = false;
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
}


void
VulkanCubeView::_Run()
{
	VkResult result = _Initialize();
	if (result != VK_SUCCESS) {
		_SetError("Vulkan initialization", result);
		return;
	}

	printf("VK renderer: %s\n", fRenderer.String());
	printf("VK version:  %s\n", fVersion.String());
	fflush(stdout);
	uint32 frames = 0;
	uint32 totalFrames = 0;
	bigtime_t startedAt = system_time();
	bigtime_t reportedAt = startedAt;
	bigtime_t completedTime = 0;
	bool pixelsChecked = false;

	while (fRunning.load()) {
		uint32 width = (uint32)fWidth.load();
		uint32 height = (uint32)fHeight.load();
		if (width != fRenderWidth || height != fRenderHeight) {
			result = vkDeviceWaitIdle(fDevice);
			if (result == VK_SUCCESS) {
				_DestroyFrameResources();
				result = _CreateFrameResources(width, height);
			}
			if (result != VK_SUCCESS) {
				_SetError("Vulkan resize", result);
				break;
			}
		}

		bigtime_t frameStart = system_time();
		float angle = fmod((frameStart - startedAt) * 0.00006, 360.0);
		result = _Render(angle);
		if (result != VK_SUCCESS) {
			_SetError("Vulkan frame", result);
			break;
		}

		if (!pixelsChecked) {
			const uint8* pixels = (const uint8*)fReadback;
			const uint8* center = pixels
				+ ((fRenderHeight / 2) * fRenderWidth + fRenderWidth / 2) * 4;
			const uint8* corner = pixels;
			bool visible = memcmp(center, corner, 3) != 0;
			printf("Vulkan readback: center=%u,%u,%u corner=%u,%u,%u (%s)\n",
				center[2], center[1], center[0], corner[2], corner[1], corner[0],
				visible ? "geometry visible" : "no geometry detected");
			if (!visible)
				*fExitCode = 1;
			pixelsChecked = true;
		}

		{
			BAutolock lock(&fBitmapLock);
			const uint8* source = (const uint8*)fReadback;
			uint8* destination = (uint8*)fBitmap->Bits();
			for (uint32 row = 0; row < fRenderHeight; row++) {
				memcpy(destination + row * fBitmap->BytesPerRow(),
					source + row * fRenderWidth * 4, fRenderWidth * 4);
			}
		}
		fSelf.SendMessage(kVulkanFrameReady);

		bigtime_t now = system_time();
		completedTime += now - frameStart;
		frames++;
		totalFrames++;
		if (now - reportedAt >= kReportInterval || totalFrames == fFrameLimit) {
			float fps = frames * 1000000.0f / (now - reportedAt);
			bigtime_t average = completedTime / frames;
			_SendStatistics(fps, average, NULL);
			printf("%.1f completed Vulkan frames/s, %.2f ms/frame, %u frames\n",
				fps, average / 1000.0, totalFrames);
			fflush(stdout);
			frames = 0;
			completedTime = 0;
			reportedAt = now;
		}
		if (*fExitCode != 0
			|| (fFrameLimit != 0 && totalFrames >= fFrameLimit)) {
			fRunning = false;
			be_app->PostMessage(B_QUIT_REQUESTED);
		}
	}

	if (fDevice != VK_NULL_HANDLE)
		vkDeviceWaitIdle(fDevice);
}


VkResult
VulkanCubeView::_Initialize()
{
	VkApplicationInfo applicationInfo = {};
	applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "IntelGfx cube";
	applicationInfo.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo instanceInfo = {};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result = vkCreateInstance(&instanceInfo, NULL, &fInstance);
	if (result != VK_SUCCESS)
		return result;

	uint32 deviceCount = 0;
	result = vkEnumeratePhysicalDevices(fInstance, &deviceCount, NULL);
	if (result != VK_SUCCESS || deviceCount == 0)
		return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
	std::vector<VkPhysicalDevice> devices(deviceCount);
	result = vkEnumeratePhysicalDevices(fInstance, &deviceCount, devices.data());
	if (result != VK_SUCCESS)
		return result;

	for (size_t deviceIndex = 0; deviceIndex < devices.size(); deviceIndex++) {
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(devices[deviceIndex], &properties);
		if (fRequireHardware && properties.vendorID != 0x8086)
			continue;
		uint32 queueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[deviceIndex], &queueCount,
			NULL);
		std::vector<VkQueueFamilyProperties> queues(queueCount);
		vkGetPhysicalDeviceQueueFamilyProperties(devices[deviceIndex], &queueCount,
			queues.data());
		for (uint32 queue = 0; queue < queueCount; queue++) {
			if ((queues[queue].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
				fPhysicalDevice = devices[deviceIndex];
				fQueueFamily = queue;
				fRenderer.SetToFormat("%s — Vulkan ANV", properties.deviceName);
				fVersion.SetToFormat("%u.%u.%u", VK_VERSION_MAJOR(properties.apiVersion),
					VK_VERSION_MINOR(properties.apiVersion),
					VK_VERSION_PATCH(properties.apiVersion));
				break;
			}
		}
		if (fPhysicalDevice != VK_NULL_HANDLE)
			break;
	}
	if (fPhysicalDevice == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	vkGetPhysicalDeviceMemoryProperties(fPhysicalDevice, &fMemoryProperties);
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = fQueueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;
	VkDeviceCreateInfo deviceInfo = {};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	result = vkCreateDevice(fPhysicalDevice, &deviceInfo, NULL, &fDevice);
	if (result != VK_SUCCESS)
		return result;
	vkGetDeviceQueue(fDevice, fQueueFamily, 0, &fQueue);

	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = fQueueFamily;
	result = vkCreateCommandPool(fDevice, &poolInfo, NULL, &fCommandPool);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferAllocateInfo commandInfo = {};
	commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandInfo.commandPool = fCommandPool;
	commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(fDevice, &commandInfo, &fCommandBuffer);
	if (result != VK_SUCCESS)
		return result;
	return _CreatePipeline();
}


VkResult
VulkanCubeView::_CreatePipeline()
{
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	attachments[1].format = VK_FORMAT_D32_SFLOAT;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference colorReference = { 0,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReference = { 1,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	subpass.pDepthStencilAttachment = &depthReference;
	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = dependency.srcStageMask;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 2;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;
	VkResult result = vkCreateRenderPass(fDevice, &renderPassInfo, NULL,
		&fRenderPass);
	if (result != VK_SUCCESS)
		return result;

	VkPushConstantRange pushConstant = {};
	pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushConstant.size = 16 * sizeof(float);
	VkPipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushConstant;
	result = vkCreatePipelineLayout(fDevice, &layoutInfo, NULL, &fPipelineLayout);
	if (result != VK_SUCCESS)
		return result;

	VkShaderModuleCreateInfo vertexInfo = {};
	vertexInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertexInfo.codeSize = sizeof(kVertexShader);
	vertexInfo.pCode = kVertexShader;
	VkShaderModuleCreateInfo fragmentInfo = vertexInfo;
	fragmentInfo.codeSize = sizeof(kFragmentShader);
	fragmentInfo.pCode = kFragmentShader;
	VkShaderModule vertexShader = VK_NULL_HANDLE;
	VkShaderModule fragmentShader = VK_NULL_HANDLE;
	result = vkCreateShaderModule(fDevice, &vertexInfo, NULL, &vertexShader);
	if (result == VK_SUCCESS)
		result = vkCreateShaderModule(fDevice, &fragmentInfo, NULL, &fragmentShader);
	if (result != VK_SUCCESS) {
		if (vertexShader != VK_NULL_HANDLE)
			vkDestroyShaderModule(fDevice, vertexShader, NULL);
		return result;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentShader;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo assembly = {};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport = {};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rasterization = {};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample = {};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depth = {};
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;
	VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
		| VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
		| VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo colorBlend = {};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &colorBlendAttachment;
	VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = {};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamicStates;
	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &assembly;
	pipelineInfo.pViewportState = &viewport;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depth;
	pipelineInfo.pColorBlendState = &colorBlend;
	pipelineInfo.pDynamicState = &dynamic;
	pipelineInfo.layout = fPipelineLayout;
	pipelineInfo.renderPass = fRenderPass;
	result = vkCreateGraphicsPipelines(fDevice, VK_NULL_HANDLE, 1, &pipelineInfo,
		NULL, &fPipeline);
	vkDestroyShaderModule(fDevice, fragmentShader, NULL);
	vkDestroyShaderModule(fDevice, vertexShader, NULL);
	return result;
}


uint32
VulkanCubeView::_FindMemoryType(uint32 typeBits,
	VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const
{
	for (uint32 i = 0; i < fMemoryProperties.memoryTypeCount; i++) {
		VkMemoryPropertyFlags flags = fMemoryProperties.memoryTypes[i].propertyFlags;
		if ((typeBits & (1U << i)) != 0 && (flags & required) == required
			&& (flags & preferred) == preferred)
			return i;
	}
	for (uint32 i = 0; i < fMemoryProperties.memoryTypeCount; i++) {
		VkMemoryPropertyFlags flags = fMemoryProperties.memoryTypes[i].propertyFlags;
		if ((typeBits & (1U << i)) != 0 && (flags & required) == required)
			return i;
	}
	return UINT32_MAX;
}


VkResult
VulkanCubeView::_AllocateImageMemory(VkImage image,
	VkMemoryPropertyFlags preferred, VkDeviceMemory* memory)
{
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(fDevice, image, &requirements);
	uint32 memoryType = _FindMemoryType(requirements.memoryTypeBits, 0, preferred);
	if (memoryType == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo = {};
	allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	VkResult result = vkAllocateMemory(fDevice, &allocationInfo, NULL, memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(fDevice, image, *memory, 0);
	return result;
}


VkResult
VulkanCubeView::_CreateFrameResources(uint32 width, uint32 height)
{
	fRenderWidth = width;
	fRenderHeight = height;
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result = vkCreateImage(fDevice, &imageInfo, NULL, &fColorImage);
	if (result != VK_SUCCESS)
		return result;
	result = _AllocateImageMemory(fColorImage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&fColorMemory);
	if (result != VK_SUCCESS)
		return result;

	imageInfo.format = VK_FORMAT_D32_SFLOAT;
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	result = vkCreateImage(fDevice, &imageInfo, NULL, &fDepthImage);
	if (result != VK_SUCCESS)
		return result;
	result = _AllocateImageMemory(fDepthImage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&fDepthMemory);
	if (result != VK_SUCCESS)
		return result;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	viewInfo.image = fColorImage;
	result = vkCreateImageView(fDevice, &viewInfo, NULL, &fColorView);
	if (result != VK_SUCCESS)
		return result;
	viewInfo.image = fDepthImage;
	viewInfo.format = VK_FORMAT_D32_SFLOAT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	result = vkCreateImageView(fDevice, &viewInfo, NULL, &fDepthView);
	if (result != VK_SUCCESS)
		return result;

	VkImageView attachments[] = { fColorView, fDepthView };
	VkFramebufferCreateInfo framebufferInfo = {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = fRenderPass;
	framebufferInfo.attachmentCount = 2;
	framebufferInfo.pAttachments = attachments;
	framebufferInfo.width = width;
	framebufferInfo.height = height;
	framebufferInfo.layers = 1;
	result = vkCreateFramebuffer(fDevice, &framebufferInfo, NULL, &fFramebuffer);
	if (result != VK_SUCCESS)
		return result;

	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = (VkDeviceSize)width * height * 4;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(fDevice, &bufferInfo, NULL, &fReadbackBuffer);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(fDevice, fReadbackBuffer, &requirements);
	uint32 memoryType = _FindMemoryType(requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	if (memoryType == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo = {};
	allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(fDevice, &allocationInfo, NULL, &fReadbackMemory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindBufferMemory(fDevice, fReadbackBuffer, fReadbackMemory, 0);
	if (result != VK_SUCCESS)
		return result;
	result = vkMapMemory(fDevice, fReadbackMemory, 0, bufferInfo.size, 0,
		&fReadback);
	if (result != VK_SUCCESS)
		return result;

	BBitmap* bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1),
		B_RGBA32);
	if (bitmap->InitCheck() != B_OK) {
		delete bitmap;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	{
		BAutolock lock(&fBitmapLock);
		delete fBitmap;
		fBitmap = bitmap;
	}
	return VK_SUCCESS;
}


VkResult
VulkanCubeView::_Render(float angle)
{
	VkResult result = vkResetCommandBuffer(fCommandBuffer, 0);
	if (result != VK_SUCCESS)
		return result;
	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(fCommandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
		return result;
	VkClearValue clearValues[2] = {};
	clearValues[0].color.float32[0] = 0.08f;
	clearValues[0].color.float32[1] = 0.09f;
	clearValues[0].color.float32[2] = 0.11f;
	clearValues[0].color.float32[3] = 1.0f;
	clearValues[1].depthStencil.depth = 1.0f;
	VkRenderPassBeginInfo renderInfo = {};
	renderInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderInfo.renderPass = fRenderPass;
	renderInfo.framebuffer = fFramebuffer;
	renderInfo.renderArea.extent.width = fRenderWidth;
	renderInfo.renderArea.extent.height = fRenderHeight;
	renderInfo.clearValueCount = 2;
	renderInfo.pClearValues = clearValues;
	vkCmdBeginRenderPass(fCommandBuffer, &renderInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(fCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fPipeline);
	VkViewport viewport = { 0, 0, (float)fRenderWidth, (float)fRenderHeight,
		0, 1 };
	VkRect2D scissor = { { 0, 0 }, { fRenderWidth, fRenderHeight } };
	vkCmdSetViewport(fCommandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(fCommandBuffer, 0, 1, &scissor);
	float transform[16];
	cube_transform(transform, angle, (float)fRenderWidth / fRenderHeight);
	vkCmdPushConstants(fCommandBuffer, fPipelineLayout,
		VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), transform);
	vkCmdDraw(fCommandBuffer, 36, 1, 0, 0);
	vkCmdEndRenderPass(fCommandBuffer);
	VkBufferImageCopy copy = {};
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = fRenderWidth;
	copy.imageExtent.height = fRenderHeight;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(fCommandBuffer, fColorImage,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fReadbackBuffer, 1, &copy);
	VkBufferMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = fReadbackBuffer;
	barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(fCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
	result = vkEndCommandBuffer(fCommandBuffer);
	if (result != VK_SUCCESS)
		return result;
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &fCommandBuffer;
	result = vkQueueSubmit(fQueue, 1, &submit, VK_NULL_HANDLE);
	if (result == VK_SUCCESS)
		result = vkQueueWaitIdle(fQueue);
	return result;
}


void
VulkanCubeView::_DestroyFrameResources()
{
	{
		BAutolock lock(&fBitmapLock);
		delete fBitmap;
		fBitmap = NULL;
	}
	if (fReadback != NULL)
		vkUnmapMemory(fDevice, fReadbackMemory);
	fReadback = NULL;
	if (fReadbackBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(fDevice, fReadbackBuffer, NULL);
	if (fReadbackMemory != VK_NULL_HANDLE)
		vkFreeMemory(fDevice, fReadbackMemory, NULL);
	if (fFramebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(fDevice, fFramebuffer, NULL);
	if (fDepthView != VK_NULL_HANDLE)
		vkDestroyImageView(fDevice, fDepthView, NULL);
	if (fColorView != VK_NULL_HANDLE)
		vkDestroyImageView(fDevice, fColorView, NULL);
	if (fDepthImage != VK_NULL_HANDLE)
		vkDestroyImage(fDevice, fDepthImage, NULL);
	if (fDepthMemory != VK_NULL_HANDLE)
		vkFreeMemory(fDevice, fDepthMemory, NULL);
	if (fColorImage != VK_NULL_HANDLE)
		vkDestroyImage(fDevice, fColorImage, NULL);
	if (fColorMemory != VK_NULL_HANDLE)
		vkFreeMemory(fDevice, fColorMemory, NULL);
	fReadbackBuffer = VK_NULL_HANDLE;
	fReadbackMemory = VK_NULL_HANDLE;
	fFramebuffer = VK_NULL_HANDLE;
	fDepthView = VK_NULL_HANDLE;
	fColorView = VK_NULL_HANDLE;
	fDepthImage = VK_NULL_HANDLE;
	fDepthMemory = VK_NULL_HANDLE;
	fColorImage = VK_NULL_HANDLE;
	fColorMemory = VK_NULL_HANDLE;
	fRenderWidth = 0;
	fRenderHeight = 0;
}


void
VulkanCubeView::_Destroy()
{
	if (fDevice != VK_NULL_HANDLE)
		vkDeviceWaitIdle(fDevice);
	if (fDevice != VK_NULL_HANDLE)
		_DestroyFrameResources();
	if (fPipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(fDevice, fPipeline, NULL);
	if (fPipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(fDevice, fPipelineLayout, NULL);
	if (fRenderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(fDevice, fRenderPass, NULL);
	if (fCommandPool != VK_NULL_HANDLE)
		vkDestroyCommandPool(fDevice, fCommandPool, NULL);
	if (fDevice != VK_NULL_HANDLE)
		vkDestroyDevice(fDevice, NULL);
	if (fInstance != VK_NULL_HANDLE)
		vkDestroyInstance(fInstance, NULL);
	fPipeline = VK_NULL_HANDLE;
	fPipelineLayout = VK_NULL_HANDLE;
	fRenderPass = VK_NULL_HANDLE;
	fCommandPool = VK_NULL_HANDLE;
	fDevice = VK_NULL_HANDLE;
	fInstance = VK_NULL_HANDLE;
}


void
VulkanCubeView::_SetError(const char* operation, VkResult result)
{
	*fExitCode = 1;
	{
		BAutolock lock(&fBitmapLock);
		fError.SetToFormat("%s failed (VkResult %d)", operation, (int)result);
	}
	fprintf(stderr, "%s\n", fError.String());
	_SendStatistics(0, 0, fError.String());
	fSelf.SendMessage(kVulkanFrameReady);
	if (fFrameLimit != 0 || fRequireHardware)
		be_app->PostMessage(B_QUIT_REQUESTED);
}


void
VulkanCubeView::_SendStatistics(float fps, bigtime_t frameTime,
	const char* detail)
{
	BMessage message(kStatistics);
	message.AddFloat("fps", fps);
	message.AddInt64("frame", frameTime);
	message.AddString("renderer", fRenderer);
	message.AddString("vendor", "Intel");
	message.AddString("version", fVersion);
	message.AddString("mode", "Vulkan");
	if (detail != NULL)
		message.AddString("error", detail);
	fWindow.SendMessage(&message);
}
