// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/flatland/engine/display_compositor.h"

#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/trace/event.h>
#include <zircon/pixelformat.h>
#include <zircon/status.h>

#include <cstdint>
#include <vector>

#include "fuchsia/hardware/display/cpp/fidl.h"
#include "src/ui/lib/escher/util/trace_macros.h"
#include "src/ui/scenic/lib/flatland/buffers/util.h"
#include "src/ui/scenic/lib/flatland/global_image_data.h"
#include "src/ui/scenic/lib/utils/helpers.h"

namespace flatland {

namespace {

using fhd_Transform = fuchsia::hardware::display::Transform;

// Debugging color used to highlight images that have gone through the GPU rendering path.
const std::array<float, 4> kGpuRenderingDebugColor = {0.9f, 0.5f, 0.5f, 1.f};

// TODO(fxbug.dev/71410): Remove all references to zx_pixel_format_t.
fuchsia::sysmem::PixelFormatType ConvertZirconFormatToSysmemFormat(const zx_pixel_format_t format) {
  switch (format) {
    // These two Zircon formats correspond to the Sysmem BGRA32 format.
    case ZX_PIXEL_FORMAT_RGB_x888:
    case ZX_PIXEL_FORMAT_ARGB_8888:
      return fuchsia::sysmem::PixelFormatType::BGRA32;
    case ZX_PIXEL_FORMAT_BGR_888x:
    case ZX_PIXEL_FORMAT_ABGR_8888:
      return fuchsia::sysmem::PixelFormatType::R8G8B8A8;
    case ZX_PIXEL_FORMAT_NV12:
      return fuchsia::sysmem::PixelFormatType::NV12;
    case ZX_PIXEL_FORMAT_I420:
      return fuchsia::sysmem::PixelFormatType::I420;
  }
  FX_CHECK(false) << "Unsupported Zircon pixel format: " << format;
  return fuchsia::sysmem::PixelFormatType::INVALID;
}

// Returns a zircon format for buffer with this pixel format.
// TODO(fxbug.dev/71410): Remove all references to zx_pixel_format_t.
zx_pixel_format_t BufferCollectionPixelFormatToZirconFormat(
    const fuchsia::sysmem::PixelFormat& pixel_format) {
  switch (pixel_format.type) {
    case fuchsia::sysmem::PixelFormatType::BGRA32:
      return ZX_PIXEL_FORMAT_ARGB_8888;
    case fuchsia::sysmem::PixelFormatType::R8G8B8A8:
      return ZX_PIXEL_FORMAT_ABGR_8888;
    case fuchsia::sysmem::PixelFormatType::NV12:
      return ZX_PIXEL_FORMAT_NV12;
    case fuchsia::sysmem::PixelFormatType::I420:
      return ZX_PIXEL_FORMAT_I420;
    default:
      break;
  }
  FX_CHECK(false) << "Unsupported pixel format: " << static_cast<uint32_t>(pixel_format.type);
  return ZX_PIXEL_FORMAT_NONE;
}

// Returns an image type that describes the tiling format used for buffer with
// this pixel format. The values are display driver specific and not documented
// in display-controller.fidl.
// TODO(fxbug.dev/33334): Remove this when image type is removed from the display
// controller API.
uint32_t BufferCollectionPixelFormatToImageType(const fuchsia::sysmem::PixelFormat& pixel_format) {
  if (pixel_format.has_format_modifier) {
    switch (pixel_format.format_modifier.value) {
      case fuchsia::sysmem::FORMAT_MODIFIER_INTEL_I915_X_TILED:
        return 1;  // IMAGE_TYPE_X_TILED
      case fuchsia::sysmem::FORMAT_MODIFIER_INTEL_I915_Y_TILED:
        return 2;  // IMAGE_TYPE_Y_LEGACY_TILED
      case fuchsia::sysmem::FORMAT_MODIFIER_INTEL_I915_YF_TILED:
        return 3;  // IMAGE_TYPE_YF_TILED
    }
  }
  return fuchsia::hardware::display::TYPE_SIMPLE;
}

fuchsia::hardware::display::AlphaMode GetAlphaMode(
    const fuchsia::ui::composition::BlendMode& blend_mode) {
  fuchsia::hardware::display::AlphaMode alpha_mode;
  switch (blend_mode) {
    case fuchsia::ui::composition::BlendMode::SRC:
      alpha_mode = fuchsia::hardware::display::AlphaMode::DISABLE;
      break;
    case fuchsia::ui::composition::BlendMode::SRC_OVER:
      alpha_mode = fuchsia::hardware::display::AlphaMode::PREMULTIPLIED;
      break;
  }
  return alpha_mode;
}

// Creates a duplicate of |token| in |duplicate|.
// Returns an error string if it fails, otherwise std::nullopt.
std::optional<std::string> DuplicateToken(
    fuchsia::sysmem::BufferCollectionTokenSyncPtr& token,
    fuchsia::sysmem::BufferCollectionTokenSyncPtr& duplicate) {
  std::vector<fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken>> dup_tokens;
  if (const auto status = token->DuplicateSync({ZX_RIGHT_SAME_RIGHTS}, &dup_tokens);
      status != ZX_OK) {
    return std::string("Could not duplicate token: ") + zx_status_get_string(status);
  }
  FX_DCHECK(dup_tokens.size() == 1);
  duplicate = dup_tokens.front().BindSync();
  return std::nullopt;
}

// Consumes |token| and returns a new AttachToken.
// Returns std::nullopt on failure.
std::optional<fuchsia::sysmem::BufferCollectionTokenSyncPtr> ConvertToAttachToken(
    fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
    fuchsia::sysmem::BufferCollectionTokenSyncPtr token) {
  fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection_sync_ptr;
  sysmem_allocator->BindSharedCollection(std::move(token), buffer_collection_sync_ptr.NewRequest());
  if (const auto status = buffer_collection_sync_ptr->Sync(); status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not sync token: " << zx_status_get_string(status);
    return std::nullopt;
  }

  fuchsia::sysmem::BufferCollectionTokenSyncPtr attach_token;
  if (const auto status =
          buffer_collection_sync_ptr->AttachToken(ZX_RIGHT_SAME_RIGHTS, attach_token.NewRequest());
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not create AttachToken: " << zx_status_get_string(status);
    return std::nullopt;
  }
  if (const auto status = buffer_collection_sync_ptr->Close(); status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not close token: " << zx_status_get_string(status);
    return std::nullopt;
  }

  return attach_token;
}

// Returns a BufferCollectionSyncPtr duplicate of |token| with empty constraints set.
// Since it has the same failure domain as |token|, it can be used to check the status of
// allocations made from that collection.
std::optional<fuchsia::sysmem::BufferCollectionSyncPtr>
CreateBufferCollectionPtrWithEmptyConstraints(
    fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
    fuchsia::sysmem::BufferCollectionTokenSyncPtr& token) {
  fuchsia::sysmem::BufferCollectionTokenSyncPtr token_dup;
  if (auto error = DuplicateToken(token, token_dup)) {
    FX_LOGS(ERROR) << *error;
    return std::nullopt;
  }

  fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection;
  sysmem_allocator->BindSharedCollection(std::move(token_dup), buffer_collection.NewRequest());
  if (const auto status =
          buffer_collection->SetConstraints(false, fuchsia::sysmem::BufferCollectionConstraints{});
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not set constraints: " << zx_status_get_string(status);
    return std::nullopt;
  }

  return buffer_collection;
}

// Returns whether |metadata| describes a valid image.
bool IsValidBufferImage(const allocation::ImageMetadata& metadata) {
  if (metadata.identifier == 0) {
    FX_LOGS(ERROR) << "ImageMetadata identifier is invalid.";
    return false;
  }

  if (metadata.collection_id == allocation::kInvalidId) {
    FX_LOGS(ERROR) << "ImageMetadata collection ID is invalid.";
    return false;
  }

  if (metadata.width == 0 || metadata.height == 0) {
    FX_LOGS(ERROR) << "ImageMetadata has a null dimension: "
                   << "(" << metadata.width << ", " << metadata.height << ").";
    return false;
  }

  return true;
}

// Calls CheckBuffersAllocated |token| and returns whether the allocation succeeded.
bool CheckBuffersAllocated(fuchsia::sysmem::BufferCollectionSyncPtr& token) {
  zx_status_t allocation_status = ZX_OK;
  const auto check_status = token->CheckBuffersAllocated(&allocation_status);
  return check_status == ZX_OK && allocation_status == ZX_OK;
}

// Calls WaitForBuffersAllocated() on |token| and returns the pixel format of the allocation.
// |token| must have already checked that buffers are allocated.
// TODO(fxbug.dev/71344): Delete after we don't need the pixel format anymore.
fuchsia::sysmem::PixelFormat GetPixelFormat(fuchsia::sysmem::BufferCollectionSyncPtr& token) {
  zx_status_t allocation_status = ZX_OK;
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info = {};
  const auto wait_status =
      token->WaitForBuffersAllocated(&allocation_status, &buffer_collection_info);
  FX_DCHECK(wait_status == ZX_OK && allocation_status == ZX_OK)
      << "WaitForBuffersAllocated failed: " << wait_status << ":" << allocation_status;
  return buffer_collection_info.settings.image_format_constraints.pixel_format;
}

// TODO(fxbug.dev/85601): Remove after YUV buffers can be imported to display. We filter YUV
// images out of display path.
bool IsYUV(const fuchsia::sysmem::PixelFormat& format) {
  return format.type == fuchsia::sysmem::PixelFormatType::NV12 ||
         format.type == fuchsia::sysmem::PixelFormatType::I420;
}

// Returns whether |token| is compatible with the display and, if it is, its pixel format.
// It is possible for the image to be supported by the display, but for the pixel format fetch to
// fail.
// TODO(fxbug.dev/71344): Just return a bool after we don't need the pixel format anymore.
std::optional<fuchsia::sysmem::PixelFormat> DetermineDisplaySupportFor(
    fuchsia::sysmem::BufferCollectionSyncPtr token) {
  const bool image_supports_display = CheckBuffersAllocated(token);
  if (!image_supports_display) {
    return std::nullopt;
  }

  const fuchsia::sysmem::PixelFormat pixel_format = GetPixelFormat(token);

  token->Close();

  // TODO(fxbug.dev/85601): Remove after YUV buffers can be imported to display. We filter YUV
  // images out of display path.
  if (IsYUV(pixel_format)) {
    return std::nullopt;
  }

  return pixel_format;
}

}  // anonymous namespace

DisplayCompositor::DisplayCompositor(
    async_dispatcher_t* dispatcher,
    std::shared_ptr<fuchsia::hardware::display::ControllerSyncPtr> display_controller,
    const std::shared_ptr<Renderer>& renderer, fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator,
    BufferCollectionImportMode import_mode)
    : display_controller_(std::move(display_controller)),
      renderer_(renderer),
      release_fence_manager_(dispatcher),
      sysmem_allocator_(std::move(sysmem_allocator)),
      import_mode_(import_mode) {
  FX_DCHECK(dispatcher);
  FX_DCHECK(renderer_);
  FX_DCHECK(sysmem_allocator_);
}

DisplayCompositor::~DisplayCompositor() {
  // Destroy all of the display layers.
  DiscardConfig();
  for (const auto& [_, data] : display_engine_data_map_) {
    for (const auto& layer : data.layers) {
      (*display_controller_)->DestroyLayer(layer);
    }
    for (const auto& event_data : data.frame_event_datas) {
      (*display_controller_)->ReleaseEvent(event_data.wait_id);
      (*display_controller_)->ReleaseEvent(event_data.signal_id);
    }
  }

  // TODO(fxbug.dev/112156): Release |render_targets| and |protected_render_targets| collections and
  // images.
}

bool DisplayCompositor::ImportBufferCollection(
    const allocation::GlobalBufferCollectionId collection_id,
    fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
    fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
    const BufferCollectionUsage usage, const std::optional<fuchsia::math::SizeU> size) {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::ImportBufferCollection");
  FX_DCHECK(display_controller_);
  // Expect the default Buffer Collection usage type.
  FX_DCHECK(usage == BufferCollectionUsage::kClientImage);

  auto renderer_token = token.BindSync();

  // Create a token for the display controller to set constraints on.
  fuchsia::sysmem::BufferCollectionTokenSyncPtr display_token;
  if (auto error = DuplicateToken(renderer_token, display_token)) {
    FX_LOGS(ERROR) << *error;
    return false;
  }

  // Set renderer constraints.
  if (!renderer_->ImportBufferCollection(collection_id, sysmem_allocator, std::move(renderer_token),
                                         usage, size)) {
    FX_LOGS(INFO) << "Renderer could not import buffer collection.";
    return false;
  }

  switch (import_mode_) {
    case BufferCollectionImportMode::RendererOnly:
      // Fall back to using the renderer. Don't attempt direct-to-display and don't keep any
      // references.
      if (const auto status = display_token->Close(); status != ZX_OK) {
        FX_LOGS(ERROR) << "Could not close token: " << zx_status_get_string(status);
      }
      return true;
    case BufferCollectionImportMode::EnforceDisplayConstraints:
      // Continue to use |display_token| as is. Allocation will fail if the display constraints are
      // incompatible.
      break;
    case BufferCollectionImportMode::AttemptDisplayConstraints:
      // Replace |display_token| with an AttachToken. In this mode we get direct-to-display in case
      // the display "just happens" to be happy with what the client and renderer agreed on.
      // TODO(fxbug.dev/74423): Replace with prunable token when it is available.
      if (auto attach_token = ConvertToAttachToken(sysmem_allocator, std::move(display_token))) {
        display_token = std::move(*attach_token);
      } else {
        return false;
      }
      break;
  }

  // Create a BufferCollectionPtr from a duplicate of |display_token| with which to later check if
  // buffers allocated from the BufferCollection are display-compatible.
  if (auto collection_ptr =
          CreateBufferCollectionPtrWithEmptyConstraints(sysmem_allocator, display_token)) {
    display_buffer_collection_ptrs_[collection_id] = std::move(*collection_ptr);
  } else {
    return false;
  }

  std::scoped_lock lock(lock_);  // Lock |display_controller_|.
  // Import the buffer collection into the display controller, setting display constraints.
  return scenic_impl::ImportBufferCollection(
      collection_id, *display_controller_, std::move(display_token),
      // Indicate that no specific size, format, or type is required.
      fuchsia::hardware::display::ImageConfig{.pixel_format = ZX_PIXEL_FORMAT_NONE, .type = 0});
}

void DisplayCompositor::ReleaseBufferCollection(
    const allocation::GlobalBufferCollectionId collection_id, const BufferCollectionUsage usage) {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::ReleaseBufferCollection");
  FX_DCHECK(usage == BufferCollectionUsage::kClientImage);
  std::scoped_lock lock(lock_);
  FX_DCHECK(display_controller_);
  (*display_controller_)->ReleaseBufferCollection(collection_id);
  renderer_->ReleaseBufferCollection(collection_id, usage);
  display_buffer_collection_ptrs_.erase(collection_id);
  buffer_collection_supports_display_.erase(collection_id);
}

fuchsia::sysmem::BufferCollectionSyncPtr DisplayCompositor::TakeDisplayBufferCollectionPtr(
    const allocation::GlobalBufferCollectionId collection_id) {
  const auto token_it = display_buffer_collection_ptrs_.find(collection_id);
  FX_DCHECK(token_it != display_buffer_collection_ptrs_.end());
  auto token = std::move(token_it->second);
  display_buffer_collection_ptrs_.erase(token_it);
  return token;
}

fuchsia::hardware::display::ImageConfig DisplayCompositor::CreateImageConfig(
    const allocation::ImageMetadata& metadata) const {
  FX_DCHECK(buffer_collection_pixel_format_.count(metadata.collection_id));
  const auto pixel_format = buffer_collection_pixel_format_.at(metadata.collection_id);
  return fuchsia::hardware::display::ImageConfig{
      .width = metadata.width,
      .height = metadata.height,
      .pixel_format = BufferCollectionPixelFormatToZirconFormat(pixel_format),
      .type = BufferCollectionPixelFormatToImageType(pixel_format)};
}

bool DisplayCompositor::ImportBufferImage(const allocation::ImageMetadata& metadata,
                                          const BufferCollectionUsage usage) {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::ImportBufferImage");
  FX_DCHECK(display_controller_);

  if (!IsValidBufferImage(metadata)) {
    return false;
  }

  if (!renderer_->ImportBufferImage(metadata, usage)) {
    FX_LOGS(ERROR) << "Renderer could not import image.";
    return false;
  }

  const allocation::GlobalBufferCollectionId collection_id = metadata.collection_id;
  const bool display_support_already_set =
      buffer_collection_supports_display_.find(collection_id) !=
      buffer_collection_supports_display_.end();

  // In RendererOnly mode, the only images that should be imported by the display is the
  // framebuffer, and their display support is already set in AddDisplay() (instead of below).
  // For every other image in RendererOnly mode we can early exit.
  if (import_mode_ == BufferCollectionImportMode::RendererOnly &&
      (!display_support_already_set || !buffer_collection_supports_display_[collection_id])) {
    buffer_collection_supports_display_[collection_id] = false;
    return true;
  }

  if (!display_support_already_set) {
    const auto pixel_format =
        DetermineDisplaySupportFor(TakeDisplayBufferCollectionPtr(collection_id));
    buffer_collection_supports_display_[collection_id] = pixel_format.has_value();
    if (pixel_format.has_value()) {
      buffer_collection_pixel_format_[collection_id] = pixel_format.value();
    }
  }

  if (!buffer_collection_supports_display_[collection_id]) {
    // When display isn't supported, so depending on mode we:
    switch (import_mode_) {
      case BufferCollectionImportMode::AttemptDisplayConstraints:
        // Fallback to renderer and continue.
        return true;
      case BufferCollectionImportMode::EnforceDisplayConstraints:
        // Fail outright.
        return false;
      default:
        FX_NOTREACHED() << "Should have been handled above";
        return false;
    }
  }

  const fuchsia::hardware::display::ImageConfig image_config = CreateImageConfig(metadata);
  zx_status_t import_image_status = ZX_OK;
  {
    std::scoped_lock lock(lock_);  // Lock |display_controller_|.
    const auto status = (*display_controller_)
                            ->ImportImage2(image_config, collection_id, metadata.identifier,
                                           metadata.vmo_index, &import_image_status);
    FX_DCHECK(status == ZX_OK);
  }

  if (import_image_status != ZX_OK) {
    FX_LOGS(ERROR) << "Display controller could not import the image.";
    return false;
  }

  return true;
}

void DisplayCompositor::ReleaseBufferImage(const allocation::GlobalImageId image_id) {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::ReleaseBufferImage");

  // Locks the rest of the function.
  std::scoped_lock lock(lock_);
  FX_DCHECK(display_controller_);
  (*display_controller_)->ReleaseImage(image_id);

  // Release image from the renderer.
  renderer_->ReleaseBufferImage(image_id);

  image_event_map_.erase(image_id);
}

uint64_t DisplayCompositor::CreateDisplayLayer() {
  std::scoped_lock lock(lock_);
  uint64_t layer_id;
  zx_status_t create_layer_status;
  const zx_status_t transport_status =
      (*display_controller_)->CreateLayer(&create_layer_status, &layer_id);
  if (create_layer_status != ZX_OK || transport_status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to create layer, " << create_layer_status;
    return 0;
  }
  return layer_id;
}

void DisplayCompositor::SetDisplayLayers(const uint64_t display_id,
                                         const std::vector<uint64_t>& layers) {
  // Set all of the layers for each of the images on the display.
  std::scoped_lock lock(lock_);
  const auto status = (*display_controller_)->SetDisplayLayers(display_id, layers);
  FX_DCHECK(status == ZX_OK);
}

bool DisplayCompositor::SetRenderDataOnDisplay(const RenderData& data) {
  // Every rectangle should have an associated image.
  const uint32_t num_images = static_cast<uint32_t>(data.images.size());

  // Since we map 1 image to 1 layer, if there are more images than layers available for
  // the given display, then they cannot be directly composited to the display in hardware.
  std::vector<uint64_t> layers;
  {
    std::scoped_lock lock(lock_);
    layers = display_engine_data_map_.at(data.display_id).layers;
  }
  if (layers.size() < num_images) {
    return false;
  }

  for (uint32_t i = 0; i < num_images; i++) {
    const allocation::GlobalImageId image_id = data.images[i].identifier;
    if (image_event_map_.find(image_id) == image_event_map_.end()) {
      image_event_map_[image_id] = NewImageEventData();
    } else {
      // If the event is not signaled, image must still be in use by the display and cannot be used
      // again.
      const auto status =
          image_event_map_[image_id].signal_event.wait_one(ZX_EVENT_SIGNALED, zx::time(), nullptr);
      if (status != ZX_OK) {
        return false;
      }
    }
    pending_images_in_config_.push_back(image_id);
  }

  // We only set as many layers as needed for the images we have.
  SetDisplayLayers(data.display_id,
                   std::vector<uint64_t>(layers.begin(), layers.begin() + num_images));

  for (uint32_t i = 0; i < num_images; i++) {
    const allocation::GlobalImageId image_id = data.images[i].identifier;
    if (image_id != allocation::kInvalidImageId) {
      if (buffer_collection_supports_display_[data.images[i].collection_id]) {
        ApplyLayerImage(layers[i], data.rectangles[i], data.images[i], /*wait_id*/ 0,
                        /*signal_id*/ image_event_map_[image_id].signal_id);
      } else {
        return false;
      }
    } else {
      // TODO(fxbug.dev/104887): Not all display hardware is able to handle color layers with
      // specific sizes, which is required for doing solid-fill rects on the display path.
      // If we encounter one of those rects here -- unless it is the backmost layer and fullscreen
      // -- then we abort.
      const auto& rect = data.rectangles[i];
      const auto& display_size = display_info_map_[data.display_id].dimensions;
      if (i == 0 && rect.origin.x == 0 && rect.origin.y == 0 &&
          rect.extent.x == static_cast<float>(display_size.x) &&
          rect.extent.y == static_cast<float>(display_size.y)) {
        ApplyLayerColor(layers[i], rect, data.images[i]);
      } else {
        return false;
      }
    }
  }
  return true;
}

void DisplayCompositor::ApplyLayerColor(const uint64_t layer_id, const ImageRect rectangle,
                                        const allocation::ImageMetadata image) {
  std::scoped_lock lock(lock_);

  // We have to convert the image_metadata's multiply color, which is an array of normalized
  // floating point values, to an unnormalized array of uint8_ts in the range 0-255.
  std::vector<uint8_t> col = {static_cast<uint8_t>(255 * image.multiply_color[0]),
                              static_cast<uint8_t>(255 * image.multiply_color[1]),
                              static_cast<uint8_t>(255 * image.multiply_color[2]),
                              static_cast<uint8_t>(255 * image.multiply_color[3])};

  (*display_controller_)->SetLayerColorConfig(layer_id, ZX_PIXEL_FORMAT_ARGB_8888, col);

// TODO(fxbug.dev/104887): Currently, not all display hardware supports the ability to
// set either the position or the alpha on a color layer, as color layers are not primary
// layers. There exist hardware that require a color layer to be the backmost layer and to be
// the size of the entire display. This means that for the time being, we must rely on GPU
// composition for solid color rects.
//
// There is the option of assigning a 1x1 image with the desired color to a standard image layer,
// as a way of mimicking color layers (and this is what is done in the GPU path as well) --
// however, not all hardware supports images with sizes that differ from the destination size of
// the rect. So implementing that solution on the display path as well is problematic.
#if 0

  const auto [src, dst] = DisplaySrcDstFrames::New(rectangle, image);

  const fhd_Transform transform =
      GetDisplayTransformFromOrientationAndFlip(rectangle.orientation, image.flip);

  (*display_controller_)->SetLayerPrimaryPosition(layer_id, transform, src, dst);
  auto alpha_mode = GetAlphaMode(image.blend_mode);
  (*display_controller_)->SetLayerPrimaryAlpha(layer_id, alpha_mode, image.multiply_color[3]);
#endif
}

void DisplayCompositor::ApplyLayerImage(const uint64_t layer_id, const ImageRect rectangle,
                                        const allocation::ImageMetadata image,
                                        const scenic_impl::DisplayEventId wait_id,
                                        const scenic_impl::DisplayEventId signal_id) {
  const auto [src, dst] = DisplaySrcDstFrames::New(rectangle, image);
  FX_DCHECK(src.width && src.height) << "Source frame cannot be empty.";
  FX_DCHECK(dst.width && dst.height) << "Destination frame cannot be empty.";
  const fhd_Transform transform =
      GetDisplayTransformFromOrientationAndFlip(rectangle.orientation, image.flip);
  const auto alpha_mode = GetAlphaMode(image.blend_mode);

  std::scoped_lock lock(lock_);
  // TODO(fxbug.dev/71344): Pixel format should be ignored when using sysmem. We do not want to have
  // to deal with this default image format.
  const auto pixel_format = buffer_collection_pixel_format_.at(image.collection_id);
  const fuchsia::hardware::display::ImageConfig image_config = CreateImageConfig(image);
  (*display_controller_)->SetLayerPrimaryConfig(layer_id, image_config);
  (*display_controller_)->SetLayerPrimaryPosition(layer_id, transform, src, dst);
  (*display_controller_)->SetLayerPrimaryAlpha(layer_id, alpha_mode, image.multiply_color[3]);
  // Set the imported image on the layer.
  (*display_controller_)->SetLayerImage(layer_id, image.identifier, wait_id, signal_id);
}

bool DisplayCompositor::CheckConfig() {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::CheckConfig");
  fuchsia::hardware::display::ConfigResult result;
  std::vector<fuchsia::hardware::display::ClientCompositionOp> ops;
  std::scoped_lock lock(lock_);
  (*display_controller_)->CheckConfig(/*discard*/ false, &result, &ops);
  return result == fuchsia::hardware::display::ConfigResult::OK;
}

void DisplayCompositor::DiscardConfig() {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::DiscardConfig");
  pending_images_in_config_.clear();
  fuchsia::hardware::display::ConfigResult result;
  std::vector<fuchsia::hardware::display::ClientCompositionOp> ops;
  std::scoped_lock lock(lock_);
  (*display_controller_)->CheckConfig(/*discard*/ true, &result, &ops);
}

fuchsia::hardware::display::ConfigStamp DisplayCompositor::ApplyConfig() {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::ApplyConfig");
  std::scoped_lock lock(lock_);
  {
    const auto status = (*display_controller_)->ApplyConfig();
    FX_DCHECK(status == ZX_OK);
  }
  fuchsia::hardware::display::ConfigStamp pending_config_stamp;
  {
    const auto status = (*display_controller_)->GetLatestAppliedConfigStamp(&pending_config_stamp);
    FX_DCHECK(status == ZX_OK);
  }
  return pending_config_stamp;
}

bool DisplayCompositor::PerformGpuComposition(
    const uint64_t frame_number, const zx::time presentation_time,
    const std::vector<RenderData>& render_data_list, std::vector<zx::event> release_fences,
    scheduling::FrameRenderer::FramePresentedCallback callback) {
  // Create an event that will be signaled when the final display's content has finished
  // rendering; it will be passed into |release_fence_manager_.OnGpuCompositedFrame()|.  If there
  // are multiple displays which require GPU-composited content, we pass this event to be signaled
  // when the final display's content has finished rendering (thus guaranteeing that all previous
  // content has also finished rendering).
  // TODO(fxbug.dev/77640): we might want to reuse events, instead of creating a new one every
  // frame.
  zx::event render_finished_fence = utils::CreateEvent();

  for (size_t i = 0; i < render_data_list.size(); ++i) {
    const bool is_final_display = i == (render_data_list.size() - 1);
    const auto& render_data = render_data_list[i];
    const auto display_engine_data_it = display_engine_data_map_.find(render_data.display_id);
    FX_DCHECK(display_engine_data_it != display_engine_data_map_.end());
    auto& display_engine_data = display_engine_data_it->second;

    // Clear any past CC state here, before applying GPU CC.
    if (cc_state_machine_.GpuRequiresDisplayClearing()) {
      const zx_status_t status =
          (*display_controller_)
              ->SetDisplayColorConversion(render_data.display_id, kDefaultColorConversionOffsets,
                                          kDefaultColorConversionCoefficients,
                                          kDefaultColorConversionOffsets);
      FX_CHECK(status == ZX_OK) << "Could not apply hardware color conversion: " << status;
      cc_state_machine_.DisplayCleared();
    }

    if (display_engine_data.vmo_count == 0) {
      FX_LOGS(WARNING) << "No VMOs were created when creating display.";
      return false;
    }
    const uint32_t curr_vmo = display_engine_data.curr_vmo;
    display_engine_data.curr_vmo =
        (display_engine_data.curr_vmo + 1) % display_engine_data.vmo_count;
    const auto& render_targets = renderer_->RequiresRenderInProtected(render_data.images)
                                     ? display_engine_data.protected_render_targets
                                     : display_engine_data.render_targets;
    FX_DCHECK(curr_vmo < render_targets.size()) << curr_vmo << "/" << render_targets.size();
    FX_DCHECK(curr_vmo < display_engine_data.frame_event_datas.size())
        << curr_vmo << "/" << display_engine_data.frame_event_datas.size();
    const auto& render_target = render_targets[curr_vmo];

    // Reset the event data.
    auto& event_data = display_engine_data.frame_event_datas[curr_vmo];

    // TODO(fxbug.dev/91737): Remove this after the direct-to-display path is stable.
    // We expect the retired event to already have been signaled. Verify this without waiting.
    {
      const zx_status_t status =
          event_data.signal_event.wait_one(ZX_EVENT_SIGNALED, zx::time(), nullptr);
      if (status != ZX_OK) {
        FX_DCHECK(status == ZX_ERR_TIMED_OUT) << "unexpected status: " << status;
        FX_LOGS(ERROR)
            << "flatland::DisplayCompositor::RenderFrame rendering into in-use backbuffer";
      }
    }

    event_data.wait_event.signal(ZX_EVENT_SIGNALED, 0);
    event_data.signal_event.signal(ZX_EVENT_SIGNALED, 0);

    // Apply the debugging color to the images.
#ifdef VISUAL_DEBUGGING_ENABLED
    auto images = render_data.images;
    for (auto& image : images) {
      image.multiply_color[0] *= kDebugColor[0];
      image.multiply_color[1] *= kDebugColor[1];
      image.multiply_color[2] *= kDebugColor[2];
      image.multiply_color[3] *= kDebugColor[3];
    }
#else
    auto& images = render_data.images;
#endif  // VISUAL_DEBUGGING_ENABLED

    const auto apply_cc = (cc_state_machine_.GetDataToApply() != std::nullopt);
    std::vector<zx::event> render_fences;
    render_fences.push_back(std::move(event_data.wait_event));
    // Only add render_finished_fence if we're rendering the final display's framebuffer.
    if (is_final_display) {
      render_fences.push_back(std::move(render_finished_fence));
      renderer_->Render(render_target, render_data.rectangles, images, render_fences, apply_cc);
      // Retrieve fence.
      render_finished_fence = std::move(render_fences.back());
    } else {
      renderer_->Render(render_target, render_data.rectangles, images, render_fences, apply_cc);
    }

    // Retrieve fence.
    event_data.wait_event = std::move(render_fences[0]);

    const auto layer = display_engine_data.layers[0];
    SetDisplayLayers(render_data.display_id, {layer});
    ApplyLayerImage(layer, {glm::vec2(0), glm::vec2(render_target.width, render_target.height)},
                    render_target, event_data.wait_id, event_data.signal_id);

    if (!CheckConfig()) {
      FX_LOGS(ERROR) << "Both display hardware composition and GPU rendering have failed.";
      // TODO(fxbug.dev/59646): Figure out how we really want to handle this case here.
      return false;
    }
  }

  // See ReleaseFenceManager comments for details.
  FX_DCHECK(render_finished_fence);
  release_fence_manager_.OnGpuCompositedFrame(frame_number, std::move(render_finished_fence),
                                              std::move(release_fences), std::move(callback));
  return true;
}

void DisplayCompositor::RenderFrame(const uint64_t frame_number, const zx::time presentation_time,
                                    const std::vector<RenderData>& render_data_list,
                                    std::vector<zx::event> release_fences,
                                    scheduling::FrameRenderer::FramePresentedCallback callback) {
  TRACE_DURATION("gfx", "flatland::DisplayCompositor::RenderFrame");
  TRACE_FLOW_STEP("gfx", "scenic_frame", frame_number);

  // Config should be reset before doing anything new.
  DiscardConfig();
  const bool hardware_failure = !SetRenderDatasOnDisplay(render_data_list);

  // Determine whether we need to fall back to GPU composition. Avoid calling CheckConfig() if we
  // don't need to, because this requires a round-trip to the display controller.
  const bool fallback_to_gpu_composition =
      hardware_failure || kDisableDisplayComposition || !CheckConfig();

  if (fallback_to_gpu_composition) {
    DiscardConfig();
    if (!PerformGpuComposition(frame_number, presentation_time, render_data_list,
                               std::move(release_fences), std::move(callback))) {
      return;
    }
  } else {
    // CC was successfully applied to the config so we update the state machine.
    cc_state_machine_.SetApplyConfigSucceeded();

    // Unsignal image events before applying config.
    for (auto id : pending_images_in_config_) {
      image_event_map_[id].signal_event.signal(ZX_EVENT_SIGNALED, 0);
    }

    // See ReleaseFenceManager comments for details.
    release_fence_manager_.OnDirectScanoutFrame(frame_number, std::move(release_fences),
                                                std::move(callback));
  }

  // TODO(fxbug.dev/77414): we should be calling ApplyConfig2() here, but it's not implemented yet.
  // Additionally, if the previous frame was "direct scanout" (but not if "gpu composited") we
  // should obtain the fences for that frame and pass them directly to ApplyConfig2().
  // ReleaseFenceManager is somewhat poorly suited to this, because it was designed for an old
  // version of ApplyConfig2(), which latter proved to be infeasible for some drivers to implement.
  const auto& config_stamp = ApplyConfig();
  pending_apply_configs_.push_back({.config_stamp = config_stamp, .frame_number = frame_number});
}

bool DisplayCompositor::SetRenderDatasOnDisplay(const std::vector<RenderData>& render_data_list) {
  if (kDisableDisplayComposition) {
    return false;
  }

  for (const auto& data : render_data_list) {
    if (!SetRenderDataOnDisplay(data)) {
      // TODO(fxbug.dev/77416): just because setting the data on one display fails (e.g. due to
      // too many layers), that doesn't mean that all displays need to use GPU-composition.  Some
      // day we might want to use GPU-composition for some client images, and direct-scanout for
      // others.
      return false;
    }

    // Check the state machine to see if there's any CC data to apply.
    if (const auto cc_data = cc_state_machine_.GetDataToApply()) {
      // Apply direct-to-display color conversion here.
      const zx_status_t status =
          (*display_controller_)
              ->SetDisplayColorConversion(data.display_id, (*cc_data).preoffsets,
                                          (*cc_data).coefficients, (*cc_data).postoffsets);
      FX_CHECK(status == ZX_OK) << "Could not apply hardware color conversion: " << status;
    }
  }

  return true;
}

void DisplayCompositor::OnVsync(zx::time timestamp,
                                fuchsia::hardware::display::ConfigStamp applied_config_stamp) {
  TRACE_DURATION("gfx", "Flatland::DisplayCompositor::OnVsync");

  // We might receive multiple OnVsync() callbacks with the same |applied_config_stamp| if the scene
  // doesn't change. Early exit for these cases.
  if (last_presented_config_stamp_.has_value() &&
      fidl::Equals(applied_config_stamp, last_presented_config_stamp_.value())) {
    return;
  }

  // Verify that the configuration from Vsync is in the [pending_apply_configs_] queue.
  const auto vsync_frame_it =
      std::find_if(pending_apply_configs_.begin(), pending_apply_configs_.end(),
                   [applied_config_stamp](const ApplyConfigInfo& info) {
                     return fidl::Equals(info.config_stamp, applied_config_stamp);
                   });

  // It is possible that the config stamp doesn't match any config applied by this DisplayCompositor
  // instance. i.e. it could be from another client. Thus we just ignore these events.
  if (vsync_frame_it == pending_apply_configs_.end()) {
    FX_LOGS(INFO) << "The config stamp <" << applied_config_stamp.value << "> was not generated "
                  << "by current DisplayCompositor. Vsync event skipped.";
    return;
  }

  // Handle the presented ApplyConfig() call, as well as the skipped ones.
  auto it = pending_apply_configs_.begin();
  auto end = std::next(vsync_frame_it);
  while (it != end) {
    release_fence_manager_.OnVsync(it->frame_number, timestamp);
    it = pending_apply_configs_.erase(it);
  }
  last_presented_config_stamp_ = applied_config_stamp;
}

DisplayCompositor::FrameEventData DisplayCompositor::NewFrameEventData() {
  FrameEventData result;
  {  // The DC waits on this to be signaled by the renderer.
    const auto status = zx::event::create(0, &result.wait_event);
    FX_DCHECK(status == ZX_OK);
  }
  {  // The DC signals this once it has set the layer image.  We pre-signal this event so the first
    // frame rendered with it behaves as though it was previously OKed for recycling.
    const auto status = zx::event::create(0, &result.signal_event);
    FX_DCHECK(status == ZX_OK);
  }

  std::scoped_lock lock(lock_);
  result.wait_id = scenic_impl::ImportEvent(*display_controller_, result.wait_event);
  FX_DCHECK(result.wait_id != fuchsia::hardware::display::INVALID_DISP_ID);
  result.signal_event.signal(0, ZX_EVENT_SIGNALED);
  result.signal_id = scenic_impl::ImportEvent(*display_controller_, result.signal_event);
  FX_DCHECK(result.signal_id != fuchsia::hardware::display::INVALID_DISP_ID);
  return result;
}

DisplayCompositor::ImageEventData DisplayCompositor::NewImageEventData() {
  ImageEventData result;
  // The DC signals this once it has set the layer image.  We pre-signal this event so the first
  // frame rendered with it behaves as though it was previously OKed for recycling.
  {
    const auto status = zx::event::create(0, &result.signal_event);
    FX_DCHECK(status == ZX_OK);
  }
  {
    const auto status = result.signal_event.signal(0, ZX_EVENT_SIGNALED);
    FX_DCHECK(status == ZX_OK);
  }

  std::scoped_lock lock(lock_);
  result.signal_id = scenic_impl::ImportEvent(*display_controller_, result.signal_event);
  FX_DCHECK(result.signal_id != fuchsia::hardware::display::INVALID_DISP_ID);

  return result;
}

void DisplayCompositor::AddDisplay(scenic_impl::display::Display* display, const DisplayInfo info,
                                   const uint32_t num_render_targets,
                                   fuchsia::sysmem::BufferCollectionInfo_2* out_collection_info) {
  const auto display_id = display->display_id();
  FX_DCHECK(display_engine_data_map_.find(display_id) == display_engine_data_map_.end())
      << "DisplayCompositor::AddDisplay(): display already exists: " << display_id;

  const uint32_t width = info.dimensions.x;
  const uint32_t height = info.dimensions.y;

  // Grab the best pixel format that the renderer prefers given the list of available formats on
  // the display.
  FX_DCHECK(info.formats.size());
  auto pixel_format = renderer_->ChoosePreferredPixelFormat(info.formats);

  display_info_map_[display_id] = std::move(info);
  auto& display_engine_data = display_engine_data_map_[display_id];

  // When we add in a new display, we create a couple of layers for that display upfront to be
  // used when we directly composite render data in hardware via the display controller.
  // TODO(fxbug.dev/77873): per-display layer lists are probably a bad idea; this approach doesn't
  // reflect the constraints of the underlying display hardware.
  for (uint32_t i = 0; i < 2; i++) {
    display_engine_data.layers.push_back(CreateDisplayLayer());
  }

  // Add vsync callback on display. Note that this will overwrite the existing callback on
  // |display| and other clients won't receive any, i.e. gfx.
  display->SetVsyncCallback(
      [weak_ref = weak_from_this()](zx::time timestamp,
                                    fuchsia::hardware::display::ConfigStamp applied_config_stamp) {
        if (auto ref = weak_ref.lock())
          ref->OnVsync(timestamp, applied_config_stamp);
      });

  // Exit early if there are no vmos to create.
  if (num_render_targets == 0) {
    return;
  }

  // If we are creating vmos, we need a non-null buffer collection pointer to return back
  // to the caller.
  FX_DCHECK(out_collection_info);

  display_engine_data.render_targets = AllocateDisplayRenderTargets(
      /*use_protected_memory=*/false, num_render_targets, {width, height}, pixel_format,
      out_collection_info);
  for (uint32_t i = 0; i < num_render_targets; i++) {
    display_engine_data.frame_event_datas.push_back(NewFrameEventData());
  }
  display_engine_data.vmo_count = num_render_targets;
  display_engine_data.curr_vmo = 0;

  // Create another set of tokens and allocate a protected render target. Protected memory buffer
  // pool is usually limited, so it is better for Scenic to preallocate to avoid being blocked by
  // running out of protected memory.
  if (renderer_->SupportsRenderInProtected()) {
    display_engine_data.protected_render_targets = AllocateDisplayRenderTargets(
        /*use_protected_memory=*/true, num_render_targets, {width, height}, pixel_format, nullptr);
  }
}

void DisplayCompositor::SetColorConversionValues(const std::array<float, 9>& coefficients,
                                                 const std::array<float, 3>& preoffsets,
                                                 const std::array<float, 3>& postoffsets) {
  // Lock the whole function.
  std::scoped_lock lock(lock_);

  cc_state_machine_.SetData(
      {.coefficients = coefficients, .preoffsets = preoffsets, .postoffsets = postoffsets});

  renderer_->SetColorConversionValues(coefficients, preoffsets, postoffsets);
}

bool DisplayCompositor::SetMinimumRgb(const uint8_t minimum_rgb) {
  fuchsia::hardware::display::Controller_SetMinimumRgb_Result cmd_result;
  const auto status = (*display_controller_)->SetMinimumRgb(minimum_rgb, &cmd_result);
  if (status != ZX_OK || cmd_result.is_err()) {
    FX_LOGS(WARNING) << "FlatlandDisplayCompositor SetMinimumRGB failed";
    return false;
  }
  return true;
}

std::vector<allocation::ImageMetadata> DisplayCompositor::AllocateDisplayRenderTargets(
    const bool use_protected_memory, const uint32_t num_render_targets,
    const fuchsia::math::SizeU& size, const zx_pixel_format_t pixel_format,
    fuchsia::sysmem::BufferCollectionInfo_2* out_collection_info) {
  // Create the buffer collection token to be used for frame buffers.
  fuchsia::sysmem::BufferCollectionTokenSyncPtr compositor_token;
  {
    const auto status = sysmem_allocator_->AllocateSharedCollection(compositor_token.NewRequest());
    FX_DCHECK(status == ZX_OK) << "status: " << zx_status_get_string(status);
  }

  // Duplicate the token for the display and for the renderer.
  fuchsia::sysmem::BufferCollectionTokenSyncPtr renderer_token;
  fuchsia::sysmem::BufferCollectionTokenSyncPtr display_token;
  {
    std::vector<fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken>> dup_tokens;
    const auto status =
        compositor_token->DuplicateSync({ZX_RIGHT_SAME_RIGHTS, ZX_RIGHT_SAME_RIGHTS}, &dup_tokens);
    FX_DCHECK(status == ZX_OK) << "status: " << zx_status_get_string(status);
    FX_DCHECK(dup_tokens.size() == 2);
    renderer_token = dup_tokens.at(0).BindSync();
    display_token = dup_tokens.at(1).BindSync();
  }

  // Set renderer constraints.
  const auto collection_id = allocation::GenerateUniqueBufferCollectionId();
  {
    const auto result = renderer_->ImportBufferCollection(
        collection_id, sysmem_allocator_.get(), std::move(renderer_token),
        BufferCollectionUsage::kRenderTarget, std::optional<fuchsia::math::SizeU>(size));
    FX_DCHECK(result);
  }

  {  // Set display constraints.
    const auto result = scenic_impl::ImportBufferCollection(
        collection_id, *display_controller_, std::move(display_token),
        fuchsia::hardware::display::ImageConfig{.pixel_format = pixel_format});
    FX_DCHECK(result);
  }

// Set local constraints.
#ifdef CPU_ACCESSIBLE_VMO
  const bool make_cpu_accessible = true;
#else
  const bool make_cpu_accessible = false;
#endif

  fuchsia::sysmem::BufferCollectionSyncPtr collection_ptr;
  if (make_cpu_accessible && !use_protected_memory) {
    const auto [buffer_usage, memory_constraints] = GetUsageAndMemoryConstraintsForCpuWriteOften();
    collection_ptr = CreateBufferCollectionSyncPtrAndSetConstraints(
        sysmem_allocator_.get(), std::move(compositor_token), num_render_targets, size.width,
        size.height, buffer_usage, ConvertZirconFormatToSysmemFormat(pixel_format),
        memory_constraints);
  } else {
    fuchsia::sysmem::BufferCollectionConstraints constraints;
    constraints.min_buffer_count_for_camping = num_render_targets;
    constraints.usage.none = fuchsia::sysmem::noneUsage;
    if (use_protected_memory) {
      constraints.has_buffer_memory_constraints = true;
      constraints.buffer_memory_constraints.secure_required = true;
      constraints.buffer_memory_constraints.inaccessible_domain_supported = true;
      constraints.buffer_memory_constraints.cpu_domain_supported = false;
      constraints.buffer_memory_constraints.ram_domain_supported = false;
    }

    sysmem_allocator_->BindSharedCollection(std::move(compositor_token),
                                            collection_ptr.NewRequest());
    collection_ptr->SetName(10u, use_protected_memory
                                     ? "FlatlandDisplayCompositorProtectedRenderTarget"
                                     : "FlatlandDisplayCompositorRenderTarget");
    const auto status = collection_ptr->SetConstraints(true, constraints);
    FX_DCHECK(status == ZX_OK) << "status: " << zx_status_get_string(status);
  }

  // Wait for buffers allocated so it can populate its information struct with the vmo data.
  fuchsia::sysmem::BufferCollectionInfo_2 collection_info;
  {
    zx_status_t allocation_status = ZX_OK;
    const auto status =
        collection_ptr->WaitForBuffersAllocated(&allocation_status, &collection_info);
    FX_DCHECK(status == ZX_OK) << "status: " << zx_status_get_string(status);
    FX_DCHECK(allocation_status == ZX_OK) << "status: " << zx_status_get_string(allocation_status);
  }

  {
    const auto status = collection_ptr->Close();
    FX_DCHECK(status == ZX_OK) << "status: " << zx_status_get_string(status);
  }

  // We know that this collection is supported by display because we collected constraints from
  // display in scenic_impl::ImportBufferCollection() and waited for successful allocation.
  buffer_collection_supports_display_[collection_id] = true;
  buffer_collection_pixel_format_[collection_id] =
      collection_info.settings.image_format_constraints.pixel_format;
  if (out_collection_info) {
    *out_collection_info = std::move(collection_info);
  }

  std::vector<allocation::ImageMetadata> render_targets;
  for (uint32_t i = 0; i < num_render_targets; i++) {
    const allocation::ImageMetadata target = {.collection_id = collection_id,
                                              .identifier = allocation::GenerateUniqueImageId(),
                                              .vmo_index = i,
                                              .width = size.width,
                                              .height = size.height};
    render_targets.push_back(target);
    bool res = ImportBufferImage(target, BufferCollectionUsage::kRenderTarget);
    FX_DCHECK(res);
  }
  return render_targets;
}

}  // namespace flatland
