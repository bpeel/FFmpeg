/*
 * Copyright (c) 2022 Neil Roberts
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Flootay scripted overlay
 *
 * @see{https://github.com/bpeel/flootay}
 */

#include <flootay.h>
#include <stdint.h>

#include "config.h"
#include "config_components.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/file_open.h"
#include "avfilter.h"
#include "internal.h"
#include "formats.h"
#include "video.h"

typedef struct FlootayContext {
    const AVClass *class;
    struct flootay *flootay;
    char *filename;
    cairo_surface_t *surface;
    cairo_t *cr;
} FlootayContext;

#define OFFSET(x) offsetof(FlootayContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

#define COMMON_OPTIONS \
    {"filename",       "set the filename of file to read",                         OFFSET(filename),   AV_OPT_TYPE_STRING,     {.str = NULL},  0, 0, FLAGS }, \
    {"f",              "set the filename of file to read",                         OFFSET(filename),   AV_OPT_TYPE_STRING,     {.str = NULL},  0, 0, FLAGS }, \

static av_cold int init(AVFilterContext *ctx)
{
    FlootayContext *flt = ctx->priv;

    if (!flt->filename) {
        av_log(ctx, AV_LOG_ERROR, "No filename provided!\n");
        return AVERROR(EINVAL);
    }

    flt->flootay = flootay_new();

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FlootayContext *flt = ctx->priv;

    if (flt->flootay)
        flootay_free(flt->flootay);

    if (flt->cr)
        cairo_destroy(flt->cr);

    if (flt->surface)
        cairo_surface_destroy(flt->surface);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *fmts = NULL;
    int ret;

    if ((ret = ff_add_format(&fmts, AV_PIX_FMT_BGR24)) < 0)
        return ret;

    return ff_set_common_formats(ctx, fmts);
}

static int config_input(AVFilterLink *inlink)
{
    FlootayContext *flt = inlink->dst->priv;

    flt->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                              inlink->w, inlink->h);
    flt->cr = cairo_create(flt->surface);

    return 0;
}

static void blend_surface(FlootayContext *flt, AVFrame *picref)
{
    const uint8_t *src = cairo_image_surface_get_data(flt->surface);
    int src_stride = cairo_image_surface_get_stride(flt->surface);
    uint8_t *dst = picref->data[0];
    int dst_stride = picref->linesize[0];
    int width = picref->width;
    int height = picref->height;
    uint32_t src_pixel;
    uint8_t alpha;
    int x, y, i, c;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            memcpy(&src_pixel, src, sizeof src_pixel);
            alpha = src_pixel >> 24;
            for (i = 0; i < 3; i++) {
                c = (src_pixel & 0xff) * 255;
                src_pixel >>= 8;
                *dst = (*dst * (255 - alpha) + c) / 255;
                dst++;
            }
            src += 4;
        }

        src += src_stride - width * 4;
        dst += dst_stride - width * 3;
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *picref)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FlootayContext *flt = ctx->priv;
    double timestamp = picref->pts * av_q2d(inlink->time_base);

    if (!flootay_render(flt->flootay,
                        flt->cr,
                        timestamp)) {
        av_log(ctx,
               AV_LOG_ERROR,
               "Flootay rendering failed: %s\n",
               flootay_get_error(flt->flootay));
        return AVERROR_EXTERNAL;
    }

    cairo_surface_flush(flt->surface);

    blend_surface(flt, picref);

    return ff_filter_frame(outlink, picref);
}

static const AVFilterPad flt_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .flags            = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
        .filter_frame     = filter_frame,
        .config_props     = config_input,
    },
};

static const AVFilterPad flt_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVOption flt_options[] = {
    COMMON_OPTIONS
    {NULL},
};

AVFILTER_DEFINE_CLASS(flt);

static av_cold int init_flt(AVFilterContext *ctx)
{
    FlootayContext *flt = ctx->priv;
    int ret = init(ctx);
    bool load_ret;
    char *base_dir;
    const char *last_part;
    FILE *file;

    if (ret < 0)
        return ret;

    file = avpriv_fopen_utf8(flt->filename, "r");

    if (file == NULL) {
        ret = AVERROR(errno);
        av_log(ctx,
               AV_LOG_ERROR,
               "Unable to open flootay script \"%s\": %s\n",
               flt->filename,
               av_err2str(ret));

        return ret;
    }

    last_part = strrchr(flt->filename, '/');

    if (last_part == NULL)
        base_dir = NULL;
    else
        base_dir = av_strndup(flt->filename, last_part - flt->filename);

    load_ret = flootay_load_script(flt->flootay,
                                   base_dir,
                                   file);

    av_free(base_dir);

    fclose(file);

    if (!load_ret) {
        av_log(ctx,
               AV_LOG_ERROR,
               "Error loading %s: %s\n",
               flt->filename,
               flootay_get_error(flt->flootay));
        return AVERROR_EXTERNAL;
    }

    return 0;
}

const AVFilter ff_vf_flootay = {
    .name          = "flootay",
    .description   = NULL_IF_CONFIG_SMALL("Render a flootay script onto input video."),
    .priv_size     = sizeof(FlootayContext),
    .init          = init_flt,
    .uninit        = uninit,
    FILTER_INPUTS(flt_inputs),
    FILTER_OUTPUTS(flt_outputs),
    FILTER_QUERY_FUNC(query_formats),
    .priv_class    = &flt_class,
};
