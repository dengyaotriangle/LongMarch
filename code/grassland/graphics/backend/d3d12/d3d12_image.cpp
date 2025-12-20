#include "grassland/graphics/backend/d3d12/d3d12_image.h"

namespace grassland::graphics::backend {

D3D12Image::D3D12Image(D3D12Core *core, int width, int height, ImageFormat format,bool mip) : core_(core), format_(format) {
  mip_ = 1;
  if (mip) {
    mip_ = std::log2(width > height ? width : height) + 1;
  }
  core_->Device()->CreateImageMip(width, height, mip_, ImageFormatToDXGIFormat(format), &image_);
}

Extent2D D3D12Image::Extent() const {
  Extent2D extent;
  extent.width = image_->Width();
  extent.height = image_->Height();
  return extent;
}

ImageFormat D3D12Image::Format() const {
  return format_;
}
void D3D12Image::UploadData(const void *data) const {
  auto pixel_size = PixelSize(format_);
  const UINT64 upload_buffer_size = GetRequiredIntermediateSize(image_->Handle(), 0, 1);
  std::unique_ptr<d3d12::Buffer> upload_buffer;
  core_->Device()->CreateBuffer(upload_buffer_size, D3D12_HEAP_TYPE_UPLOAD, &upload_buffer);

  D3D12_SUBRESOURCE_DATA subresource_data{};
  subresource_data.pData = data;
  subresource_data.RowPitch = image_->Width() * pixel_size;
  subresource_data.SlicePitch = subresource_data.RowPitch * image_->Height();

  core_->SingleTimeCommand([&, upload_buffer = upload_buffer.get()](ID3D12GraphicsCommandList *command_list) {
    CD3DX12_RESOURCE_BARRIER to_copy = CD3DX12_RESOURCE_BARRIER::Transition(
        image_->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST, 0);
    command_list->ResourceBarrier(1, &to_copy);

    UpdateSubresources(command_list, image_->Handle(), upload_buffer->Handle(), 0, 0, 1, &subresource_data);

    CD3DX12_RESOURCE_BARRIER to_read = CD3DX12_RESOURCE_BARRIER::Transition(
        image_->Handle(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ, 0);
    command_list->ResourceBarrier(1, &to_read);
  });

  auto desc = image_->Handle()->GetDesc();
  UINT mip_levels = desc.MipLevels;
  if (mip_levels <= 1) {
    return;
  }

  ID3D12Device *device = core_->Device()->Handle();
  auto blit_pipeline = core_->BlitPipeline();

  for (UINT mip = 1; mip < mip_levels; ++mip) {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;

    {
      D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
      srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      srv_heap_desc.NumDescriptors = 1;
      srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      d3d12::ThrowIfFailed(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap)),
                           "Failed to create SRV heap for mip generation.");
    }

    {
      D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
      rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      rtv_heap_desc.NumDescriptors = 1;
      rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      d3d12::ThrowIfFailed(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)),
                           "Failed to create RTV heap for mip generation.");
    }

    core_->SingleTimeCommand([&, srv_heap, rtv_heap, desc, mip](ID3D12GraphicsCommandList *command_list) { // Use blit shader pipline to downsample from previous level...
      ID3D12Device *device_local = core_->Device()->Handle();

      D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
      srv_desc.Format = desc.Format;
      if (srv_desc.Format == DXGI_FORMAT_D32_FLOAT) {
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
      }
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv_desc.Texture2D.MostDetailedMip = mip - 1;
      srv_desc.Texture2D.MipLevels = 1; 
      srv_desc.Texture2D.PlaneSlice = 0;
      srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;

      device_local->CreateShaderResourceView(image_->Handle(), &srv_desc,
                                             srv_heap->GetCPUDescriptorHandleForHeapStart());

      D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
      rtv_desc.Format = desc.Format;
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtv_desc.Texture2D.MipSlice = mip;
      rtv_desc.Texture2D.PlaneSlice = 0;

      auto rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
      device_local->CreateRenderTargetView(image_->Handle(), &rtv_desc, rtv_handle);

      CD3DX12_RESOURCE_BARRIER to_rtv = CD3DX12_RESOURCE_BARRIER::Transition(
          image_->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET, mip);
      command_list->ResourceBarrier(1, &to_rtv);

      ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
      command_list->SetDescriptorHeaps(1, heaps);
      command_list->SetGraphicsRootSignature(blit_pipeline->root_signature->Handle());
      command_list->SetPipelineState(blit_pipeline->GetPipelineState(desc.Format)->Handle());
      command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      float width = static_cast<float>(std::max<UINT64>(1, desc.Width >> mip));
      float height = static_cast<float>(std::max<UINT>(1, desc.Height >> mip));
      D3D12_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
      D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
      command_list->RSSetViewports(1, &viewport);
      command_list->RSSetScissorRects(1, &scissor);

      command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
      auto srv_gpu_handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
      command_list->SetGraphicsRootDescriptorTable(0, srv_gpu_handle);

      command_list->DrawInstanced(6, 1, 0, 0);

      CD3DX12_RESOURCE_BARRIER to_read = CD3DX12_RESOURCE_BARRIER::Transition(
          image_->Handle(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ, mip);
      command_list->ResourceBarrier(1, &to_read);
    });
  }
}


void D3D12Image::DownloadData(void *data) const {
  auto pixel_size = PixelSize(format_);
  const UINT64 download_buffer_size = GetRequiredIntermediateSize(image_->Handle(), 0, 1);
  std::unique_ptr<d3d12::Buffer> download_buffer;
  core_->Device()->CreateBuffer(download_buffer_size, D3D12_HEAP_TYPE_READBACK, &download_buffer);
  D3D12_SUBRESOURCE_DATA subresource_data{};
  subresource_data.pData = data;
  subresource_data.RowPitch = image_->Width() * pixel_size;
  subresource_data.SlicePitch = subresource_data.RowPitch * image_->Height();

  D3D12_TEXTURE_COPY_LOCATION src_location{};
  src_location.pResource = image_->Handle();
  src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_location.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dst_location{};
  dst_location.pResource = download_buffer->Handle();
  dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
  auto desc = image_->Handle()->GetDesc();
  core_->Device()->Handle()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);
  dst_location.PlacedFootprint = layout;

  core_->SingleTimeCommand([&](ID3D12GraphicsCommandList *command_list) {
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        image_->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list->ResourceBarrier(1, &barrier);

    command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(image_->Handle(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    command_list->ResourceBarrier(1, &barrier);
  });

  uint8_t *mapped_data = static_cast<uint8_t *>(download_buffer->Map());
  for (UINT row = 0; row < image_->Height(); row++) {
    memcpy(static_cast<uint8_t *>(data) + row * subresource_data.RowPitch,
           mapped_data + layout.Offset + row * layout.Footprint.RowPitch, subresource_data.RowPitch);
  }
  download_buffer->Unmap();
}

void D3D12Image::UploadData(const void *data, const Offset2D &offset, const Extent2D &extent) const {
  auto pixel_size = PixelSize(format_);

  // Create a staging image that matches the region size
  std::unique_ptr<d3d12::Image> staging_image;
  core_->Device()->CreateImage(extent.width, extent.height, ImageFormatToDXGIFormat(format_), &staging_image);

  // Create upload buffer sized for the staging image
  const UINT64 upload_buffer_size = GetRequiredIntermediateSize(staging_image->Handle(), 0, 1);
  std::unique_ptr<d3d12::Buffer> upload_buffer;
  core_->Device()->CreateBuffer(upload_buffer_size, D3D12_HEAP_TYPE_UPLOAD, &upload_buffer);

  // Calculate source data layout
  D3D12_SUBRESOURCE_DATA subresource_data{};
  subresource_data.pData = data;
  subresource_data.RowPitch = extent.width * pixel_size;
  subresource_data.SlicePitch = subresource_data.RowPitch * extent.height;

  core_->SingleTimeCommand([&](ID3D12GraphicsCommandList *command_list) {
    // Transition staging image to copy destination for upload
    CD3DX12_RESOURCE_BARRIER staging_barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        staging_image->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->ResourceBarrier(1, &staging_barrier);

    // Upload data to staging image
    UpdateSubresources(command_list, staging_image->Handle(), upload_buffer->Handle(), 0, 0, 1, &subresource_data);

    // Transition staging image to copy source
    staging_barrier = CD3DX12_RESOURCE_BARRIER::Transition(staging_image->Handle(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list->ResourceBarrier(1, &staging_barrier);

    // Transition main image to copy destination
    CD3DX12_RESOURCE_BARRIER main_barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        image_->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->ResourceBarrier(1, &main_barrier);

    // Copy from staging image to main image at specified offset
    D3D12_TEXTURE_COPY_LOCATION src_location{};
    src_location.pResource = staging_image->Handle();
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_location.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_location{};
    dst_location.pResource = image_->Handle();
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_location.SubresourceIndex = 0;

    // Copy entire staging image to the specified region
    D3D12_BOX src_box{};
    src_box.left = 0;
    src_box.top = 0;
    src_box.front = 0;
    src_box.right = extent.width;
    src_box.bottom = extent.height;
    src_box.back = 1;

    command_list->CopyTextureRegion(&dst_location, offset.x, offset.y, 0, &src_location, &src_box);

    // Transition main image back to generic read
    main_barrier = CD3DX12_RESOURCE_BARRIER::Transition(image_->Handle(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    command_list->ResourceBarrier(1, &main_barrier);
  });
}

void D3D12Image::DownloadData(void *data, const Offset2D &offset, const Extent2D &extent) const {
  auto pixel_size = PixelSize(format_);

  // Create a staging image that matches the region size
  std::unique_ptr<d3d12::Image> staging_image;
  core_->Device()->CreateImage(extent.width, extent.height, ImageFormatToDXGIFormat(format_), &staging_image);

  // Create download buffer sized for the staging image
  const UINT64 download_buffer_size = GetRequiredIntermediateSize(staging_image->Handle(), 0, 1);
  std::unique_ptr<d3d12::Buffer> download_buffer;
  core_->Device()->CreateBuffer(download_buffer_size, D3D12_HEAP_TYPE_READBACK, &download_buffer);

  // Calculate destination data layout
  D3D12_SUBRESOURCE_DATA subresource_data{};
  subresource_data.pData = data;
  subresource_data.RowPitch = extent.width * pixel_size;
  subresource_data.SlicePitch = subresource_data.RowPitch * extent.height;

  D3D12_TEXTURE_COPY_LOCATION src_location{};
  src_location.pResource = image_->Handle();
  src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_location.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dst_location{};
  dst_location.pResource = staging_image->Handle();
  dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst_location.SubresourceIndex = 0;

  // Copy from staging image to download buffer
  D3D12_TEXTURE_COPY_LOCATION staging_src_location{};
  staging_src_location.pResource = staging_image->Handle();
  staging_src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  staging_src_location.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION buffer_dst_location{};
  buffer_dst_location.pResource = download_buffer->Handle();
  buffer_dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
  auto staging_desc = staging_image->Handle()->GetDesc();
  core_->Device()->Handle()->GetCopyableFootprints(&staging_desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);
  buffer_dst_location.PlacedFootprint = layout;

  core_->SingleTimeCommand([&](ID3D12GraphicsCommandList *command_list) {
    // Transition staging image to copy destination
    CD3DX12_RESOURCE_BARRIER staging_barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        staging_image->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->ResourceBarrier(1, &staging_barrier);

    // Transition main image to copy source
    CD3DX12_RESOURCE_BARRIER main_barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        image_->Handle(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list->ResourceBarrier(1, &main_barrier);

    // Copy from main image to staging image at specified offset
    D3D12_BOX src_box{};
    src_box.left = offset.x;
    src_box.top = offset.y;
    src_box.front = 0;
    src_box.right = offset.x + extent.width;
    src_box.bottom = offset.y + extent.height;
    src_box.back = 1;

    command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, &src_box);

    // Transition staging image to copy source
    staging_barrier = CD3DX12_RESOURCE_BARRIER::Transition(staging_image->Handle(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list->ResourceBarrier(1, &staging_barrier);

    // Copy from staging image to download buffer
    D3D12_BOX staging_box{};
    staging_box.left = 0;
    staging_box.top = 0;
    staging_box.front = 0;
    staging_box.right = extent.width;
    staging_box.bottom = extent.height;
    staging_box.back = 1;

    command_list->CopyTextureRegion(&buffer_dst_location, 0, 0, 0, &staging_src_location, &staging_box);

    // Transition main image back to generic read after all operations are finished
    main_barrier = CD3DX12_RESOURCE_BARRIER::Transition(image_->Handle(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    command_list->ResourceBarrier(1, &main_barrier);
  });

  // Copy data from download buffer to user buffer
  uint8_t *mapped_data = static_cast<uint8_t *>(download_buffer->Map());
  for (UINT row = 0; row < extent.height; row++) {
    memcpy(static_cast<uint8_t *>(data) + row * subresource_data.RowPitch,
           mapped_data + layout.Offset + row * layout.Footprint.RowPitch, subresource_data.RowPitch);
  }
  download_buffer->Unmap();
}

d3d12::Image *D3D12Image::Image() const {
  return image_.get();
}

}  // namespace grassland::graphics::backend
