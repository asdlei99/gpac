/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2010-2017
 *					All rights reserved
 *
 *  This file is part of GPAC / OpenHEVC decoder filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <gpac/filters.h>
#include <gpac/avparse.h>
#include <gpac/constants.h>

#if defined(GPAC_HAS_OPENHEVC) && !defined(GPAC_DISABLE_AV_PARSERS)

#include <gpac/internal/media_dev.h>
#include <openHevcWrapper.h>


#define OPEN_SHVC

#if defined(WIN32) && !defined(_WIN32_WCE) && !defined(__GNUC__)
#  pragma comment(lib, "libLibOpenHevcWrapper")

#if !defined _WIN64
void libOpenHevcSetViewLayers(OpenHevc_Handle codec, int val)
{
}
#endif

#endif

#define HEVC_MAX_STREAMS 2

typedef struct
{
	GF_FilterPid *ipid;
	u32 cfg_crc;
	u32 id;
	u32 dep_id;
} GF_HEVCStream;

typedef struct{
	u64 cts;
	u32 duration;
	u8 sap_type;
	u8 seek_flag;
} OHEVCDecFrameInfo;

typedef struct
{
	//options
	u32 threading;
	Bool no_copy;
	u32 nb_threads;
	Bool pack_hfr;
	Bool seek_reset;
	Bool force_stereo;

	//internal
	GF_Filter *filter;
	GF_FilterPid *opid;
	GF_HEVCStream streams[HEVC_MAX_STREAMS];
	u32 nb_streams;
	Bool is_multiview;

	OHEVCDecFrameInfo *frame_infos;
	u32 frame_infos_alloc, frame_infos_size;

	u32 width, stride, height, out_size, luma_bpp, chroma_bpp;
	GF_Fraction sar;
	GF_FilterPacket *packed_pck;
	char *packed_data;

	u32 hevc_nalu_size_length;
	Bool has_pic;

	OpenHevc_Handle codec;
	u32 nb_layers, cur_layer;

	Bool decoder_started;
	
	u32 frame_idx;
	u32 dec_frames;
	u8  chroma_format_idc;

#ifdef  OPENHEVC_HAS_AVC_BASE
	u32 avc_base_id;
	u32 avc_nalu_size_length;
	char *avc_base;
	u32 avc_base_size;
	u32 avc_base_pts;
#endif

	Bool force_stereo_reset;

	GF_FilterHWFrame hw_frame;
	OpenHevc_Frame frame_ptr;
	Bool frame_out;
} GF_OHEVCDecCtx;

static GF_Err ohevcdec_configure_scalable_pid(GF_OHEVCDecCtx *ctx, GF_FilterPid *pid, u32 oti, Bool has_scalable, const GF_PropertyValue *dsi)
{
	GF_HEVCConfig *cfg = NULL;
	char *data;
	u32 data_len;
	GF_BitStream *bs;
	u32 i, j;

	if (!ctx->codec) return GF_NOT_SUPPORTED;
	if (! has_scalable) return GF_OK;

	if (!dsi || !dsi->data_len) {
		ctx->nb_layers++;
		ctx->cur_layer++;
		libOpenHevcSetActiveDecoders(ctx->codec, ctx->cur_layer-1);
		libOpenHevcSetViewLayers(ctx->codec, ctx->cur_layer-1);
		return GF_OK;
	}
	//FIXME in isomedia this should be an LHCC, not an HVCC
	if (oti==GPAC_OTI_VIDEO_LHVC) {
		cfg = gf_odf_hevc_cfg_read(dsi->value.data, dsi->data_len, GF_FALSE);
	} else {
		cfg = gf_odf_hevc_cfg_read(dsi->value.data, dsi->data_len, GF_FALSE);
	}
	
	if (!cfg) return GF_NON_COMPLIANT_BITSTREAM;
	if (!ctx->hevc_nalu_size_length) ctx->hevc_nalu_size_length = cfg->nal_unit_size;
	else if (ctx->hevc_nalu_size_length != cfg->nal_unit_size)
		return GF_NON_COMPLIANT_BITSTREAM;
	
	ctx->nb_layers++;
	ctx->cur_layer++;
	libOpenHevcSetActiveDecoders(ctx->codec, ctx->nb_layers-1);
	libOpenHevcSetViewLayers(ctx->codec, ctx->nb_layers-1);

#ifdef  OPENHEVC_HAS_AVC_BASE
	//LHVC mode with base AVC layer: set extradata for LHVC
	if (ctx->avc_base_id) {
		libOpenShvcCopyExtraData(ctx->codec, NULL, (u8 *) dsi->value.data, 0, dsi->data_len);
	} else
#endif
	//LHVC mode with base HEVC layer: decode the LHVC SPS/PPS/VPS
	{
		bs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
		for (i=0; i< gf_list_count(cfg->param_array); i++) {
			GF_HEVCParamArray *ar = (GF_HEVCParamArray *)gf_list_get(cfg->param_array, i);
			for (j=0; j< gf_list_count(ar->nalus); j++) {
				GF_AVCConfigSlot *sl = (GF_AVCConfigSlot *)gf_list_get(ar->nalus, j);
				gf_bs_write_int(bs, sl->size, 8*ctx->hevc_nalu_size_length);
				gf_bs_write_data(bs, sl->data, sl->size);
			}
		}

		gf_bs_get_content(bs, &data, &data_len);
		gf_bs_del(bs);
		//the decoder may not be already started
		if (!ctx->decoder_started) {
			libOpenHevcStartDecoder(ctx->codec);
			ctx->decoder_started=1;
		}
		
		libOpenHevcDecode(ctx->codec, (u8 *)data, data_len, 0);
		gf_free(data);
	}
	gf_odf_hevc_cfg_del(cfg);
	return GF_OK;
}

#if defined(OPENHEVC_HAS_AVC_BASE) && !defined(GPAC_DISABLE_LOG)
void openhevc_log_callback(void *udta, int l, const char*fmt, va_list vl)
{
	u32 level = GF_LOG_DEBUG;
	if (l <= OHEVC_LOG_ERROR) l = GF_LOG_ERROR;
	else if (l <= OHEVC_LOG_WARNING) l = GF_LOG_WARNING;
	else if (l <= OHEVC_LOG_INFO) l = GF_LOG_INFO;
	else if (l >= OHEVC_LOG_VERBOSE) return;

	if (gf_log_tool_level_on(GF_LOG_CODEC, level)) {
		gf_log_va_list(level, GF_LOG_CODEC, fmt, vl);
	}
}
#endif

static void ohevcdec_set_codec_name(GF_Filter *filter)
{
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx*) gf_filter_get_udta(filter);
#ifdef  OPENHEVC_HAS_AVC_BASE
	if (ctx->avc_base_id) {
		if (ctx->cur_layer==1) gf_filter_set_name(filter, "OpenHEVC-v"NV_VERSION"-AVC|H264");
		else gf_filter_set_name(filter, "OpenHEVC-v"NV_VERSION"-AVC|H264+LHVC");
	}
	if (ctx->cur_layer==1) gf_filter_set_name(filter, "OpenHEVC-v"NV_VERSION);
	else gf_filter_set_name(filter, "OpenHEVC-v"NV_VERSION"-LHVC");
#else
	return gf_filter_set_name(filter, libOpenHevcVersion(ctx->codec) );
#endif
}

static GF_Err ohevcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	u32 i, j, dep_id=0, id=0, cfg_crc=0, oti, stride_mul=1;
	Bool found, has_scalable = GF_FALSE;
	const GF_PropertyValue *p, *dsi;
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx*) gf_filter_get_udta(filter);

	if (is_remove) {
		if (ctx->streams[0].ipid == pid) {
			memset(ctx->streams, 0, HEVC_MAX_STREAMS*sizeof(GF_HEVCStream));
			if (ctx->opid) gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
			ctx->nb_streams = 0;
			if (ctx->codec) libOpenHevcClose(ctx->codec);
			ctx->codec = NULL;
			return GF_OK;
		} else {
			for (i=0; i<ctx->nb_streams; i++) {
				if (ctx->streams[i].ipid == pid) {
					ctx->streams[i].ipid = NULL;
					ctx->streams[i].cfg_crc = 0;
					memmove(&ctx->streams[i], &ctx->streams[i+1], sizeof(GF_HEVCStream)*(ctx->nb_streams-1));
					ctx->nb_streams--;
					return GF_OK;
				}
			}
		}
		return GF_OK;
	}
	if (! gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DEPENDENCY_ID);
	if (p) dep_id = p->value.uint;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
	if (!p) p = gf_filter_pid_get_property(pid, GF_PROP_PID_ESID);
	if (p) id = p->value.uint;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_OTI);
	oti = p ? p->value.uint : 0;
	if (!oti) return GF_NOT_SUPPORTED;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_SCALABLE);
	if (p) has_scalable = p->value.boolean;

	dsi = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (dsi && dsi->value.data && dsi->data_len) {
		cfg_crc = gf_crc_32(dsi->value.data, dsi->data_len);
		for (i=0; i<ctx->nb_streams; i++) {
			if ((ctx->streams[i].ipid == pid) && (ctx->streams[i].cfg_crc == cfg_crc)) return GF_OK;
		}
	}

	found = GF_FALSE;
	for (i=0; i<ctx->nb_streams; i++) {
		if (ctx->streams[i].ipid == pid) {
			ctx->streams[i].cfg_crc = cfg_crc;
			found = GF_TRUE;
		}
	}
	if (!found) {
		if (ctx->nb_streams==HEVC_MAX_STREAMS) {
			return GF_NOT_SUPPORTED;
		}
		//insert new pid in order of dependencies
		for (i=0; i<ctx->nb_streams; i++) {

			if (!dep_id && !ctx->streams[i].dep_id) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[SVC Decoder] Detected multiple independent base (%s and %s)\n", gf_filter_pid_get_name(pid), gf_filter_pid_get_name(ctx->streams[i].ipid)));
				return GF_REQUIRES_NEW_INSTANCE;
			}

			if (ctx->streams[i].id == dep_id) {
				if (ctx->nb_streams > i+2)
					memmove(&ctx->streams[i+1], &ctx->streams[i+2], sizeof(GF_HEVCStream) * (ctx->nb_streams-i-1));

				ctx->streams[i+1].ipid = pid;
				ctx->streams[i+1].cfg_crc = cfg_crc;
				ctx->streams[i+1].dep_id = dep_id;
				ctx->streams[i+1].id = id;
				gf_filter_pid_set_framing_mode(pid, GF_TRUE);
				found = GF_TRUE;
				break;
			}
			if (ctx->streams[i].dep_id == id) {
				if (ctx->nb_streams > i+1)
					memmove(&ctx->streams[i+1], &ctx->streams[i], sizeof(GF_HEVCStream) * (ctx->nb_streams-i));

				ctx->streams[i].ipid = pid;
				ctx->streams[i].cfg_crc = cfg_crc;
				ctx->streams[i].dep_id = dep_id;
				ctx->streams[i].id = id;
				gf_filter_pid_set_framing_mode(pid, GF_TRUE);
				found = GF_TRUE;
				break;
			}
		}
		if (!found) {
			ctx->streams[ctx->nb_streams].ipid = pid;
			ctx->streams[ctx->nb_streams].cfg_crc = cfg_crc;
			ctx->streams[ctx->nb_streams].id = id;
			ctx->streams[ctx->nb_streams].dep_id = dep_id;
			gf_filter_pid_set_framing_mode(pid, GF_TRUE);
		}
		ctx->nb_streams++;
	}
	
	ctx->nb_layers = 1;
	ctx->cur_layer = 1;

	//scalable stream setup
	if (dep_id) {
		GF_Err e = ohevcdec_configure_scalable_pid(ctx, pid, oti, has_scalable, dsi);
		ohevcdec_set_codec_name(filter);
		return e;
	}


	if (oti == GPAC_OTI_VIDEO_AVC)
#ifdef  OPENHEVC_HAS_AVC_BASE
		ctx->avc_base_id = id;
#else
	return GF_NOT_SUPPORTED;
#endif
	
	if (dsi && dsi->data_len) {
#ifdef  OPENHEVC_HAS_AVC_BASE
		if (oti==GPAC_OTI_VIDEO_AVC) {
			GF_AVCConfig *avcc = NULL;
			AVCState avc;
			memset(&avc, 0, sizeof(AVCState));

			avcc = gf_odf_avc_cfg_read(dsi->value.data, dsi->data_len);
			if (!avcc) return GF_NON_COMPLIANT_BITSTREAM;
			ctx->avc_nalu_size_length = avcc->nal_unit_size;

			for (i=0; i< gf_list_count(avcc->sequenceParameterSets); i++) {
				GF_AVCConfigSlot *sl = (GF_AVCConfigSlot *)gf_list_get(avcc->sequenceParameterSets, i);
				s32 idx = gf_media_avc_read_sps(sl->data, sl->size, &avc, 0, NULL);
				ctx->width = MAX(avc.sps[idx].width, ctx->width);
				ctx->height = MAX(avc.sps[idx].height, ctx->height);
				ctx->luma_bpp = avcc->luma_bit_depth;
				ctx->chroma_bpp = avcc->chroma_bit_depth;
				ctx->chroma_format_idc = avcc->chroma_format;
			}
			gf_odf_avc_cfg_del(avcc);
		} else

#endif
	{
		GF_HEVCConfig *hvcc = NULL;
		HEVCState hevc;
		memset(&hevc, 0, sizeof(HEVCState));

		hvcc = gf_odf_hevc_cfg_read(dsi->value.data, dsi->data_len, GF_FALSE);
		if (!hvcc) return GF_NON_COMPLIANT_BITSTREAM;
		ctx->hevc_nalu_size_length = hvcc->nal_unit_size;

		for (i=0; i< gf_list_count(hvcc->param_array); i++) {
			GF_HEVCParamArray *ar = (GF_HEVCParamArray *)gf_list_get(hvcc->param_array, i);
			for (j=0; j< gf_list_count(ar->nalus); j++) {
				GF_AVCConfigSlot *sl = (GF_AVCConfigSlot *)gf_list_get(ar->nalus, j);
				s32 idx;
				u16 hdr = sl->data[0] << 8 | sl->data[1];

				if (ar->type==GF_HEVC_NALU_SEQ_PARAM) {
					idx = gf_media_hevc_read_sps(sl->data, sl->size, &hevc);
					ctx->width = MAX(hevc.sps[idx].width, ctx->width);
					ctx->height = MAX(hevc.sps[idx].height, ctx->height);
					ctx->luma_bpp = MAX(hevc.sps[idx].bit_depth_luma, ctx->luma_bpp);
					ctx->chroma_bpp = MAX(hevc.sps[idx].bit_depth_chroma, ctx->chroma_bpp);
					ctx->chroma_format_idc  = hevc.sps[idx].chroma_format_idc;
					
					if (hdr & 0x1f8) {
						ctx->nb_layers ++;
					}
				}
				else if (ar->type==GF_HEVC_NALU_VID_PARAM) {
					s32 vps_id = gf_media_hevc_read_vps(sl->data, sl->size, &hevc);
					//multiview
					if ((vps_id>=0) && (hevc.vps[vps_id].scalability_mask[1])) {
						ctx->is_multiview = GF_TRUE;
						if (ctx->force_stereo)
							stride_mul=2;
					}
				}
				else if (ar->type==GF_HEVC_NALU_PIC_PARAM) {
					gf_media_hevc_read_pps(sl->data, sl->size, &hevc);
				}
			}
		}
		gf_odf_hevc_cfg_del(hvcc);
	}
	
	}

#ifdef  OPENHEVC_HAS_AVC_BASE
	if (ctx->avc_base_id) {
		ctx->codec = libOpenShvcInit(ctx->nb_threads, ctx->threading);
	} else
#endif
	{
		ctx->codec = libOpenHevcInit(ctx->nb_threads, ctx->threading);
	}

#if defined(OPENHEVC_HAS_AVC_BASE) && !defined(GPAC_DISABLE_LOG)
	if (gf_log_tool_level_on(GF_LOG_CODEC, GF_LOG_DEBUG) ) {
		libOpenHevcSetDebugMode(ctx->codec, OHEVC_LOG_DEBUG);
	} else if (gf_log_tool_level_on(GF_LOG_CODEC, GF_LOG_INFO) ) {
		libOpenHevcSetDebugMode(ctx->codec, OHEVC_LOG_INFO);
	} else if (gf_log_tool_level_on(GF_LOG_CODEC, GF_LOG_WARNING) ) {
		libOpenHevcSetDebugMode(ctx->codec, OHEVC_LOG_WARNING);
	} else {
		libOpenHevcSetDebugMode(ctx->codec, OHEVC_LOG_ERROR);
	}
	libOpenHevcSetLogCallback(ctx->codec, openhevc_log_callback);
#endif

	if (dsi) {
		if (has_scalable) {
			ctx->cur_layer = ctx->nb_layers;
			libOpenHevcSetActiveDecoders(ctx->codec, ctx->cur_layer-1);
			libOpenHevcSetViewLayers(ctx->codec, ctx->cur_layer-1);
		} else {
			libOpenHevcSetActiveDecoders(ctx->codec, 1);
			libOpenHevcSetViewLayers(ctx->codec, 0);
		}

#ifdef  OPENHEVC_HAS_AVC_BASE
		if (ctx->avc_base_id) {
			libOpenShvcCopyExtraData(ctx->codec, (u8 *) dsi->value.data, NULL, dsi->data_len, 0);
		} else
#endif
			libOpenHevcCopyExtraData(ctx->codec, (u8 *) dsi->value.data, dsi->data_len);
	}else{
		//decode and display layer 0 by default - will be changed when attaching enhancement layers

		//has_scalable_layers is set, the esd describes a set of HEVC stream but we don't know how many - for now only two decoders so easy,
		//but should be fixed in the future
		if (has_scalable) {
			ctx->nb_layers = 2;
			ctx->cur_layer = 2;
		}
		libOpenHevcSetActiveDecoders(ctx->codec, ctx->cur_layer-1);
		libOpenHevcSetViewLayers(ctx->codec, ctx->cur_layer-1);
	}

	//in case we don't have a config record
	if (!ctx->chroma_format_idc) ctx->chroma_format_idc = 1;

	//we start decoder on the first frame
	ctx->dec_frames = 0;
	ohevcdec_set_codec_name(filter);

	if (!ctx->opid) {
		ctx->opid = gf_filter_pid_new(filter);
		gf_filter_pid_copy_properties(ctx->opid, ctx->streams[0].ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_OTI, &PROP_UINT(GPAC_OTI_RAW_MEDIA_STREAM) );
	}

	return GF_OK;
}

static u32 ohevcdec_get_pixel_format( u32 luma_bpp, u8 chroma_format_idc)
{
	switch (chroma_format_idc) {
	case 1:
		return (luma_bpp==10) ? GF_PIXEL_YV12_10 : GF_PIXEL_YV12;
	case 2:
		return (luma_bpp==10) ? GF_PIXEL_YUV422_10 : GF_PIXEL_YUV422;
	case 3:
		return (luma_bpp==10) ? GF_PIXEL_YUV444_10 : GF_PIXEL_YUV444;
	default:
		return 0;
	}
	return 0;
}


static Bool ohevcdec_process_event(GF_Filter *filter, const GF_FilterEvent *fevt)
{
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx*) gf_filter_get_udta(filter);

	if (fevt->base.type == GF_FEVT_QUALITY_SWITCH) {

		if (ctx->nb_layers==1) return GF_FALSE;
		/*switch up*/
		if (fevt->quality_switch.up > 0) {
			if (ctx->cur_layer>=ctx->nb_layers) return GF_FALSE;
			ctx->cur_layer++;
		} else {
			if (ctx->cur_layer<=1) return GF_FALSE;
			ctx->cur_layer--;
		}
		libOpenHevcSetViewLayers(ctx->codec, ctx->cur_layer-1);
		libOpenHevcSetActiveDecoders(ctx->codec, ctx->cur_layer-1);
		if (ctx->is_multiview)
			ctx->force_stereo_reset = ctx->force_stereo;

		//todo: we should get the set of pids active and trigger the switch up/down based on that
		//rather than not canceling the event
		return GF_FALSE;
	} else if (fevt->base.type == GF_FEVT_STOP) {
		if (ctx->seek_reset && ctx->dec_frames) {
			u32 i;
			u32 cl = ctx->cur_layer;
			u32 nl = ctx->nb_layers;

			//quick hack, we have an issue with openHEVC resuming after being flushed ...
			libOpenHevcClose(ctx->codec);
			ctx->codec = NULL;
			ctx->decoder_started = GF_FALSE;
			for (i=0; i<ctx->nb_streams; i++) {
				ohevcdec_configure_pid(filter, ctx->streams[i].ipid, GF_FALSE);
			}
			ctx->cur_layer = cl;
			ctx->nb_layers = nl;
			if (ctx->codec) {
				libOpenHevcSetActiveDecoders(ctx->codec, ctx->cur_layer-1);
				libOpenHevcSetViewLayers(ctx->codec, ctx->cur_layer-1);
			}
		}
	}
	return GF_FALSE;
}


void ohevcframe_release(GF_Filter *filter, GF_FilterPid *pid, GF_FilterPacket *pck)
{
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx *) gf_filter_get_udta(filter);
	ctx->frame_out = GF_FALSE;
	gf_filter_post_process_task(ctx->filter);
}

GF_Err ohevcframe_get_plane(GF_FilterHWFrame *frame, u32 plane_idx, const u8 **outPlane, u32 *outStride)
{
	GF_Err e;
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx *)frame->user_data;
	if (! outPlane || !outStride) return GF_BAD_PARAM;
	*outPlane = NULL;
	*outStride = 0;

	e = GF_OK;
	if (plane_idx==0) {
		*outPlane = (const u8 *) ctx->frame_ptr.pvY;
		*outStride = ctx->frame_ptr.frameInfo.nYPitch;
	} else if (plane_idx==1) {
		*outPlane = (const u8 *)  ctx->frame_ptr.pvU;
		*outStride = ctx->frame_ptr.frameInfo.nUPitch;
	} else if (plane_idx==2) {
		*outPlane = (const u8 *)  ctx->frame_ptr.pvV;
		*outStride = ctx->frame_ptr.frameInfo.nVPitch;
	} else
		return GF_BAD_PARAM;

	return GF_OK;
}

static GF_Err ohevcdec_send_output_frame(GF_OHEVCDecCtx *ctx)
{
	GF_FilterPacket *dst_pck;

	ctx->hw_frame.user_data = ctx;
	ctx->hw_frame.get_plane = ohevcframe_get_plane;
	//we only keep one frame out, force releasing it
	ctx->hw_frame.hardware_reset_pending = GF_TRUE;
	libOpenHevcGetOutput(ctx->codec, 1, &ctx->frame_ptr);

	dst_pck = gf_filter_pck_new_hw_frame(ctx->opid, &ctx->hw_frame, ohevcframe_release);
	gf_filter_pck_set_cts(dst_pck, ctx->frame_ptr.frameInfo.nTimeStamp);

	ctx->frame_out = GF_TRUE;
	gf_filter_pck_send(dst_pck);
	return GF_OK;
}
static GF_Err ohevcdec_flush_picture(GF_OHEVCDecCtx *ctx)
{
	GF_FilterPacket *pck;
	char *data;
	u32 a_w, a_h, a_stride, bit_depth;
	u64 cts;
	OpenHevc_Frame_cpy openHevcFrame_FL, openHevcFrame_SL;
	int chromat_format;

	if (ctx->no_copy && !ctx->pack_hfr) {
		libOpenHevcGetPictureInfo(ctx->codec, &openHevcFrame_FL.frameInfo);
	} else {
		libOpenHevcGetPictureInfoCpy(ctx->codec, &openHevcFrame_FL.frameInfo);
		if (ctx->nb_layers == 2) libOpenHevcGetPictureInfoCpy(ctx->codec, &openHevcFrame_SL.frameInfo);
	}

	a_w = openHevcFrame_FL.frameInfo.nWidth;
	a_h = openHevcFrame_FL.frameInfo.nHeight;
	a_stride = openHevcFrame_FL.frameInfo.nYPitch;
	bit_depth = openHevcFrame_FL.frameInfo.nBitDepth;
	chromat_format = openHevcFrame_FL.frameInfo.chromat_format;
	cts = (u32) openHevcFrame_FL.frameInfo.nTimeStamp;
	
	if (ctx->force_stereo_reset || !ctx->out_size || (ctx->width != a_w) || (ctx->height!=a_h) || (ctx->stride != a_stride)
		|| (ctx->luma_bpp!= bit_depth)  || (ctx->chroma_bpp != bit_depth) || (ctx->chroma_format_idc != (chromat_format + 1))
		|| (ctx->sar.num*openHevcFrame_FL.frameInfo.sample_aspect_ratio.den != ctx->sar.den*openHevcFrame_FL.frameInfo.sample_aspect_ratio.num)
	 ) {
		u32 pixfmt;
		ctx->width = a_w;
		ctx->stride = a_stride;
		ctx->height = a_h;
		if( chromat_format == YUV420 ) {
			ctx->out_size = ctx->stride * ctx->height * 3 / 2;
		} else if  ( chromat_format == YUV422 ) {
			ctx->out_size = ctx->stride * ctx->height * 2;
		} else if ( chromat_format == YUV444 ) {
			ctx->out_size = ctx->stride * ctx->height * 3;
		} 
		ctx->luma_bpp = ctx->chroma_bpp = bit_depth;
		ctx->chroma_format_idc = chromat_format + 1;
		ctx->sar.num = openHevcFrame_FL.frameInfo.sample_aspect_ratio.num;
		ctx->sar.den = openHevcFrame_FL.frameInfo.sample_aspect_ratio.den;
		if (!ctx->sar.num) ctx->sar.num = ctx->sar.den = 1;

		if (ctx->pack_hfr) {
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(2*ctx->width) );
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(2*ctx->height) );
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(2*ctx->stride) );
			ctx->out_size *= 4;
		} else {
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width) );
			if (ctx->force_stereo && ctx->is_multiview && ctx->cur_layer>1) {
				gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(2*ctx->height) );
				ctx->out_size *= 2;
			} else {
				gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height) );
			}

			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->stride) );
		}
		pixfmt = ohevcdec_get_pixel_format(ctx->luma_bpp, ctx->chroma_format_idc);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(pixfmt) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAR, &PROP_FRAC(ctx->sar) );
	}


	if (ctx->no_copy && !ctx->pack_hfr) {
		return ohevcdec_send_output_frame(ctx);
	}


	if (ctx->pack_hfr) {
		OpenHevc_Frame openHFrame;
		u8 *pY, *pU, *pV;
		u32 idx_w, idx_h;

		idx_w = ((ctx->frame_idx==0) || (ctx->frame_idx==2)) ? 0 : ctx->stride;
		idx_h = ((ctx->frame_idx==0) || (ctx->frame_idx==1)) ? 0 : ctx->height*2*ctx->stride;

		if (!ctx->packed_pck) {
			ctx->packed_pck = gf_filter_pck_new_alloc(ctx->opid, ctx->out_size, &ctx->packed_data);
			gf_filter_pck_set_cts(ctx->packed_pck, cts);
		}
		pY = (u8*) (ctx->packed_data + idx_h + idx_w );

		if (chromat_format == YUV422) {
			pU = (u8*)(ctx->packed_data + 4 * ctx->stride  * ctx->height + idx_w / 2 + idx_h / 2);
			pV = (u8*)(ctx->packed_data + 4 * (3 * ctx->stride * ctx->height /2)  + idx_w / 2 + idx_h / 2);
		} else if (chromat_format == YUV444) {
			pU = (u8*)(ctx->packed_data + 4 * ctx->stride * ctx->height + idx_w + idx_h);
			pV = (u8*)(ctx->packed_data + 4 * ( 2 * ctx->stride * ctx->height) + idx_w + idx_h);
		} else {
			pU = (u8*)(ctx->packed_data + 2 * ctx->stride * 2 * ctx->height + idx_w / 2 + idx_h / 4);
			pV = (u8*)(ctx->packed_data + 4 * ( 5 *ctx->stride  * ctx->height  / 4) + idx_w / 2 + idx_h / 4);
		
		}

		if (libOpenHevcGetOutput(ctx->codec, 1, &openHFrame)) {
			u32 i, s_stride, hs_stride, qs_stride, d_stride, dd_stride, hd_stride;

			s_stride = openHFrame.frameInfo.nYPitch;
			qs_stride = s_stride / 4;
			hs_stride = s_stride / 2;

			d_stride = ctx->stride;
			dd_stride = 2*ctx->stride;
			hd_stride = ctx->stride/2;

			if (chromat_format == YUV422) {
				for (i = 0; i < ctx->height; i++) {
					memcpy(pY, (u8 *)openHFrame.pvY + i*s_stride, d_stride);
					pY += dd_stride;

					memcpy(pU, (u8 *)openHFrame.pvU + i*hs_stride, hd_stride);
					pU += d_stride;

					memcpy(pV, (u8 *)openHFrame.pvV + i*hs_stride, hd_stride);
					pV += d_stride;
				}
			} else if (chromat_format == YUV444) {
				for (i = 0; i < ctx->height; i++) {
					memcpy(pY, (u8 *)openHFrame.pvY + i*s_stride, d_stride);
					pY += dd_stride;

					memcpy(pU, (u8 *)openHFrame.pvU + i*s_stride, d_stride);
					pU += dd_stride;

					memcpy(pV, (u8 *)openHFrame.pvV + i*s_stride, d_stride);
					pV += dd_stride;
				}
			} else {
				for (i = 0; i<ctx->height; i++) {
					memcpy(pY, (u8 *)openHFrame.pvY + i*s_stride, d_stride);
					pY += dd_stride;

					if (!(i % 2)) {
						memcpy(pU, (u8 *)openHFrame.pvU + i*qs_stride, hd_stride);
						pU += d_stride;

						memcpy(pV, (u8 *)openHFrame.pvV + i*qs_stride, hd_stride);
						pV += d_stride;
					}
				}
			}

			ctx->frame_idx++;
			if (ctx->frame_idx==4) {
				gf_filter_pck_send(ctx->packed_pck);
				ctx->packed_pck = NULL;
				ctx->frame_idx = 0;
			}
		}
		return GF_OK;
	}

	pck = gf_filter_pck_new_alloc(ctx->opid, ctx->out_size, &data);

	openHevcFrame_FL.pvY = (void*) data;

	if (ctx->nb_layers==2 && ctx->is_multiview && !ctx->no_copy){
		int out1, out2;
		if( chromat_format == YUV420){
			openHevcFrame_SL.pvY = (void*) (data +  ctx->stride * ctx->height);
			openHevcFrame_FL.pvU = (void*) (data + 2*ctx->stride * ctx->height);
			openHevcFrame_SL.pvU = (void*) (data +  9*ctx->stride * ctx->height/4);
			openHevcFrame_FL.pvV = (void*) (data + 5*ctx->stride * ctx->height/2);
			openHevcFrame_SL.pvV = (void*) (data + 11*ctx->stride * ctx->height/4);
		}

		libOpenHevcSetViewLayers(ctx->codec, 0);
		out1 = libOpenHevcGetOutputCpy(ctx->codec, 1, &openHevcFrame_FL);
		libOpenHevcSetViewLayers(ctx->codec, 1);
		out2 = libOpenHevcGetOutputCpy(ctx->codec, 1, &openHevcFrame_SL);
		
		gf_filter_pck_set_cts(pck, cts);
		gf_filter_pck_send(pck);

	} else {
		openHevcFrame_FL.pvU = (void*) (data + ctx->stride * ctx->height);
		if( chromat_format == YUV420) {
			openHevcFrame_FL.pvV = (void*) (data + 5*ctx->stride * ctx->height/4);
		} else if (chromat_format == YUV422) {
			openHevcFrame_FL.pvV = (void*) (data + 3*ctx->stride * ctx->height/2);
		} else if ( chromat_format == YUV444) {
			openHevcFrame_FL.pvV = (void*) (data + 2*ctx->stride * ctx->height);
		}

		if (libOpenHevcGetOutputCpy(ctx->codec, 1, &openHevcFrame_FL)) {
			gf_filter_pck_set_cts(pck, cts);
			gf_filter_pck_send(pck);
		} else
			gf_filter_pck_discard(pck);
	}
	return GF_OK;
}

static void ohevcdec_drop_frameinfo(GF_OHEVCDecCtx *ctx)
{
	if (ctx->frame_infos_size) {
		ctx->frame_infos_size--;
		memmove(&ctx->frame_infos[0], &ctx->frame_infos[1], sizeof(OHEVCDecFrameInfo)*ctx->frame_infos_size);
	}
}

static GF_Err ohevcdec_process(GF_Filter *filter)
{
	s32 got_pic;
	u64 min_dts = GF_FILTER_NO_TS;
	u64 min_cts = GF_FILTER_NO_TS;
	u32 i, idx, nb_eos=0;
	u32 data_size;
	char *data;
	Bool has_pic = GF_FALSE;
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx*) gf_filter_get_udta(filter);

	if (ctx->frame_out) return GF_EOS;

	if (!ctx->decoder_started) {
		libOpenHevcStartDecoder(ctx->codec);
		ctx->decoder_started=1;
	}

	GF_FilterPacket *pck_ref = NULL;

	for (idx=0; idx<ctx->nb_streams; idx++) {
		u64 dts, cts;
		GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->streams[idx].ipid);
		if (!pck) {
			if (gf_filter_pid_is_eos(ctx->streams[idx].ipid)) nb_eos++;
			//make sure we do have a packet on the enhancement
			else {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[OpenSVC] no input packets on running pid %s - postponing decode\n", gf_filter_pid_get_name(ctx->streams[idx].ipid) ) );
				return GF_OK;
			}
			continue;
		}
		dts = gf_filter_pck_get_dts(pck);
		cts = gf_filter_pck_get_cts(pck);

		data = (char *) gf_filter_pck_get_data(pck, &data_size);
		//TODO: this is a clock signaling, for now just trash ..
		if (!data) {
			gf_filter_pid_drop_packet(ctx->streams[idx].ipid);
			idx--;
			continue;
		}
		if (dts==GF_FILTER_NO_TS) dts = cts;
		//get packet with min dts (either a timestamp or a decode order number)
		if (min_dts > dts) {
			min_dts = dts;
			if (cts == GF_FILTER_NO_TS) min_cts = min_dts;
			else min_cts = cts;
			pck_ref = pck;
		}
	}
	if (nb_eos == ctx->nb_streams) {
#ifdef  OPENHEVC_HAS_AVC_BASE
		if (ctx->avc_base_id)
			got_pic = libOpenShvcDecode2(ctx->codec, NULL, NULL, 0, 0, 0, 0);
		else
#endif
			got_pic = libOpenHevcDecode(ctx->codec, NULL, 0, 0);

		if ( got_pic ) {
			return ohevcdec_flush_picture(ctx);
		}
		gf_filter_pid_set_eos(ctx->opid);
		return GF_EOS;
	}

	if (min_cts == GF_FILTER_NO_TS) return GF_OK;


	if (ctx->frame_infos_size==ctx->frame_infos_alloc) {
		ctx->frame_infos_alloc += 10;
		ctx->frame_infos = gf_realloc(ctx->frame_infos, sizeof(OHEVCDecFrameInfo)*ctx->frame_infos_alloc);
	}
	//queue CTS
	if (!ctx->frame_infos_size || (ctx->frame_infos[ctx->frame_infos_size-1].cts != min_cts)) {
		for (i=0; i<ctx->frame_infos_size; i++) {
			//this is likel continuing decoding if we didn't get a frame in time from the enhancement layer
			if (ctx->frame_infos[i].cts == min_cts)
				break;

			if (ctx->frame_infos[i].cts > min_cts) {
				memmove(&ctx->frame_infos[i+1], &ctx->frame_infos[i], sizeof(OHEVCDecFrameInfo) * (ctx->frame_infos_size-i));
				ctx->frame_infos[i].cts = min_cts;
				ctx->frame_infos[i].duration = gf_filter_pck_get_duration(pck_ref);
				ctx->frame_infos[i].sap_type = gf_filter_pck_get_sap(pck_ref);
				ctx->frame_infos[i].seek_flag = gf_filter_pck_get_seek_flag(pck_ref);
				break;
			}
		}
	} else {
		i = ctx->frame_infos_size;
	}

	if (i==ctx->frame_infos_size) {
		ctx->frame_infos[i].cts = min_cts;
		ctx->frame_infos[i].duration = gf_filter_pck_get_duration(pck_ref);
		ctx->frame_infos[i].sap_type = gf_filter_pck_get_sap(pck_ref);
		ctx->frame_infos[i].seek_flag = gf_filter_pck_get_seek_flag(pck_ref);
	}
	ctx->frame_infos_size++;

	ctx->dec_frames++;
	got_pic = 0;

	for (idx=0; idx<ctx->nb_streams; idx++) {
		u64 dts, cts;

		GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->streams[idx].ipid);
		if (!pck) continue;

		if (idx>=ctx->nb_streams) {
			gf_filter_pid_drop_packet(ctx->streams[idx].ipid);
			continue;
		}

		dts = gf_filter_pck_get_dts(pck);
		cts = gf_filter_pck_get_cts(pck);
		if (dts==GF_FILTER_NO_TS) dts = cts;

		if (min_dts != GF_FILTER_NO_TS) {
			if (min_dts != dts) continue;
		} else if (min_cts != cts) {
			continue;
		}

		data = (char *) gf_filter_pck_get_data(pck, &data_size);

#ifdef  OPENHEVC_HAS_AVC_BASE
		if (ctx->avc_base_id) {
			if (ctx->avc_base_id == ctx->streams[idx].id) {
				got_pic = libOpenShvcDecode2(ctx->codec, (u8 *) data, NULL, data_size, 0, cts, 0);
			} else if (ctx->cur_layer>1) {
				got_pic = libOpenShvcDecode2(ctx->codec, (u8*)NULL, (u8 *) data, 0, data_size, 0, cts);
			}
		} else
#endif
			got_pic = libOpenHevcDecode(ctx->codec, (u8 *) data, data_size, cts);


		GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[HEVC Decoder] PID %s Decode CTS %d - size %d - got pic %d\n", gf_filter_pid_get_name(ctx->streams[idx].ipid), min_cts, data_size, got_pic));
		if (got_pic) has_pic = GF_TRUE;

		gf_filter_pid_drop_packet(ctx->streams[idx].ipid);

	}

	if (!has_pic) return GF_OK;

	return ohevcdec_flush_picture(ctx);
}

static GF_Err ohevcdec_initialize(GF_Filter *filter)
{
	GF_SystemRTInfo rti;
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx *) gf_filter_get_udta(filter);
	ctx->filter = filter;
	if (!ctx->nb_threads) {
		if (gf_sys_get_rti(0, &rti, 0) ) {
			ctx->nb_threads = (rti.nb_cores>1) ? rti.nb_cores-1 : 1;
			GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[OpenHEVCDec] Initializing with %d threads\n", ctx->nb_threads));
		}
	}
	return GF_OK;
}

static void ohevcdec_finalize(GF_Filter *filter)
{
	GF_OHEVCDecCtx *ctx = (GF_OHEVCDecCtx *) gf_filter_get_udta(filter);
	if (ctx->frame_infos) gf_free(ctx->frame_infos);
	if (ctx->codec) libOpenHevcClose(ctx->codec);
}

static const GF_FilterCapability OHEVCDecInputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_EXC_BOOL(GF_PROP_PID_UNFRAMED, GF_TRUE),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_VIDEO_HEVC),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_VIDEO_LHVC),
#ifdef  OPENHEVC_HAS_AVC_BASE
	{ .code=GF_PROP_PID_OTI, .val=PROP_UINT(GPAC_OTI_VIDEO_AVC), .in_bundle=1, .priority=255 }
#endif
};

static const GF_FilterCapability OHEVCDecOutputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_RAW_MEDIA_STREAM),
};

#define OFFS(_n)	#_n, offsetof(GF_OHEVCDecCtx, _n)

static const GF_FilterArgs OHEVCDecArgs[] =
{
	{ OFFS(threading), "Set threading mode", GF_PROP_UINT, "frame", "frameslice|frame|slice", GF_FALSE},
	{ OFFS(nb_threads), "Set number of threads. If 0, uses number of cores minus one", GF_PROP_UINT, "0", NULL, GF_TRUE},
	{ OFFS(no_copy), "Directly dispatch internal decoded frame without copy", GF_PROP_BOOL, "false", NULL, GF_TRUE},
	{ OFFS(pack_hfr), "Packs 4 consecutive frames in a single output", GF_PROP_BOOL, "false", NULL, GF_TRUE},
	{ OFFS(seek_reset), "Resets decoder when seeking", GF_PROP_BOOL, "false", NULL, GF_TRUE},
	{ OFFS(force_stereo), "Forces stereo output for multiview (top-bottom only)", GF_PROP_BOOL, "false", NULL, GF_TRUE},
	{}
};

GF_FilterRegister OHEVCDecRegister = {
	.name = "ohevc",
	.description = "OpenHEVC decoder",
	.private_size = sizeof(GF_OHEVCDecCtx),
	INCAPS(OHEVCDecInputs),
	OUTCAPS(OHEVCDecOutputs),
	.initialize = ohevcdec_initialize,
	.finalize = ohevcdec_finalize,
	.args = OHEVCDecArgs,
	.configure_pid = ohevcdec_configure_pid,
	.process = ohevcdec_process,
	.process_event = ohevcdec_process_event,
	.max_extra_pids = (HEVC_MAX_STREAMS-1),
	//by default take over FFMPEG
	.priority = 100
};

#endif // defined(GPAC_HAS_OPENHEVC) && !defined(GPAC_DISABLE_AV_PARSERS)

const GF_FilterRegister *ohevcdec_register(GF_FilterSession *session)
{
#if defined(GPAC_HAS_OPENHEVC) && !defined(GPAC_DISABLE_AV_PARSERS)
	return &OHEVCDecRegister;
#else
	return NULL;
#endif
}

