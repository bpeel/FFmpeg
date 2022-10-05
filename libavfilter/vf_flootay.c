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
#include "libavfilter/colorspace.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/file_open.h"
#include "libavutil/colorspace.h"
#include "avfilter.h"
#include "internal.h"
#include "formats.h"
#include "video.h"

typedef struct FlootayContext {
    const AVClass *class;
    struct flootay *flootay;
    char *filename;
    cairo_surface_t *surface;
    bool surface_is_clear;
    cairo_t *cr;
    double rgb2yuv[3][3];
    double y_multiply, y_add;
    double uv_multiply, uv_add;
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

static int config_input(AVFilterLink *inlink)
{
    FlootayContext *flt = inlink->dst->priv;

    flt->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                              inlink->w, inlink->h);
    flt->cr = cairo_create(flt->surface);

    flt->surface_is_clear = false;

    return 0;
}

static int blend_surface_rgb(FlootayContext *flt, AVFrame *picref)
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

    return 0;
}

static void rgb_to_yuv(FlootayContext *flt,
                       const uint8_t *src,
                       int src_stride,
                       uint8_t y_out[4],
                       uint8_t a[4],
                       uint8_t *u,
                       uint8_t *v)
{
    double rgbd[3], yuvd[3];
    int u_sum = 0, v_sum = 0;
    uint32_t src_pixel;
    int i = 0;

    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            memcpy(&src_pixel, src + x * 4 + y * src_stride, sizeof src_pixel);
            a[i] = src_pixel >> 24;
            rgbd[0] = ((src_pixel >> 16) & 0xff) / 255.0;
            rgbd[1] = ((src_pixel >> 8) & 0xff) / 255.0;
            rgbd[2] = (src_pixel & 0xff) / 255.0;
            /* unpremultiply */
            if (a[i] != 0) {
                for (int c = 0; c < 3; c++)
                    rgbd[c] = rgbd[c] * 255 / a[i];
            }
            ff_matrix_mul_3x3_vec(yuvd, rgbd, flt->rgb2yuv);
            yuvd[0] *= flt->y_multiply;
            yuvd[0] += flt->y_add;
            yuvd[1] *= flt->uv_multiply;
            yuvd[1] += flt->uv_add;
            yuvd[2] *= flt->uv_multiply;
            yuvd[2] += flt->uv_add;

            y_out[i] = yuvd[0] * 255.0 + 0.5f;
            u_sum += yuvd[1] * 255.0 + 0.5f;
            v_sum += yuvd[2] * 255.0 + 0.5f;
            i++;
        }
    }

    *u = u_sum / 4;
    *v = v_sum / 4;
}

static void blend_component(uint8_t *dst, uint8_t src, uint8_t a)
{
    *dst = (*dst * (255 - a) + src * a) / 255;
}

static void blend_y(uint8_t *dst,
                    int dst_stride,
                    const uint8_t y_comp[4],
                    const uint8_t a[4])
{
    int i = 0;

    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            blend_component(dst + x, y_comp[i], a[i]);
            i++;
        }
        dst += dst_stride;
    }
}

static int init_rgb2yuv(FlootayContext *flt, AVFrame *picref)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(picref->format);
    enum AVColorSpace csp = picref->colorspace;
    const AVLumaCoefficients *luma;

    if (!desc)
        return AVERROR(EINVAL);

    if (csp == AVCOL_SPC_UNSPECIFIED)
        csp = AVCOL_SPC_SMPTE170M;

    luma = av_csp_luma_coeffs_from_avcsp(csp);

    if (!luma)
        return AVERROR(EINVAL);

    ff_fill_rgb2yuv_table(luma, flt->rgb2yuv);

    if (picref->color_range == AVCOL_RANGE_MPEG) {
        flt->y_multiply = 219.0 / 255.0;
        flt->y_add = 16.0 / 255.0;
        flt->uv_multiply = 224.0 / 255.0;
        flt->uv_add = 128.0 / 255.0;
    } else {
        flt->y_multiply = 1.0;
        flt->y_add = 0.0;
        flt->uv_multiply = 1.0;
        flt->uv_add = 0.5;
    }

    return 0;
}

static int blend_surface_yuv(FlootayContext *flt, AVFrame *picref)
{
    const uint8_t *src = cairo_image_surface_get_data(flt->surface);
    int src_stride = cairo_image_surface_get_stride(flt->surface);
    uint8_t *dst_y = picref->data[0];
    int dst_y_stride = picref->linesize[0];
    uint8_t *dst_u = picref->data[1];
    int dst_u_stride = picref->linesize[1];
    uint8_t *dst_v = picref->data[2];
    int dst_v_stride = picref->linesize[2];
    int width = picref->width;
    int height = picref->height;
    uint8_t y_comp[4], a[4], u, v;
    int a_avg;
    int ret;

    if ((ret = init_rgb2yuv(flt, picref)))
        return ret;

    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            rgb_to_yuv(flt, src, src_stride, y_comp, a, &u, &v);

            blend_y(dst_y, dst_y_stride, y_comp, a);

            a_avg = 0;
            for (int i = 0; i < 4; i++)
                a_avg += a[i];
            a_avg /= 4;

            blend_component(dst_u, u, a_avg);
            blend_component(dst_v, v, a_avg);

            src += 4 * 2;
            dst_y += 2;
            dst_u++;
            dst_v++;
        }

        src += src_stride * 2 - width * 4;
        dst_y += dst_y_stride * 2 - width;
        dst_u += dst_u_stride - width / 2;
        dst_v += dst_v_stride - width / 2;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *picref)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FlootayContext *flt = ctx->priv;
    double timestamp = picref->pts * av_q2d(inlink->time_base);
    int ret;

    if (!flt->surface_is_clear) {
        cairo_save(flt->cr);
        cairo_set_source_rgba(flt->cr, 0.0, 0.0, 0.0, 0.0);
        cairo_set_operator(flt->cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(flt->cr);
        cairo_restore(flt->cr);

        flt->surface_is_clear = true;
    }

    switch (flootay_render(flt->flootay,
                           flt->cr,
                           timestamp)) {
    case FLOOTAY_RENDER_RESULT_ERROR:
        av_log(ctx,
               AV_LOG_ERROR,
               "Flootay rendering failed: %s\n",
               flootay_get_error(flt->flootay));
        return AVERROR_EXTERNAL;

    case FLOOTAY_RENDER_RESULT_EMPTY:
        /* Don’t need to blend the result */
        break;

    case FLOOTAY_RENDER_RESULT_OK:
        flt->surface_is_clear = false;

        cairo_surface_flush(flt->surface);

        if (picref->format == AV_PIX_FMT_BGR24)
            ret = blend_surface_rgb(flt, picref);
        else
            ret = blend_surface_yuv(flt, picref);

        if (ret)
            return ret;
    }

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

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_BGR24,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_NONE
};

const AVFilter ff_vf_flootay = {
    .name          = "flootay",
    .description   = NULL_IF_CONFIG_SMALL("Render a flootay script onto input video."),
    .priv_size     = sizeof(FlootayContext),
    .init          = init_flt,
    .uninit        = uninit,
    FILTER_INPUTS(flt_inputs),
    FILTER_OUTPUTS(flt_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .priv_class    = &flt_class,
};
