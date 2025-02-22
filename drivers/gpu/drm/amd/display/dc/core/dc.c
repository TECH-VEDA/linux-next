/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 */

#include "dm_services.h"

#include "dc.h"

#include "core_status.h"
#include "core_types.h"
#include "hw_sequencer.h"

#include "resource.h"

#include "clock_source.h"
#include "dc_bios_types.h"

#include "bios_parser_interface.h"
#include "include/irq_service_interface.h"
#include "transform.h"
#include "dpp.h"
#include "timing_generator.h"
#include "virtual/virtual_link_encoder.h"

#include "link_hwss.h"
#include "link_encoder.h"

#include "dc_link_ddc.h"
#include "dm_helpers.h"
#include "mem_input.h"
#include "hubp.h"


/*******************************************************************************
 * Private functions
 ******************************************************************************/
static void destroy_links(struct dc *dc)
{
	uint32_t i;

	for (i = 0; i < dc->link_count; i++) {
		if (NULL != dc->links[i])
			link_destroy(&dc->links[i]);
	}
}

static bool create_links(
		struct dc *dc,
		uint32_t num_virtual_links)
{
	int i;
	int connectors_num;
	struct dc_bios *bios = dc->ctx->dc_bios;

	dc->link_count = 0;

	connectors_num = bios->funcs->get_connectors_number(bios);

	if (connectors_num > ENUM_ID_COUNT) {
		dm_error(
			"DC: Number of connectors %d exceeds maximum of %d!\n",
			connectors_num,
			ENUM_ID_COUNT);
		return false;
	}

	if (connectors_num == 0 && num_virtual_links == 0) {
		dm_error("DC: Number of connectors is zero!\n");
	}

	dm_output_to_console(
		"DC: %s: connectors_num: physical:%d, virtual:%d\n",
		__func__,
		connectors_num,
		num_virtual_links);

	for (i = 0; i < connectors_num; i++) {
		struct link_init_data link_init_params = {0};
		struct dc_link *link;

		link_init_params.ctx = dc->ctx;
		/* next BIOS object table connector */
		link_init_params.connector_index = i;
		link_init_params.link_index = dc->link_count;
		link_init_params.dc = dc;
		link = link_create(&link_init_params);

		if (link) {
			dc->links[dc->link_count] = link;
			link->dc = dc;
			++dc->link_count;
		}
	}

	for (i = 0; i < num_virtual_links; i++) {
		struct dc_link *link = kzalloc(sizeof(*link), GFP_KERNEL);
		struct encoder_init_data enc_init = {0};

		if (link == NULL) {
			BREAK_TO_DEBUGGER();
			goto failed_alloc;
		}

		link->ctx = dc->ctx;
		link->dc = dc;
		link->connector_signal = SIGNAL_TYPE_VIRTUAL;
		link->link_id.type = OBJECT_TYPE_CONNECTOR;
		link->link_id.id = CONNECTOR_ID_VIRTUAL;
		link->link_id.enum_id = ENUM_ID_1;
		link->link_enc = kzalloc(sizeof(*link->link_enc), GFP_KERNEL);

		enc_init.ctx = dc->ctx;
		enc_init.channel = CHANNEL_ID_UNKNOWN;
		enc_init.hpd_source = HPD_SOURCEID_UNKNOWN;
		enc_init.transmitter = TRANSMITTER_UNKNOWN;
		enc_init.connector = link->link_id;
		enc_init.encoder.type = OBJECT_TYPE_ENCODER;
		enc_init.encoder.id = ENCODER_ID_INTERNAL_VIRTUAL;
		enc_init.encoder.enum_id = ENUM_ID_1;
		virtual_link_encoder_construct(link->link_enc, &enc_init);

		link->link_index = dc->link_count;
		dc->links[dc->link_count] = link;
		dc->link_count++;
	}

	return true;

failed_alloc:
	return false;
}

static bool stream_adjust_vmin_vmax(struct dc *dc,
		struct dc_stream_state **streams, int num_streams,
		int vmin, int vmax)
{
	/* TODO: Support multiple streams */
	struct dc_stream_state *stream = streams[0];
	int i = 0;
	bool ret = false;

	for (i = 0; i < MAX_PIPES; i++) {
		struct pipe_ctx *pipe = &dc->current_state->res_ctx.pipe_ctx[i];

		if (pipe->stream == stream && pipe->stream_res.stream_enc) {
			dc->hwss.set_drr(&pipe, 1, vmin, vmax);

			/* build and update the info frame */
			resource_build_info_frame(pipe);
			dc->hwss.update_info_frame(pipe);

			ret = true;
		}
	}
	return ret;
}

static bool stream_get_crtc_position(struct dc *dc,
		struct dc_stream_state **streams, int num_streams,
		unsigned int *v_pos, unsigned int *nom_v_pos)
{
	/* TODO: Support multiple streams */
	struct dc_stream_state *stream = streams[0];
	int i = 0;
	bool ret = false;
	struct crtc_position position;

	for (i = 0; i < MAX_PIPES; i++) {
		struct pipe_ctx *pipe =
				&dc->current_state->res_ctx.pipe_ctx[i];

		if (pipe->stream == stream && pipe->stream_res.stream_enc) {
			dc->hwss.get_position(&pipe, 1, &position);

			*v_pos = position.vertical_count;
			*nom_v_pos = position.nominal_vcount;
			ret = true;
		}
	}
	return ret;
}

static bool set_gamut_remap(struct dc *dc, const struct dc_stream_state *stream)
{
	int i = 0;
	bool ret = false;
	struct pipe_ctx *pipes;

	for (i = 0; i < MAX_PIPES; i++) {
		if (dc->current_state->res_ctx.pipe_ctx[i].stream == stream) {
			pipes = &dc->current_state->res_ctx.pipe_ctx[i];
			dc->hwss.program_gamut_remap(pipes);
			ret = true;
		}
	}

	return ret;
}

static bool program_csc_matrix(struct dc *dc, struct dc_stream_state *stream)
{
	int i = 0;
	bool ret = false;
	struct pipe_ctx *pipes;

	for (i = 0; i < MAX_PIPES; i++) {
		if (dc->current_state->res_ctx.pipe_ctx[i].stream
				== stream) {

			pipes = &dc->current_state->res_ctx.pipe_ctx[i];
			dc->hwss.program_csc_matrix(pipes,
			stream->output_color_space,
			stream->csc_color_matrix.matrix);
			ret = true;
		}
	}

	return ret;
}

static void set_static_screen_events(struct dc *dc,
		struct dc_stream_state **streams,
		int num_streams,
		const struct dc_static_screen_events *events)
{
	int i = 0;
	int j = 0;
	struct pipe_ctx *pipes_affected[MAX_PIPES];
	int num_pipes_affected = 0;

	for (i = 0; i < num_streams; i++) {
		struct dc_stream_state *stream = streams[i];

		for (j = 0; j < MAX_PIPES; j++) {
			if (dc->current_state->res_ctx.pipe_ctx[j].stream
					== stream) {
				pipes_affected[num_pipes_affected++] =
						&dc->current_state->res_ctx.pipe_ctx[j];
			}
		}
	}

	dc->hwss.set_static_screen_control(pipes_affected, num_pipes_affected, events);
}

static void set_drive_settings(struct dc *dc,
		struct link_training_settings *lt_settings,
		const struct dc_link *link)
{

	int i;

	for (i = 0; i < dc->link_count; i++) {
		if (dc->links[i] == link)
			break;
	}

	if (i >= dc->link_count)
		ASSERT_CRITICAL(false);

	dc_link_dp_set_drive_settings(dc->links[i], lt_settings);
}

static void perform_link_training(struct dc *dc,
		struct dc_link_settings *link_setting,
		bool skip_video_pattern)
{
	int i;

	for (i = 0; i < dc->link_count; i++)
		dc_link_dp_perform_link_training(
			dc->links[i],
			link_setting,
			skip_video_pattern);
}

static void set_preferred_link_settings(struct dc *dc,
		struct dc_link_settings *link_setting,
		struct dc_link *link)
{
	link->preferred_link_setting = *link_setting;
	dp_retrain_link_dp_test(link, link_setting, false);
}

static void enable_hpd(const struct dc_link *link)
{
	dc_link_dp_enable_hpd(link);
}

static void disable_hpd(const struct dc_link *link)
{
	dc_link_dp_disable_hpd(link);
}


static void set_test_pattern(
		struct dc_link *link,
		enum dp_test_pattern test_pattern,
		const struct link_training_settings *p_link_settings,
		const unsigned char *p_custom_pattern,
		unsigned int cust_pattern_size)
{
	if (link != NULL)
		dc_link_dp_set_test_pattern(
			link,
			test_pattern,
			p_link_settings,
			p_custom_pattern,
			cust_pattern_size);
}

static void set_dither_option(struct dc_stream_state *stream,
		enum dc_dither_option option)
{
	struct bit_depth_reduction_params params;
	struct dc_link *link = stream->status.link;
	struct pipe_ctx *pipes = NULL;
	int i;

	for (i = 0; i < MAX_PIPES; i++) {
		if (link->dc->current_state->res_ctx.pipe_ctx[i].stream ==
				stream) {
			pipes = &link->dc->current_state->res_ctx.pipe_ctx[i];
			break;
		}
	}

	memset(&params, 0, sizeof(params));
	if (!pipes)
		return;
	if (option > DITHER_OPTION_MAX)
		return;

	stream->dither_option = option;

	resource_build_bit_depth_reduction_params(stream,
				&params);
	stream->bit_depth_params = params;
	pipes->stream_res.opp->funcs->
		opp_program_bit_depth_reduction(pipes->stream_res.opp, &params);
}

void set_dpms(
	struct dc *dc,
	struct dc_stream_state *stream,
	bool dpms_off)
{
	struct pipe_ctx *pipe_ctx = NULL;
	int i;

	for (i = 0; i < MAX_PIPES; i++) {
		if (dc->current_state->res_ctx.pipe_ctx[i].stream == stream) {
			pipe_ctx = &dc->current_state->res_ctx.pipe_ctx[i];
			break;
		}
	}

	if (!pipe_ctx) {
		ASSERT(0);
		return;
	}

	if (stream->dpms_off != dpms_off) {
		stream->dpms_off = dpms_off;
		if (dpms_off)
			core_link_disable_stream(pipe_ctx,
					KEEP_ACQUIRED_RESOURCE);
		else
			core_link_enable_stream(dc->current_state, pipe_ctx);
	}
}

static void allocate_dc_stream_funcs(struct dc  *dc)
{
	if (dc->hwss.set_drr != NULL) {
		dc->stream_funcs.adjust_vmin_vmax =
				stream_adjust_vmin_vmax;
	}

	dc->stream_funcs.set_static_screen_events =
			set_static_screen_events;

	dc->stream_funcs.get_crtc_position =
			stream_get_crtc_position;

	dc->stream_funcs.set_gamut_remap =
			set_gamut_remap;

	dc->stream_funcs.program_csc_matrix =
			program_csc_matrix;

	dc->stream_funcs.set_dither_option =
			set_dither_option;

	dc->stream_funcs.set_dpms =
			set_dpms;

	dc->link_funcs.set_drive_settings =
			set_drive_settings;

	dc->link_funcs.perform_link_training =
			perform_link_training;

	dc->link_funcs.set_preferred_link_settings =
			set_preferred_link_settings;

	dc->link_funcs.enable_hpd =
			enable_hpd;

	dc->link_funcs.disable_hpd =
			disable_hpd;

	dc->link_funcs.set_test_pattern =
			set_test_pattern;
}

static void destruct(struct dc *dc)
{
	dc_release_state(dc->current_state);
	dc->current_state = NULL;

	destroy_links(dc);

	dc_destroy_resource_pool(dc);

	if (dc->ctx->gpio_service)
		dal_gpio_service_destroy(&dc->ctx->gpio_service);

	if (dc->ctx->i2caux)
		dal_i2caux_destroy(&dc->ctx->i2caux);

	if (dc->ctx->created_bios)
		dal_bios_parser_destroy(&dc->ctx->dc_bios);

	if (dc->ctx->logger)
		dal_logger_destroy(&dc->ctx->logger);

	kfree(dc->ctx);
	dc->ctx = NULL;

	kfree(dc->bw_vbios);
	dc->bw_vbios = NULL;

	kfree(dc->bw_dceip);
	dc->bw_dceip = NULL;

#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	kfree(dc->dcn_soc);
	dc->dcn_soc = NULL;

	kfree(dc->dcn_ip);
	dc->dcn_ip = NULL;

#endif
}

static bool construct(struct dc *dc,
		const struct dc_init_data *init_params)
{
	struct dal_logger *logger;
	struct dc_context *dc_ctx = kzalloc(sizeof(*dc_ctx), GFP_KERNEL);
	struct bw_calcs_dceip *dc_dceip = kzalloc(sizeof(*dc_dceip),
						  GFP_KERNEL);
	struct bw_calcs_vbios *dc_vbios = kzalloc(sizeof(*dc_vbios),
						  GFP_KERNEL);
#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	struct dcn_soc_bounding_box *dcn_soc = kzalloc(sizeof(*dcn_soc),
						       GFP_KERNEL);
	struct dcn_ip_params *dcn_ip = kzalloc(sizeof(*dcn_ip), GFP_KERNEL);
#endif

	enum dce_version dc_version = DCE_VERSION_UNKNOWN;

	if (!dc_dceip) {
		dm_error("%s: failed to create dceip\n", __func__);
		goto fail;
	}

	dc->bw_dceip = dc_dceip;

	if (!dc_vbios) {
		dm_error("%s: failed to create vbios\n", __func__);
		goto fail;
	}

	dc->bw_vbios = dc_vbios;
#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	if (!dcn_soc) {
		dm_error("%s: failed to create dcn_soc\n", __func__);
		goto fail;
	}

	dc->dcn_soc = dcn_soc;

	if (!dcn_ip) {
		dm_error("%s: failed to create dcn_ip\n", __func__);
		goto fail;
	}

	dc->dcn_ip = dcn_ip;
#endif

	if (!dc_ctx) {
		dm_error("%s: failed to create ctx\n", __func__);
		goto fail;
	}

	dc->current_state = dc_create_state();

	if (!dc->current_state) {
		dm_error("%s: failed to create validate ctx\n", __func__);
		goto fail;
	}

	dc_ctx->cgs_device = init_params->cgs_device;
	dc_ctx->driver_context = init_params->driver;
	dc_ctx->dc = dc;
	dc_ctx->asic_id = init_params->asic_id;

	/* Create logger */
	logger = dal_logger_create(dc_ctx, init_params->log_mask);

	if (!logger) {
		/* can *not* call logger. call base driver 'print error' */
		dm_error("%s: failed to create Logger!\n", __func__);
		goto fail;
	}
	dc_ctx->logger = logger;
	dc->ctx = dc_ctx;
	dc->ctx->dce_environment = init_params->dce_environment;

	dc_version = resource_parse_asic_id(init_params->asic_id);
	dc->ctx->dce_version = dc_version;
#if defined(CONFIG_DRM_AMD_DC_FBC)
	dc->ctx->fbc_gpu_addr = init_params->fbc_gpu_addr;
#endif
	/* Resource should construct all asic specific resources.
	 * This should be the only place where we need to parse the asic id
	 */
	if (init_params->vbios_override)
		dc_ctx->dc_bios = init_params->vbios_override;
	else {
		/* Create BIOS parser */
		struct bp_init_data bp_init_data;

		bp_init_data.ctx = dc_ctx;
		bp_init_data.bios = init_params->asic_id.atombios_base_address;

		dc_ctx->dc_bios = dal_bios_parser_create(
				&bp_init_data, dc_version);

		if (!dc_ctx->dc_bios) {
			ASSERT_CRITICAL(false);
			goto fail;
		}

		dc_ctx->created_bios = true;
		}

	/* Create I2C AUX */
	dc_ctx->i2caux = dal_i2caux_create(dc_ctx);

	if (!dc_ctx->i2caux) {
		ASSERT_CRITICAL(false);
		goto fail;
	}

	/* Create GPIO service */
	dc_ctx->gpio_service = dal_gpio_service_create(
			dc_version,
			dc_ctx->dce_environment,
			dc_ctx);

	if (!dc_ctx->gpio_service) {
		ASSERT_CRITICAL(false);
		goto fail;
	}

	dc->res_pool = dc_create_resource_pool(
			dc,
			init_params->num_virtual_links,
			dc_version,
			init_params->asic_id);
	if (!dc->res_pool)
		goto fail;

	dc_resource_state_construct(dc, dc->current_state);

	if (!create_links(dc, init_params->num_virtual_links))
		goto fail;

	allocate_dc_stream_funcs(dc);

	return true;

fail:

	destruct(dc);
	return false;
}

/*******************************************************************************
 * Public functions
 ******************************************************************************/

struct dc *dc_create(const struct dc_init_data *init_params)
 {
	struct dc *dc = kzalloc(sizeof(*dc), GFP_KERNEL);
	unsigned int full_pipe_count;

	if (NULL == dc)
		goto alloc_fail;

	if (false == construct(dc, init_params))
		goto construct_fail;

	/*TODO: separate HW and SW initialization*/
	dc->hwss.init_hw(dc);

	full_pipe_count = dc->res_pool->pipe_count;
	if (dc->res_pool->underlay_pipe_index != NO_UNDERLAY_PIPE)
		full_pipe_count--;
	dc->caps.max_streams = min(
			full_pipe_count,
			dc->res_pool->stream_enc_count);

	dc->caps.max_links = dc->link_count;
	dc->caps.max_audios = dc->res_pool->audio_count;

	dc->config = init_params->flags;

	dm_logger_write(dc->ctx->logger, LOG_DC,
			"Display Core initialized\n");


	/* TODO: missing feature to be enabled */
	dc->debug.disable_dfs_bypass = true;

	return dc;

construct_fail:
	kfree(dc);

alloc_fail:
	return NULL;
}

void dc_destroy(struct dc **dc)
{
	destruct(*dc);
	kfree(*dc);
	*dc = NULL;
}

static void program_timing_sync(
		struct dc *dc,
		struct dc_state *ctx)
{
	int i, j;
	int group_index = 0;
	int pipe_count = dc->res_pool->pipe_count;
	struct pipe_ctx *unsynced_pipes[MAX_PIPES] = { NULL };

	for (i = 0; i < pipe_count; i++) {
		if (!ctx->res_ctx.pipe_ctx[i].stream || ctx->res_ctx.pipe_ctx[i].top_pipe)
			continue;

		unsynced_pipes[i] = &ctx->res_ctx.pipe_ctx[i];
	}

	for (i = 0; i < pipe_count; i++) {
		int group_size = 1;
		struct pipe_ctx *pipe_set[MAX_PIPES];

		if (!unsynced_pipes[i])
			continue;

		pipe_set[0] = unsynced_pipes[i];
		unsynced_pipes[i] = NULL;

		/* Add tg to the set, search rest of the tg's for ones with
		 * same timing, add all tgs with same timing to the group
		 */
		for (j = i + 1; j < pipe_count; j++) {
			if (!unsynced_pipes[j])
				continue;

			if (resource_are_streams_timing_synchronizable(
					unsynced_pipes[j]->stream,
					pipe_set[0]->stream)) {
				pipe_set[group_size] = unsynced_pipes[j];
				unsynced_pipes[j] = NULL;
				group_size++;
			}
		}

		/* set first unblanked pipe as master */
		for (j = 0; j < group_size; j++) {
			struct pipe_ctx *temp;

			if (!pipe_set[j]->stream_res.tg->funcs->is_blanked(pipe_set[j]->stream_res.tg)) {
				if (j == 0)
					break;

				temp = pipe_set[0];
				pipe_set[0] = pipe_set[j];
				pipe_set[j] = temp;
				break;
			}
		}

		/* remove any other unblanked pipes as they have already been synced */
		for (j = j + 1; j < group_size; j++) {
			if (!pipe_set[j]->stream_res.tg->funcs->is_blanked(pipe_set[j]->stream_res.tg)) {
				group_size--;
				pipe_set[j] = pipe_set[group_size];
				j--;
			}
		}

		if (group_size > 1) {
			dc->hwss.enable_timing_synchronization(
				dc, group_index, group_size, pipe_set);
			group_index++;
		}
	}
}

static bool context_changed(
		struct dc *dc,
		struct dc_state *context)
{
	uint8_t i;

	if (context->stream_count != dc->current_state->stream_count)
		return true;

	for (i = 0; i < dc->current_state->stream_count; i++) {
		if (dc->current_state->streams[i] != context->streams[i])
			return true;
	}

	return false;
}

bool dc_enable_stereo(
	struct dc *dc,
	struct dc_state *context,
	struct dc_stream_state *streams[],
	uint8_t stream_count)
{
	bool ret = true;
	int i, j;
	struct pipe_ctx *pipe;

	for (i = 0; i < MAX_PIPES; i++) {
		if (context != NULL)
			pipe = &context->res_ctx.pipe_ctx[i];
		else
			pipe = &dc->current_state->res_ctx.pipe_ctx[i];
		for (j = 0 ; pipe && j < stream_count; j++)  {
			if (streams[j] && streams[j] == pipe->stream &&
				dc->hwss.setup_stereo)
				dc->hwss.setup_stereo(pipe, dc);
		}
	}

	return ret;
}


/*
 * Applies given context to HW and copy it into current context.
 * It's up to the user to release the src context afterwards.
 */
static enum dc_status dc_commit_state_no_check(struct dc *dc, struct dc_state *context)
{
	struct dc_bios *dcb = dc->ctx->dc_bios;
	enum dc_status result = DC_ERROR_UNEXPECTED;
	struct pipe_ctx *pipe;
	int i, j, k, l;
	struct dc_stream_state *dc_streams[MAX_STREAMS] = {0};

	for (i = 0; i < context->stream_count; i++)
		dc_streams[i] =  context->streams[i];

	if (!dcb->funcs->is_accelerated_mode(dcb))
		dc->hwss.enable_accelerated_mode(dc);

	for (i = 0; i < context->stream_count; i++) {
		const struct dc_sink *sink = context->streams[i]->sink;

		dc->hwss.apply_ctx_for_surface(
				dc, context->streams[i],
				context->stream_status[i].plane_count,
				context);

		/*
		 * enable stereo
		 * TODO rework dc_enable_stereo call to work with validation sets?
		 */
		for (k = 0; k < MAX_PIPES; k++) {
			pipe = &context->res_ctx.pipe_ctx[k];

			for (l = 0 ; pipe && l < context->stream_count; l++)  {
				if (context->streams[l] &&
					context->streams[l] == pipe->stream &&
					dc->hwss.setup_stereo)
					dc->hwss.setup_stereo(pipe, dc);
			}
		}



		CONN_MSG_MODE(sink->link, "{%dx%d, %dx%d@%dKhz}",
				context->streams[i]->timing.h_addressable,
				context->streams[i]->timing.v_addressable,
				context->streams[i]->timing.h_total,
				context->streams[i]->timing.v_total,
				context->streams[i]->timing.pix_clk_khz);
	}

	dc->hwss.ready_shared_resources(dc, context);

	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		pipe = &context->res_ctx.pipe_ctx[i];
		dc->hwss.wait_for_mpcc_disconnect(dc, dc->res_pool, pipe);
	}
	result = dc->hwss.apply_ctx_to_hw(dc, context);

	program_timing_sync(dc, context);

	dc_enable_stereo(dc, context, dc_streams, context->stream_count);

	for (i = 0; i < context->stream_count; i++) {
		for (j = 0; j < MAX_PIPES; j++) {
			pipe = &context->res_ctx.pipe_ctx[j];

			if (!pipe->top_pipe && pipe->stream == context->streams[i])
				dc->hwss.pipe_control_lock(dc, pipe, false);
		}
	}

	dc_release_state(dc->current_state);

	dc->current_state = context;

	dc_retain_state(dc->current_state);

	dc->hwss.optimize_shared_resources(dc);

	return result;
}

bool dc_commit_state(struct dc *dc, struct dc_state *context)
{
	enum dc_status result = DC_ERROR_UNEXPECTED;
	int i;

	if (false == context_changed(dc, context))
		return DC_OK;

	dm_logger_write(dc->ctx->logger, LOG_DC, "%s: %d streams\n",
				__func__, context->stream_count);

	for (i = 0; i < context->stream_count; i++) {
		struct dc_stream_state *stream = context->streams[i];

		dc_stream_log(stream,
				dc->ctx->logger,
				LOG_DC);
	}

	result = dc_commit_state_no_check(dc, context);

	return (result == DC_OK);
}


bool dc_post_update_surfaces_to_stream(struct dc *dc)
{
	int i;
	struct dc_state *context = dc->current_state;

	post_surface_trace(dc);

	for (i = 0; i < dc->res_pool->pipe_count; i++)
		if (context->res_ctx.pipe_ctx[i].stream == NULL
				|| context->res_ctx.pipe_ctx[i].plane_state == NULL)
			dc->hwss.power_down_front_end(dc, i);

	/* 3rd param should be true, temp w/a for RV*/
#if defined(CONFIG_DRM_AMD_DC_DCN1_0)
	dc->hwss.set_bandwidth(dc, context, dc->ctx->dce_version < DCN_VERSION_1_0);
#else
	dc->hwss.set_bandwidth(dc, context, true);
#endif
	return true;
}

/*
 * TODO this whole function needs to go
 *
 * dc_surface_update is needlessly complex. See if we can just replace this
 * with a dc_plane_state and follow the atomic model a bit more closely here.
 */
bool dc_commit_planes_to_stream(
		struct dc *dc,
		struct dc_plane_state **plane_states,
		uint8_t new_plane_count,
		struct dc_stream_state *dc_stream,
		struct dc_state *state)
{
	/* no need to dynamically allocate this. it's pretty small */
	struct dc_surface_update updates[MAX_SURFACES];
	struct dc_flip_addrs *flip_addr;
	struct dc_plane_info *plane_info;
	struct dc_scaling_info *scaling_info;
	int i;
	struct dc_stream_update *stream_update =
			kzalloc(sizeof(struct dc_stream_update), GFP_KERNEL);

	if (!stream_update) {
		BREAK_TO_DEBUGGER();
		return false;
	}

	flip_addr = kcalloc(MAX_SURFACES, sizeof(struct dc_flip_addrs),
			    GFP_KERNEL);
	plane_info = kcalloc(MAX_SURFACES, sizeof(struct dc_plane_info),
			     GFP_KERNEL);
	scaling_info = kcalloc(MAX_SURFACES, sizeof(struct dc_scaling_info),
			       GFP_KERNEL);

	memset(updates, 0, sizeof(updates));

	stream_update->src = dc_stream->src;
	stream_update->dst = dc_stream->dst;
	stream_update->out_transfer_func = dc_stream->out_transfer_func;

	for (i = 0; i < new_plane_count; i++) {
		updates[i].surface = plane_states[i];
		updates[i].gamma =
			(struct dc_gamma *)plane_states[i]->gamma_correction;
		updates[i].in_transfer_func = plane_states[i]->in_transfer_func;
		flip_addr[i].address = plane_states[i]->address;
		flip_addr[i].flip_immediate = plane_states[i]->flip_immediate;
		plane_info[i].color_space = plane_states[i]->color_space;
		plane_info[i].format = plane_states[i]->format;
		plane_info[i].plane_size = plane_states[i]->plane_size;
		plane_info[i].rotation = plane_states[i]->rotation;
		plane_info[i].horizontal_mirror = plane_states[i]->horizontal_mirror;
		plane_info[i].stereo_format = plane_states[i]->stereo_format;
		plane_info[i].tiling_info = plane_states[i]->tiling_info;
		plane_info[i].visible = plane_states[i]->visible;
		plane_info[i].per_pixel_alpha = plane_states[i]->per_pixel_alpha;
		plane_info[i].dcc = plane_states[i]->dcc;
		scaling_info[i].scaling_quality = plane_states[i]->scaling_quality;
		scaling_info[i].src_rect = plane_states[i]->src_rect;
		scaling_info[i].dst_rect = plane_states[i]->dst_rect;
		scaling_info[i].clip_rect = plane_states[i]->clip_rect;

		updates[i].flip_addr = &flip_addr[i];
		updates[i].plane_info = &plane_info[i];
		updates[i].scaling_info = &scaling_info[i];
	}

	dc_commit_updates_for_stream(
			dc,
			updates,
			new_plane_count,
			dc_stream, stream_update, plane_states, state);

	kfree(flip_addr);
	kfree(plane_info);
	kfree(scaling_info);
	kfree(stream_update);
	return true;
}

struct dc_state *dc_create_state(void)
{
	struct dc_state *context = kzalloc(sizeof(struct dc_state),
					   GFP_KERNEL);

	if (!context)
		return NULL;

	kref_init(&context->refcount);
	return context;
}

void dc_retain_state(struct dc_state *context)
{
	kref_get(&context->refcount);
}

static void dc_state_free(struct kref *kref)
{
	struct dc_state *context = container_of(kref, struct dc_state, refcount);
	dc_resource_state_destruct(context);
	kfree(context);
}

void dc_release_state(struct dc_state *context)
{
	kref_put(&context->refcount, dc_state_free);
}

static bool is_surface_in_context(
		const struct dc_state *context,
		const struct dc_plane_state *plane_state)
{
	int j;

	for (j = 0; j < MAX_PIPES; j++) {
		const struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[j];

		if (plane_state == pipe_ctx->plane_state) {
			return true;
		}
	}

	return false;
}

static unsigned int pixel_format_to_bpp(enum surface_pixel_format format)
{
	switch (format) {
	case SURFACE_PIXEL_FORMAT_VIDEO_420_YCbCr:
	case SURFACE_PIXEL_FORMAT_VIDEO_420_YCrCb:
		return 12;
	case SURFACE_PIXEL_FORMAT_GRPH_ARGB1555:
	case SURFACE_PIXEL_FORMAT_GRPH_RGB565:
	case SURFACE_PIXEL_FORMAT_VIDEO_420_10bpc_YCbCr:
	case SURFACE_PIXEL_FORMAT_VIDEO_420_10bpc_YCrCb:
		return 16;
	case SURFACE_PIXEL_FORMAT_GRPH_ARGB8888:
	case SURFACE_PIXEL_FORMAT_GRPH_ABGR8888:
	case SURFACE_PIXEL_FORMAT_GRPH_ARGB2101010:
	case SURFACE_PIXEL_FORMAT_GRPH_ABGR2101010:
		return 32;
	case SURFACE_PIXEL_FORMAT_GRPH_ARGB16161616:
	case SURFACE_PIXEL_FORMAT_GRPH_ARGB16161616F:
	case SURFACE_PIXEL_FORMAT_GRPH_ABGR16161616F:
		return 64;
	default:
		ASSERT_CRITICAL(false);
		return -1;
	}
}

static enum surface_update_type get_plane_info_update_type(
		const struct dc_surface_update *u,
		int surface_index)
{
	struct dc_plane_info temp_plane_info;
	memset(&temp_plane_info, 0, sizeof(temp_plane_info));

	if (!u->plane_info)
		return UPDATE_TYPE_FAST;

	temp_plane_info = *u->plane_info;

	/* Copy all parameters that will cause a full update
	 * from current surface, the rest of the parameters
	 * from provided plane configuration.
	 * Perform memory compare and special validation
	 * for those that can cause fast/medium updates
	 */

	/* Full update parameters */
	temp_plane_info.color_space = u->surface->color_space;
	temp_plane_info.dcc = u->surface->dcc;
	temp_plane_info.horizontal_mirror = u->surface->horizontal_mirror;
	temp_plane_info.plane_size = u->surface->plane_size;
	temp_plane_info.rotation = u->surface->rotation;
	temp_plane_info.stereo_format = u->surface->stereo_format;

	if (surface_index == 0)
		temp_plane_info.visible = u->plane_info->visible;
	else
		temp_plane_info.visible = u->surface->visible;

	if (memcmp(u->plane_info, &temp_plane_info,
			sizeof(struct dc_plane_info)) != 0)
		return UPDATE_TYPE_FULL;

	if (pixel_format_to_bpp(u->plane_info->format) !=
			pixel_format_to_bpp(u->surface->format)) {
		/* different bytes per element will require full bandwidth
		 * and DML calculation
		 */
		return UPDATE_TYPE_FULL;
	}

	if (memcmp(&u->plane_info->tiling_info, &u->surface->tiling_info,
			sizeof(union dc_tiling_info)) != 0) {
		/* todo: below are HW dependent, we should add a hook to
		 * DCE/N resource and validated there.
		 */
		if (u->plane_info->tiling_info.gfx9.swizzle != DC_SW_LINEAR) {
			/* swizzled mode requires RQ to be setup properly,
			 * thus need to run DML to calculate RQ settings
			 */
			return UPDATE_TYPE_FULL;
		}
	}

	return UPDATE_TYPE_MED;
}

static enum surface_update_type  get_scaling_info_update_type(
		const struct dc_surface_update *u)
{
	if (!u->scaling_info)
		return UPDATE_TYPE_FAST;

	if (u->scaling_info->src_rect.width != u->surface->src_rect.width
			|| u->scaling_info->src_rect.height != u->surface->src_rect.height
			|| u->scaling_info->clip_rect.width != u->surface->clip_rect.width
			|| u->scaling_info->clip_rect.height != u->surface->clip_rect.height
			|| u->scaling_info->dst_rect.width != u->surface->dst_rect.width
			|| u->scaling_info->dst_rect.height != u->surface->dst_rect.height)
		return UPDATE_TYPE_FULL;

	if (u->scaling_info->src_rect.x != u->surface->src_rect.x
			|| u->scaling_info->src_rect.y != u->surface->src_rect.y
			|| u->scaling_info->clip_rect.x != u->surface->clip_rect.x
			|| u->scaling_info->clip_rect.y != u->surface->clip_rect.y
			|| u->scaling_info->dst_rect.x != u->surface->dst_rect.x
			|| u->scaling_info->dst_rect.y != u->surface->dst_rect.y)
		return UPDATE_TYPE_MED;

	return UPDATE_TYPE_FAST;
}

static enum surface_update_type det_surface_update(
		const struct dc *dc,
		const struct dc_surface_update *u,
		int surface_index)
{
	const struct dc_state *context = dc->current_state;
	enum surface_update_type type = UPDATE_TYPE_FAST;
	enum surface_update_type overall_type = UPDATE_TYPE_FAST;

	if (!is_surface_in_context(context, u->surface))
		return UPDATE_TYPE_FULL;

	type = get_plane_info_update_type(u, surface_index);
	if (overall_type < type)
		overall_type = type;

	type = get_scaling_info_update_type(u);
	if (overall_type < type)
		overall_type = type;

	if (u->in_transfer_func ||
		u->hdr_static_metadata) {
		if (overall_type < UPDATE_TYPE_MED)
			overall_type = UPDATE_TYPE_MED;
	}

	return overall_type;
}

enum surface_update_type dc_check_update_surfaces_for_stream(
		struct dc *dc,
		struct dc_surface_update *updates,
		int surface_count,
		struct dc_stream_update *stream_update,
		const struct dc_stream_status *stream_status)
{
	int i;
	enum surface_update_type overall_type = UPDATE_TYPE_FAST;

	if (stream_status == NULL || stream_status->plane_count != surface_count)
		return UPDATE_TYPE_FULL;

	if (stream_update)
		return UPDATE_TYPE_FULL;

	for (i = 0 ; i < surface_count; i++) {
		enum surface_update_type type =
				det_surface_update(dc, &updates[i], i);

		if (type == UPDATE_TYPE_FULL)
			return type;

		if (overall_type < type)
			overall_type = type;
	}

	return overall_type;
}

static struct dc_stream_status *stream_get_status(
	struct dc_state *ctx,
	struct dc_stream_state *stream)
{
	uint8_t i;

	for (i = 0; i < ctx->stream_count; i++) {
		if (stream == ctx->streams[i]) {
			return &ctx->stream_status[i];
		}
	}

	return NULL;
}

static const enum surface_update_type update_surface_trace_level = UPDATE_TYPE_FULL;


static void commit_planes_for_stream(struct dc *dc,
		struct dc_surface_update *srf_updates,
		int surface_count,
		struct dc_stream_state *stream,
		struct dc_stream_update *stream_update,
		enum surface_update_type update_type,
		struct dc_state *context)
{
	int i, j;

	if (update_type == UPDATE_TYPE_FULL) {
		dc->hwss.set_bandwidth(dc, context, false);
		context_clock_trace(dc, context);
	}

	if (update_type > UPDATE_TYPE_FAST) {
		for (j = 0; j < dc->res_pool->pipe_count; j++) {
			struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[j];

			dc->hwss.wait_for_mpcc_disconnect(dc, dc->res_pool, pipe_ctx);
		}
	}

	if (surface_count == 0) {
		/*
		 * In case of turning off screen, no need to program front end a second time.
		 * just return after program front end.
		 */
		dc->hwss.apply_ctx_for_surface(dc, stream, surface_count, context);
		return;
	}

	/* Lock pipes for provided surfaces, or all active if full update*/
	for (i = 0; i < surface_count; i++) {
		struct dc_plane_state *plane_state = srf_updates[i].surface;

		for (j = 0; j < dc->res_pool->pipe_count; j++) {
			struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[j];

			if (update_type != UPDATE_TYPE_FULL && pipe_ctx->plane_state != plane_state)
				continue;
			if (!pipe_ctx->plane_state || pipe_ctx->top_pipe)
				continue;

			dc->hwss.pipe_control_lock(
					dc,
					pipe_ctx,
					true);
		}
		if (update_type == UPDATE_TYPE_FULL)
			break;
	}

	/* Full fe update*/
	for (j = 0; j < dc->res_pool->pipe_count; j++) {
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[j];

		if (update_type != UPDATE_TYPE_FULL || !pipe_ctx->plane_state)
			continue;

		if (!pipe_ctx->top_pipe && pipe_ctx->stream) {
			struct dc_stream_status *stream_status = stream_get_status(context, pipe_ctx->stream);

			dc->hwss.apply_ctx_for_surface(
					dc, pipe_ctx->stream, stream_status->plane_count, context);
		}
	}

	if (update_type > UPDATE_TYPE_FAST)
		context_timing_trace(dc, &context->res_ctx);

	/* Perform requested Updates */
	for (i = 0; i < surface_count; i++) {
		struct dc_plane_state *plane_state = srf_updates[i].surface;

		if (update_type == UPDATE_TYPE_MED)
			dc->hwss.apply_ctx_for_surface(
					dc, stream, surface_count, context);

		for (j = 0; j < dc->res_pool->pipe_count; j++) {
			struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[j];

			if (pipe_ctx->plane_state != plane_state)
				continue;

			if (srf_updates[i].flip_addr)
				dc->hwss.update_plane_addr(dc, pipe_ctx);

			if (update_type == UPDATE_TYPE_FAST)
				continue;

			/* work around to program degamma regs for split pipe after set mode. */
			if (srf_updates[i].in_transfer_func || (pipe_ctx->top_pipe &&
					pipe_ctx->top_pipe->plane_state == pipe_ctx->plane_state))
				dc->hwss.set_input_transfer_func(
						pipe_ctx, pipe_ctx->plane_state);

			if (stream_update != NULL &&
					stream_update->out_transfer_func != NULL) {
				dc->hwss.set_output_transfer_func(
						pipe_ctx, pipe_ctx->stream);
			}

			if (srf_updates[i].hdr_static_metadata) {
				resource_build_info_frame(pipe_ctx);
				dc->hwss.update_info_frame(pipe_ctx);
			}
		}
	}

	/* Unlock pipes */
	for (i = dc->res_pool->pipe_count - 1; i >= 0; i--) {
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];

		for (j = 0; j < surface_count; j++) {
			if (update_type != UPDATE_TYPE_FULL &&
			    srf_updates[j].surface != pipe_ctx->plane_state)
				continue;
			if (!pipe_ctx->plane_state || pipe_ctx->top_pipe)
				continue;

			dc->hwss.pipe_control_lock(
					dc,
					pipe_ctx,
					false);

			break;
		}
	}
}

void dc_commit_updates_for_stream(struct dc *dc,
		struct dc_surface_update *srf_updates,
		int surface_count,
		struct dc_stream_state *stream,
		struct dc_stream_update *stream_update,
		struct dc_plane_state **plane_states,
		struct dc_state *state)
{
	const struct dc_stream_status *stream_status;
	enum surface_update_type update_type;
	struct dc_state *context;
	struct dc_context *dc_ctx = dc->ctx;
	int i, j;

	stream_status = dc_stream_get_status(stream);
	context = dc->current_state;

	update_type = dc_check_update_surfaces_for_stream(
				dc, srf_updates, surface_count, stream_update, stream_status);

	if (update_type >= update_surface_trace_level)
		update_surface_trace(dc, srf_updates, surface_count);


	if (update_type >= UPDATE_TYPE_FULL) {

		/* initialize scratch memory for building context */
		context = dc_create_state();
		if (context == NULL) {
			DC_ERROR("Failed to allocate new validate context!\n");
			return;
		}

		dc_resource_state_copy_construct(state, context);
	}


	for (i = 0; i < surface_count; i++) {
		struct dc_plane_state *surface = srf_updates[i].surface;

		/* TODO: On flip we don't build the state, so it still has the
		 * old address. Which is why we are updating the address here
		 */
		if (srf_updates[i].flip_addr)
			surface->address = srf_updates[i].flip_addr->address;

		if (update_type >= UPDATE_TYPE_MED) {
			for (j = 0; j < dc->res_pool->pipe_count; j++) {
				struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[j];

				if (pipe_ctx->plane_state != surface)
					continue;

				resource_build_scaling_params(pipe_ctx);
			}
		}
	}

	commit_planes_for_stream(
				dc,
				srf_updates,
				surface_count,
				stream,
				stream_update,
				update_type,
				context);

	if (update_type >= UPDATE_TYPE_FULL)
		dc_post_update_surfaces_to_stream(dc);

	if (dc->current_state != context) {

		struct dc_state *old = dc->current_state;

		dc->current_state = context;
		dc_release_state(old);

	}

	return;

}

uint8_t dc_get_current_stream_count(struct dc *dc)
{
	return dc->current_state->stream_count;
}

struct dc_stream_state *dc_get_stream_at_index(struct dc *dc, uint8_t i)
{
	if (i < dc->current_state->stream_count)
		return dc->current_state->streams[i];
	return NULL;
}

enum dc_irq_source dc_interrupt_to_irq_source(
		struct dc *dc,
		uint32_t src_id,
		uint32_t ext_id)
{
	return dal_irq_service_to_irq_source(dc->res_pool->irqs, src_id, ext_id);
}

void dc_interrupt_set(struct dc *dc, enum dc_irq_source src, bool enable)
{

	if (dc == NULL)
		return;

	dal_irq_service_set(dc->res_pool->irqs, src, enable);
}

void dc_interrupt_ack(struct dc *dc, enum dc_irq_source src)
{
	dal_irq_service_ack(dc->res_pool->irqs, src);
}

void dc_set_power_state(
	struct dc *dc,
	enum dc_acpi_cm_power_state power_state)
{
	struct kref refcount;

	switch (power_state) {
	case DC_ACPI_CM_POWER_STATE_D0:
		dc_resource_state_construct(dc, dc->current_state);

		dc->hwss.init_hw(dc);
		break;
	default:

		dc->hwss.power_down(dc);

		/* Zero out the current context so that on resume we start with
		 * clean state, and dc hw programming optimizations will not
		 * cause any trouble.
		 */

		/* Preserve refcount */
		refcount = dc->current_state->refcount;
		dc_resource_state_destruct(dc->current_state);
		memset(dc->current_state, 0,
				sizeof(*dc->current_state));

		dc->current_state->refcount = refcount;

		break;
	}

}

void dc_resume(struct dc *dc)
{

	uint32_t i;

	for (i = 0; i < dc->link_count; i++)
		core_link_resume(dc->links[i]);
}

bool dc_submit_i2c(
		struct dc *dc,
		uint32_t link_index,
		struct i2c_command *cmd)
{

	struct dc_link *link = dc->links[link_index];
	struct ddc_service *ddc = link->ddc;

	return dal_i2caux_submit_i2c_command(
		ddc->ctx->i2caux,
		ddc->ddc_pin,
		cmd);
}

static bool link_add_remote_sink_helper(struct dc_link *dc_link, struct dc_sink *sink)
{
	if (dc_link->sink_count >= MAX_SINKS_PER_LINK) {
		BREAK_TO_DEBUGGER();
		return false;
	}

	dc_sink_retain(sink);

	dc_link->remote_sinks[dc_link->sink_count] = sink;
	dc_link->sink_count++;

	return true;
}

struct dc_sink *dc_link_add_remote_sink(
		struct dc_link *link,
		const uint8_t *edid,
		int len,
		struct dc_sink_init_data *init_data)
{
	struct dc_sink *dc_sink;
	enum dc_edid_status edid_status;

	if (len > MAX_EDID_BUFFER_SIZE) {
		dm_error("Max EDID buffer size breached!\n");
		return NULL;
	}

	if (!init_data) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	if (!init_data->link) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	dc_sink = dc_sink_create(init_data);

	if (!dc_sink)
		return NULL;

	memmove(dc_sink->dc_edid.raw_edid, edid, len);
	dc_sink->dc_edid.length = len;

	if (!link_add_remote_sink_helper(
			link,
			dc_sink))
		goto fail_add_sink;

	edid_status = dm_helpers_parse_edid_caps(
			link->ctx,
			&dc_sink->dc_edid,
			&dc_sink->edid_caps);

	if (edid_status != EDID_OK)
		goto fail;

	return dc_sink;
fail:
	dc_link_remove_remote_sink(link, dc_sink);
fail_add_sink:
	dc_sink_release(dc_sink);
	return NULL;
}

void dc_link_remove_remote_sink(struct dc_link *link, struct dc_sink *sink)
{
	int i;

	if (!link->sink_count) {
		BREAK_TO_DEBUGGER();
		return;
	}

	for (i = 0; i < link->sink_count; i++) {
		if (link->remote_sinks[i] == sink) {
			dc_sink_release(sink);
			link->remote_sinks[i] = NULL;

			/* shrink array to remove empty place */
			while (i < link->sink_count - 1) {
				link->remote_sinks[i] = link->remote_sinks[i+1];
				i++;
			}
			link->remote_sinks[i] = NULL;
			link->sink_count--;
			return;
		}
	}
}
