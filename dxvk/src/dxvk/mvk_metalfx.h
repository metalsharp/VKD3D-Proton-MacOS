/*
 * mvk_metalfx.h
 *
 * MetalSharp: Apple's MetalFX upscalers through Vulkan, for D3D layers (VKD3D-Proton, DXVK) on MoltenVK.
 *
 * Wine's Vulkan layer forwards only the functions and structures of known Vulkan extensions, but on 64-bit it hands
 * a VkDependencyInfo and a VkSwapchainCreateInfoKHR to the driver as they are, extension chain included. So:
 *
 * - a VkMetalFXScaleInfoMVK in the pNext chain of vkCmdPipelineBarrier2's VkDependencyInfo runs a MetalFX temporal
 *   or denoised scaler, or its frame interpolator, after the barrier, in the command buffer's order;
 * - a VkSwapchainMetalFXSpatialCreateInfoMVK in the pNext chain of VkSwapchainCreateInfoKHR has the swapchain's
 *   images upscaled with MetalFX's spatial scaler to the surface's native size when they are presented.
 *
 * A MoltenVK that supports them says so in VkPhysicalDeviceDriverProperties::driverInfo: the words "metalfx"
 * (temporal and spatial scalers), "metalfx-denoised" (MetalFX's denoised scaler, macOS 26) and
 * "metalfx-interpolation" (MetalFX's frame interpolator keeping its own history, macOS 27).
 *
 * This file is copied as it is into the D3D layers that use it.
 */

#ifndef __mvk_metalfx_h_
#define __mvk_metalfx_h_ 1

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_STRUCTURE_TYPE_METALFX_SCALE_INFO_MVK ((VkStructureType)1000999100)
#define VK_STRUCTURE_TYPE_SWAPCHAIN_METALFX_SPATIAL_CREATE_INFO_MVK ((VkStructureType)1000999101)
#define VK_STRUCTURE_TYPE_METALFX_FRAME_INTERPOLATION_INFO_MVK ((VkStructureType)1000999102)
#define VK_STRUCTURE_TYPE_METALFX_CONTENT_OFFSETS_MVK ((VkStructureType)1000999103)

#define VK_MVK_METALFX_DRIVER_INFO "metalfx"
#define VK_MVK_METALFX_DENOISED_DRIVER_INFO "metalfx-denoised"
#define VK_MVK_METALFX_INTERPOLATION_DRIVER_INFO "metalfx-interpolation"

typedef enum VkMetalFXScalerTypeMVK {
	VK_METALFX_SCALER_TYPE_NONE_MVK = 0,		/* only releases pReleaseScalerIds */
	VK_METALFX_SCALER_TYPE_TEMPORAL_MVK = 1,	/* MTLFXTemporalScaler (DLSS Super Resolution) */
	VK_METALFX_SCALER_TYPE_DENOISED_MVK = 2,	/* MTLFXTemporalDenoisedScaler (DLSS Ray Reconstruction) */
	VK_METALFX_SCALER_TYPE_FRAME_INTERPOLATION_MVK = 3,	/* MTLFXFrameInterpolator (DLSS Frame Generation): the frame
													   between the last one and the color image, with a
													   VkMetalFXFrameInterpolationInfoMVK in pNext */
} VkMetalFXScalerTypeMVK;

/* The textures of a scale, by their index in images and formats */
typedef enum VkMetalFXImageMVK {
	VK_METALFX_IMAGE_COLOR_MVK = 0,
	VK_METALFX_IMAGE_DEPTH_MVK = 1,
	VK_METALFX_IMAGE_MOTION_MVK = 2,
	VK_METALFX_IMAGE_OUTPUT_MVK = 3,
	VK_METALFX_IMAGE_EXPOSURE_MVK = 4,				/* optional */
	VK_METALFX_IMAGE_DIFFUSE_ALBEDO_MVK = 5,		/* denoised */
	VK_METALFX_IMAGE_SPECULAR_ALBEDO_MVK = 6,		/* denoised */
	VK_METALFX_IMAGE_NORMALS_MVK = 7,				/* denoised */
	VK_METALFX_IMAGE_ROUGHNESS_MVK = 8,				/* denoised */
	VK_METALFX_IMAGE_SPECULAR_HIT_DISTANCE_MVK = 9,	/* denoised, optional */
	VK_METALFX_IMAGE_COUNT_MVK = 10,
	/* Frame interpolation: color, depth, motion, output and, optionally, the UI (color and alpha) in the denoised
	 * diffuse albedo's place. Depth and motion are at the render size, the others at the swapchain's. */
	VK_METALFX_IMAGE_UI_MVK = VK_METALFX_IMAGE_DIFFUSE_ALBEDO_MVK,
	/* Temporal: the reactive mask (optional, one channel, 1 favoring the current frame: DLSS's
	 * Bias.Current.Color.Mask), in the denoised diffuse albedo's place. A MoltenVK before it ignores it. */
	VK_METALFX_IMAGE_REACTIVE_MVK = VK_METALFX_IMAGE_DIFFUSE_ALBEDO_MVK,
} VkMetalFXImageMVK;

typedef enum VkMetalFXScaleFlagBitsMVK {
	VK_METALFX_SCALE_AUTO_EXPOSURE_BIT_MVK = 0x1,
	VK_METALFX_SCALE_RESET_BIT_MVK = 0x2,
	VK_METALFX_SCALE_DEPTH_REVERSED_BIT_MVK = 0x4,
	/* The motion vectors are at the output resolution (DLSS without MV_LOWRES): they are brought to the input's. */
	VK_METALFX_SCALE_MOTION_DISPLAY_RES_BIT_MVK = 0x8,
	/* Temporal: the motion vectors include the camera jitter (DLSS's MVJittered), which MetalFX takes out with the
	 * jitter offset where it can (macOS 27). */
	VK_METALFX_SCALE_JITTERED_MOTION_BIT_MVK = 0x10,
} VkMetalFXScaleFlagBitsMVK;

/*
 * One scale. The scaler is MoltenVK's, kept under scalerId (any nonzero value the caller picks, one per upscaling
 * feature, since a temporal scaler keeps the history of its earlier frames) and made again when the images' sizes or
 * formats change. Its caller releases it by naming its id in pReleaseScalerIds of a later scale (of any type,
 * VK_METALFX_SCALER_TYPE_NONE_MVK included); MoltenVK releases the rest with the device.
 *
 * formats[i] is the format images[i] is read or written as (VK_FORMAT_UNDEFINED: the image's own), such as a float
 * format for an integer-typed depth; a different format needs an image made with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT.
 * The images are read in the general layout, and their contents must be available to the barrier's second scope;
 * the output is written for a later barrier to make visible.
 */
typedef struct VkMetalFXScaleInfoMVK {
	VkStructureType sType;
	const void* pNext;
	uint64_t scalerId;
	uint32_t scalerType;					/* VkMetalFXScalerTypeMVK */
	uint32_t flags;							/* VkMetalFXScaleFlagBitsMVK */
	VkImage images[VK_METALFX_IMAGE_COUNT_MVK];
	VkFormat formats[VK_METALFX_IMAGE_COUNT_MVK];
	uint32_t inputContentWidth;				/* 0: the color image's width */
	uint32_t inputContentHeight;			/* 0: the color image's height */
	float motionVectorScaleX;
	float motionVectorScaleY;
	float jitterOffsetX;
	float jitterOffsetY;
	float preExposure;
	float worldToViewMatrix[16];			/* denoised: row-major, of row vectors (D3D's) */
	float viewToClipMatrix[16];				/* denoised */
	uint32_t releaseScalerIdCount;
	const uint64_t* pReleaseScalerIds;
} VkMetalFXScaleInfoMVK;

/* The camera and time of a frame interpolation (in the pNext chain of its VkMetalFXScaleInfoMVK). */
typedef struct VkMetalFXFrameInterpolationInfoMVK {
	VkStructureType sType;
	const void* pNext;
	float deltaTime;						/* seconds since the last frame */
	float nearPlane;
	float farPlane;
	float fieldOfView;						/* vertical, in degrees */
	float aspectRatio;
	uint32_t uiComposited;					/* the UI image's contents are in the color image too */
} VkMetalFXFrameInterpolationInfoMVK;

/* Temporal: where each image's region starts, in pixels (DLSS's subrect bases), in the pNext chain of its
 * VkMetalFXScaleInfoMVK; MetalFX reads the regions in place where it can (macOS 27). */
typedef struct VkMetalFXContentOffsetsMVK {
	VkStructureType sType;
	const void* pNext;
	uint32_t offsetX[VK_METALFX_IMAGE_COUNT_MVK];	/* by VkMetalFXImageMVK: color, depth, motion, output, reactive */
	uint32_t offsetY[VK_METALFX_IMAGE_COUNT_MVK];
} VkMetalFXContentOffsetsMVK;

/* MetalFX spatial upscaling at present, to the surface's native size, where that is larger than the images' */
typedef struct VkSwapchainMetalFXSpatialCreateInfoMVK {
	VkStructureType sType;
	const void* pNext;
	uint32_t enable;
} VkSwapchainMetalFXSpatialCreateInfoMVK;

#ifdef __cplusplus
}
#endif

#endif
