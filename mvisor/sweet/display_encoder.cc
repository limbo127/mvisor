/* 
 * MVisor - Sweet Renderer
 * Copyright (C) 2022 Terrence <terrence@tenclass.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "display_encoder.h"

#include <libyuv.h>

#include "logger.h"


SweetDisplayEncoder::SweetDisplayEncoder(uint width, uint height, uint bpp, uint stride, DisplayStreamConfig* config) :
  screen_width_(width), screen_height_(height), screen_bpp_(bpp), screen_stride_(stride), config_(config)
{
  /* make sure screen size is multiple of 2 */
  MV_ASSERT(screen_width_ % 2 == 0);
  MV_ASSERT(screen_height_ % 2 == 0);

  screen_bitmap_ = new uint8_t[screen_stride_ * screen_height_];
  InitializeX264();

  /* start encode thread */
  encode_thread_ = std::thread(&SweetDisplayEncoder::EncodeProcess, this);
}

SweetDisplayEncoder::~SweetDisplayEncoder() {
  /* terminate encode thread */
  destroyed_ = true;
  encode_cv_.notify_all();
  if (encode_thread_.joinable()) {
    encode_thread_.join();
  }

  x264_encoder_close(x264_);
  x264_picture_clean(&input_yuv_);
  delete screen_bitmap_;
}

void SweetDisplayEncoder::InitializeX264() {
  x264_param_t param;
  char tune[30];

  /* check fast decode flag, zerolatency is required */
  if (config_->flags() & 1) {
    strcpy(tune, "zerolatency,fastdecode");
  } else {
    strcpy(tune, "zerolatency");
  }
  if (x264_param_default_preset(&param, config_->preset().c_str(), tune) < 0) {
    MV_PANIC("failed to set default preset %s", config_->preset().c_str());
  }

  param.i_csp = X264_CSP_I420;
  param.i_width = screen_width_;
  param.i_height = screen_height_;
  
  /* rate control method is not configurable now */
  param.rc.i_rc_method = X264_RC_CRF;
  param.rc.f_rf_constant = config_->qmin();
  param.rc.i_vbv_max_bitrate = config_->bitrate() / 1000;
  param.rc.i_vbv_buffer_size = config_->bitrate() * 2 / 1000;

  param.i_fps_num = config_->fps();
  param.i_fps_den = 1;
  param.b_vfr_input = 0;
  param.b_repeat_headers = 1;
  param.b_annexb = 1;
  param.i_log_level = X264_LOG_ERROR;
  param.i_threads = config_->threads();
  param.i_keyint_min = 7200;
  param.i_keyint_max = 7200;
  param.i_scenecut_threshold = 0;

  /* check CABAC and num_ref_frames */
  if (config_->flags() & 2) {
    param.b_cabac = 1;
  }
  if (config_->flags() & 4) {
    param.i_frame_reference = 3;
  }

  if (x264_param_apply_profile(&param, config_->profile().c_str()) < 0) {
    MV_PANIC("failed to set profile %s", config_->profile().c_str());
  }

  if (x264_picture_alloc(&input_yuv_, param.i_csp, param.i_width, param.i_height) < 0) {
    MV_PANIC("failed to allocate yuv picture %dx%d", param.i_width, param.i_height);
  }

  x264_ = x264_encoder_open(&param);
  MV_ASSERT(x264_);
}

void SweetDisplayEncoder::Start(OutputCallback callback) {
  std::lock_guard<std::mutex> lock(encode_mutex_);
  started_ = true;
  force_keyframe_ = true;
  output_callback_ = callback;
  CreateEncodeSlice(0, 0, screen_height_, screen_width_);
}

void SweetDisplayEncoder::Stop() {
  std::lock_guard<std::mutex> lock(encode_mutex_);
  started_ = false;
  output_callback_ = nullptr;
}

/* Copy bits from partial to screen buffer */
void SweetDisplayEncoder::RenderPartial(DisplayPartialBitmap* partial) {
  auto dst = (uint8_t*)screen_bitmap_;
  int dst_stride = screen_stride_;
  uint8_t* dst_end = dst + dst_stride * screen_height_;

  if (partial->flip) {
    dst += dst_stride * (partial->y + partial->height - 1) + partial->x * (screen_bpp_ >> 3);
    dst_stride = -dst_stride;
  } else {
    dst += dst_stride * partial->y + partial->x * (screen_bpp_ >> 3);
  }
  int linesize = partial->width * (screen_bpp_ >> 3);
  size_t lines = partial->height;
  size_t src_index = 0;

  /* Note: It seems the size of each iovec is never greater than 64KB */
  while (lines > 0 && src_index < partial->vector.size()) {
    auto src = (uint8_t*)partial->vector[src_index].iov_base;
    auto copy_lines = partial->vector[src_index].iov_len / partial->stride;
    while (copy_lines > 0 && lines > 0) {
      MV_ASSERT(dst + linesize <= dst_end);
      memcpy(dst, src, linesize);
      src += partial->stride;
      dst += dst_stride;
      --copy_lines;
      --lines;
    }
    ++src_index;
  }
}

void SweetDisplayEncoder::Render(std::vector<DisplayPartialBitmap>& partials) {
  std::lock_guard<std::mutex> lock(encode_mutex_);
  for (auto& partial : partials) {
    RenderPartial(&partial);

    if (started_) {
      CreateEncodeSlice(partial.y, partial.x, partial.y + partial.height, partial.x + partial.width);
    }
  }

  // Wake up encode thread
  if (!encode_slices_.empty()) {
    encode_cv_.notify_all();
  }
}

void SweetDisplayEncoder::CreateEncodeSlice(uint top, uint left, uint bottom, uint right) {
  /* make sure position and size of the created slice is multiple of 2 */
  uint width_alignment = 16;
  if (left % width_alignment) {
    left -= left % width_alignment;
  }
  if (right % width_alignment) {
    right += width_alignment - (right % width_alignment);
  }

  uint height_alignment = 2;
  if (top % height_alignment) {
    top -= top % height_alignment;
  }
  if (bottom % height_alignment) {
    bottom += height_alignment - (bottom % height_alignment);
  }
  /* avoid overflow */
  if (right > screen_width_) {
    right = screen_width_;
  }
  if (bottom > screen_height_) {
    bottom = screen_height_;
  }

  EncodeSlice* slice = new EncodeSlice {
    .x = left,
    .y = top,
    .width = right - left,
    .height = bottom - top
  };
  if (x264_picture_alloc(&slice->yuv, X264_CSP_I420, slice->width, slice->height) < 0) {
    MV_PANIC("failed to allocate yuv slice %dx%d", slice->width, slice->height);
  }
  encode_slices_.push_back(slice);
}

void SweetDisplayEncoder::EncodeProcess() {
  SetThreadName("sweet-encoder");

  auto idle_interval = std::chrono::milliseconds(500);

  while (!destroyed_) {
    std::unique_lock<std::mutex> lock(encode_mutex_);
    encode_cv_.wait_for(lock, idle_interval);

    if (destroyed_)
      break;
    
    if (!started_)
      continue;

    if (encode_slices_.empty()) {
      lock.unlock();
    } else {
      ConvertSlices();

      auto copied(encode_slices_);
      encode_slices_.clear();
      lock.unlock();

      DrawSlices(copied);
    }
  
    Encode();

    lock.lock();
    if (output_callback_) {
      output_callback_(output_nal_->p_payload, output_nal_size_);
    }
  }
}

void SweetDisplayEncoder::ConvertSlices() {
  auto start_time = std::chrono::steady_clock::now();

  for (auto slice: encode_slices_) {
    uint8_t* src = screen_bitmap_ + screen_stride_ * slice->y + slice->x * (screen_bpp_ >> 3);
    auto dst = &slice->yuv.img;

    if (screen_bpp_ == 32) {
      libyuv::ARGBToI420(src, screen_stride_,
        dst->plane[0], dst->i_stride[0],
        dst->plane[1], dst->i_stride[1],
        dst->plane[2], dst->i_stride[2],
        slice->width, slice->height);
    } else if (screen_bpp_ == 24) {
      libyuv::RGB24ToI420(src, screen_stride_,
        dst->plane[0], dst->i_stride[0],
        dst->plane[1], dst->i_stride[1],
        dst->plane[2], dst->i_stride[2],
        slice->width, slice->height);
    }
  }

  auto delta = std::chrono::steady_clock::now() - start_time;
  auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
  MV_LOG("converted cost %ld ms", delta_ms);
}

void SweetDisplayEncoder::DrawSlices(std::vector<EncodeSlice*>& slices) {
  auto& dst = input_yuv_.img;

  for (auto slice: slices) {
    auto& src = slice->yuv.img;
    uint8_t* to, *from;

    // copy Y bits
    from = src.plane[0];
    to = dst.plane[0] + dst.i_stride[0] * slice->y + slice->x;
    for (uint j = 0; j < slice->height; j++) {
      memcpy(to, from, slice->width);
      to += dst.i_stride[0];
      from += src.i_stride[0];
    }

    // copy U bits
    to = dst.plane[1] + dst.i_stride[1] * (slice->y >> 1) + (slice->x >> 1);
    from = src.plane[1];
    for (uint j = 0; j < (slice->height >> 1); j++) {
      memcpy(to, from, slice->width >> 1);
      to += dst.i_stride[1];
      from += src.i_stride[1];
    }

    // copy V bits
    to = dst.plane[2] + dst.i_stride[2] * (slice->y >> 1) + (slice->x >> 1);
    from = src.plane[2];
    for (uint j = 0; j < (slice->height >> 1); j++) {
      memcpy(to, from, slice->width >> 1);
      to += dst.i_stride[2];
      from += src.i_stride[2];
    }
  
    // cleanup
    x264_picture_clean(&slice->yuv);
    delete slice;
  }
}

void SweetDisplayEncoder::Encode() {
  ++input_yuv_.i_pts;
  if (force_keyframe_) {
    input_yuv_.i_type = X264_TYPE_KEYFRAME;
    force_keyframe_ = false;
  } else {
    input_yuv_.i_type = X264_TYPE_AUTO;
  }

  output_nal_size_ = x264_encoder_encode(x264_, &output_nal_, &output_nal_sequence_, &input_yuv_, &output_yuv_);
  if (output_nal_size_ < 0)
    return;
}

void SweetDisplayEncoder::ForceKeyframe() {
  force_keyframe_ = true;
  encode_cv_.notify_all();
}