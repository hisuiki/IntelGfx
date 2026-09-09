#ifndef CUBE_MESSAGES_H
#define CUBE_MESSAGES_H

static const uint32 kStatistics = 'stat';
static const uint32 kVerticalSync = 'vsyn';
static const uint32 kRendererChanged = 'rend';
static const uint32 kVulkanFrameReady = 'vfrm';

enum renderer_mode {
	RENDERER_OPENGL = 0,
	RENDERER_VULKAN = 1
};

#endif
