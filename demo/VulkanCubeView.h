#ifndef VULKAN_CUBE_VIEW_H
#define VULKAN_CUBE_VIEW_H

#include <atomic>

#include <Bitmap.h>
#include <Locker.h>
#include <Messenger.h>
#include <String.h>
#include <View.h>

#include <vulkan/vulkan.h>


class VulkanCubeView : public BView {
public:
								VulkanCubeView(bool requireHardware,
									uint32 frameLimit,
									std::atomic<int>* exitCode);
	virtual						~VulkanCubeView();

	virtual void				AttachedToWindow();
	virtual void				DetachedFromWindow();
	virtual void				Draw(BRect updateRect);
	virtual void				FrameResized(float width, float height);
	virtual void				MessageReceived(BMessage* message);

			void				Stop();

private:
	static int32				_ThreadEntry(void* data);
			void				_Run();
			VkResult			_Initialize();
			void				_Destroy();
			VkResult			_CreatePipeline();
			VkResult			_CreateFrameResources(uint32 width,
									uint32 height);
			void				_DestroyFrameResources();
			VkResult			_Render(float angle);
			uint32				_FindMemoryType(uint32 typeBits,
									VkMemoryPropertyFlags required,
									VkMemoryPropertyFlags preferred) const;
			VkResult			_AllocateImageMemory(VkImage image,
									VkMemoryPropertyFlags preferred,
									VkDeviceMemory* memory);
			void				_SetError(const char* operation,
									VkResult result);
			void				_SendStatistics(float fps,
									bigtime_t frameTime, const char* detail);

			thread_id			fThread;
			std::atomic<bool>	fRunning;
			std::atomic<int>		fWidth;
			std::atomic<int>		fHeight;
			bool					fRequireHardware;
			uint32					fFrameLimit;
			std::atomic<int>*		fExitCode;
			BMessenger				fWindow;
			BMessenger				fSelf;
			BString					fRenderer;
			BString					fVersion;
			BString					fError;
			BLocker					fBitmapLock;
			BBitmap*				fBitmap;

			VkInstance				fInstance;
			VkPhysicalDevice		fPhysicalDevice;
			VkPhysicalDeviceMemoryProperties fMemoryProperties;
			VkDevice				fDevice;
			uint32					fQueueFamily;
			VkQueue					fQueue;
			VkCommandPool			fCommandPool;
			VkCommandBuffer			fCommandBuffer;
			VkRenderPass				fRenderPass;
			VkPipelineLayout			fPipelineLayout;
			VkPipeline				fPipeline;

			uint32					fRenderWidth;
			uint32					fRenderHeight;
			VkImage					fColorImage;
			VkDeviceMemory			fColorMemory;
			VkImageView				fColorView;
			VkImage					fDepthImage;
			VkDeviceMemory			fDepthMemory;
			VkImageView				fDepthView;
			VkFramebuffer			fFramebuffer;
			VkBuffer				fReadbackBuffer;
			VkDeviceMemory			fReadbackMemory;
			void*					fReadback;
};

#endif
