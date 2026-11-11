// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Copied from saulob's 13 Sentinels PC port (github.com/saulob/13-sentinels-aegis-rim-pc/gpu/shadps4/core/libraries/videodec/, GPL-2.0-or-later,
// shadPS4 code with saulob's changes) for 13 Sentinels: Aegis Rim's movies (H.264 through FFmpeg).

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_utils.h"
#include "videodec2_impl.h"
#include "videodec_error.h"

namespace Libraries::Videodec2 {

namespace {

struct OutputGeometry {
    u32 width;
    u32 pitch;
    u32 height;
    u64 size;
};

OutputGeometry GeometryOf(const AVFrame& frame) {
    OutputGeometry geometry;
    geometry.width = Common::AlignUp<u32>(frame.width, 16);
    geometry.pitch = Common::AlignUp<u32>(frame.width, 64);
    geometry.height = Common::AlignUp<u32>(frame.height, 16);
    geometry.size = (u64{geometry.pitch} * geometry.height * 3) / 2;
    return geometry;
}

void FillAvcPictureInfo(OrbisVideodec2AvcPictureInfo& info, const AVFrame& frame,
                        const AVCodecContext& context, const OutputGeometry& geometry) {
    info = {};
    info.this_size = sizeof(info);
    info.is_valid = true;
    info.pts_data = frame.pts;
    info.dts_data = frame.pkt_dts;
    info.attached_data = reinterpret_cast<u64>(frame.opaque);
    info.idr_pictureflag = (frame.flags & AV_FRAME_FLAG_KEY) != 0;
    info.profile_idc = static_cast<u8>(context.profile > 0 ? context.profile & 0xFF : 0);
    info.level_idc = static_cast<u8>(context.level > 0 ? context.level : 0);
    info.pic_width_in_mbs_minus1 = geometry.width / 16 - 1;
    info.pic_height_in_map_units_minus1 = geometry.height / 16 - 1;
    info.frame_mbs_only_flag = 1;
    info.frame_cropping_flag = geometry.pitch != u32(frame.width) || geometry.height != u32(frame.height);
    info.frame_crop_top_offset = 0;
    info.frame_crop_left_offset = 0;
    info.frame_crop_right_offset = geometry.pitch - frame.width;
    info.frame_crop_bottom_offset = geometry.height - frame.height;
}

void FillHevcPictureInfo(OrbisVideodec2HevcPictureInfo& info, const AVFrame& frame,
                         const OutputGeometry& geometry) {
    info = {};
    info.this_size = sizeof(info);
    info.is_valid = true;
    info.pts_data = frame.pts;
    info.dts_data = frame.pkt_dts;
    info.attached_data = reinterpret_cast<u64>(frame.opaque);
    info.frame_crop_top_offset = 0;
    info.frame_crop_left_offset = 0;
    info.frame_crop_right_offset = geometry.pitch - frame.width;
    info.frame_crop_bottom_offset = geometry.height - frame.height;
}

} // namespace

void VdecDecoder::ReleaseAVPacket(AVPacket* packet) {
    av_packet_free(&packet);
}

void VdecDecoder::ReleaseAVFrame(AVFrame* frame) {
    av_frame_free(&frame);
}

void VdecDecoder::ReleaseAVCodecContext(AVCodecContext* context) {
    avcodec_free_context(&context);
}

void VdecDecoder::ReleaseSwsContext(SwsContext* context) {
    sws_freeContext(context);
}

VdecDecoder::VdecDecoder(const OrbisVideodec2DecoderConfigInfo& config_info,
                         const OrbisVideodec2DecoderMemoryInfo& memory_info)
    : m_codec_context(nullptr, &ReleaseAVCodecContext), m_sws_context(nullptr, &ReleaseSwsContext) {
    const AVCodecID codec_id = config_info.codec_type == OrbisVideodec2CodecType::Avc
                                   ? AV_CODEC_ID_H264
                                   : AV_CODEC_ID_HEVC;
    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        LOG_ERROR(Lib_Vdec2, "No FFmpeg decoder for codec {}", avcodec_get_name(codec_id));
        return;
    }
    AVCodecContextPtr context{avcodec_alloc_context3(codec), &ReleaseAVCodecContext};
    if (!context) {
        LOG_ERROR(Lib_Vdec2, "Could not allocate the codec context");
        return;
    }
    context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    const int res = avcodec_open2(context.get(), codec, nullptr);
    if (res < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not open codec: {}", res);
        return;
    }
    m_codec_context = std::move(context);
}

VdecDecoder::~VdecDecoder() = default;

void VdecDecoder::DrainFrames() {
    while (true) {
        AVFramePtr frame{av_frame_alloc(), &ReleaseAVFrame};
        if (!frame) {
            break;
        }
        int ret = avcodec_receive_frame(m_codec_context.get(), frame.get());
        if (ret == 0) {
            m_frame_queue.push(std::move(frame));
        } else {
            break;
        }
    }
}

s32 VdecDecoder::Decode(const OrbisVideodec2InputData& input_data,
                        OrbisVideodec2FrameBuffer& frame_buffer,
                        OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    if (!input_data.au_data) {
        LOG_ERROR(Lib_Vdec2, "Invalid access unit data pointer");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
    }
    if (input_data.au_size == 0) {
        LOG_ERROR(Lib_Vdec2, "Invalid access unit size");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
    }

    AVPacketPtr packet{av_packet_alloc(), &ReleaseAVPacket};
    if (!packet) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate packet");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    packet->data = (u8*)input_data.au_data;
    packet->size = input_data.au_size;
    packet->pts = input_data.pts_data;
    packet->dts = input_data.dts_data;
    packet->opaque = reinterpret_cast<void*>(input_data.attached_data);

    int ret = avcodec_send_packet(m_codec_context.get(), packet.get());
    if (ret == AVERROR_EOF) {
        // Attempt to flush buffers and try again.
        avcodec_flush_buffers(m_codec_context.get());
        ret = avcodec_send_packet(m_codec_context.get(), packet.get());
    }
    if (ret == AVERROR(EAGAIN)) {
        DrainFrames();
        ret = avcodec_send_packet(m_codec_context.get(), packet.get());
    }
    if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error sending packet to decoder: {}", ret);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    packet.reset();

    DrainFrames();

    if (m_frame_queue.empty()) {
        return ORBIS_OK;
    }

    AVFramePtr frame = std::move(m_frame_queue.front());
    m_frame_queue.pop();

    if (frame->flags & AV_FRAME_FLAG_INTERLACED) {
        LOG_ERROR(Lib_Vdec2, "Interlaced video output is not supported.");
    }
    return WriteOutput(std::move(frame), frame_buffer, output_info);
}

s32 VdecDecoder::Flush(OrbisVideodec2FrameBuffer& frame_buffer,
                       OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    if (m_frame_queue.empty()) {
        avcodec_send_packet(m_codec_context.get(), nullptr);
        DrainFrames();
    }

    if (m_frame_queue.empty()) {
        return ORBIS_OK;
    }

    AVFramePtr frame = std::move(m_frame_queue.front());
    m_frame_queue.pop();
    return WriteOutput(std::move(frame), frame_buffer, output_info);
}

s32 VdecDecoder::Reset() {
    m_frame_queue = {};
    avcodec_flush_buffers(m_codec_context.get());
    return ORBIS_OK;
}

s32 VdecDecoder::WriteOutput(AVFramePtr frame, OrbisVideodec2FrameBuffer& frame_buffer,
                             OrbisVideodec2OutputInfo& output_info) {
    if (frame->format != AV_PIX_FMT_NV12) {
        AVFramePtr nv12_frame = ConvertNV12Frame(*frame);
        if (!nv12_frame) {
            return ORBIS_VIDEODEC2_ERROR_API_FAIL;
        }
        frame = std::move(nv12_frame);
    }

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    const OutputGeometry geometry = GeometryOf(*frame);

    if (!frame_buffer.frame_buffer || frame_buffer.frame_buffer_size < geometry.size + info_size ||
        !Videodec::CopyNV12DataChecked(static_cast<u8*>(frame_buffer.frame_buffer),
                                frame_buffer.frame_buffer_size - info_size, *frame)) {
        LOG_ERROR(Lib_Vdec2, "Frame buffer too small: {}x{} needs {:#x}, got {:#x}", frame->width,
                  frame->height, geometry.size + info_size, frame_buffer.frame_buffer_size);
        return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
    }
    frame_buffer.is_accepted = true;

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = geometry.width;
    output_info.frame_pitch = geometry.pitch;
    output_info.frame_height = geometry.height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = geometry.size;

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = geometry.pitch;
    }

    u8* const picture_info = static_cast<u8*>(output_info.frame_buffer) + output_info.frame_buffer_size;
    if (is_avc) {
        FillAvcPictureInfo(*reinterpret_cast<OrbisVideodec2AvcPictureInfo*>(picture_info), *frame,
                           *m_codec_context, geometry);
    } else {
        FillHevcPictureInfo(*reinterpret_cast<OrbisVideodec2HevcPictureInfo*>(picture_info),
                            *frame, geometry);
    }

    return ORBIS_OK;
}

VdecDecoder::AVFramePtr VdecDecoder::ConvertNV12Frame(AVFrame& frame) {
    AVFramePtr nv12_frame{av_frame_alloc(), &ReleaseAVFrame};
    if (!nv12_frame) {
        return {nullptr, &ReleaseAVFrame};
    }
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->flags = frame.flags;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;
    nv12_frame->opaque = frame.opaque;

    if (av_frame_get_buffer(nv12_frame.get(), 0) < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not allocate the NV12 frame");
        return {nullptr, &ReleaseAVFrame};
    }

    m_sws_context.reset(sws_getCachedContext(
        m_sws_context.release(), frame.width, frame.height, AVPixelFormat(frame.format),
        nv12_frame->width, nv12_frame->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr,
        nullptr, nullptr));
    if (!m_sws_context) {
        LOG_ERROR(Lib_Vdec2, "No conversion from pixel format {} to NV12", frame.format);
        return {nullptr, &ReleaseAVFrame};
    }

    const auto res = sws_scale(m_sws_context.get(), frame.data, frame.linesize, 0, frame.height,
                               nv12_frame->data, nv12_frame->linesize);
    if (res < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not convert to NV12: {}", res);
        return {nullptr, &ReleaseAVFrame};
    }

    return nv12_frame;
}

} // namespace Libraries::Videodec2
