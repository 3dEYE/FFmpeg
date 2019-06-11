#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"

typedef struct NullContext {
    const AVClass *class;
    AVRational frame_rate;
    int64_t previous_pts;
} FrameRateContext;

#define OFFSET(x) offsetof(FrameRateContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM

static const AVOption framerateexact_options[] = {
    { "fps_exact", "set frame rate",  OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, V|F },
    { NULL },
};

AVFILTER_DEFINE_CLASS(framerateexact);

static av_cold int init(AVFilterContext *ctx)
{
    FrameRateContext *frc = ctx->priv;

    frc->previous_pts = -1;
    
    return 0;
}

static int config_output_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FrameRateContext *frc = ctx->priv;

    outlink->time_base  = av_inv_q(frc->frame_rate);
    outlink->frame_rate = frc->frame_rate;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *ref)
{
    FrameRateContext *frc = inlink->dst->priv;

    if (frc->previous_pts == -1 || (ref->pts - frc->previous_pts) * 1000 * inlink->time_base.num / inlink->time_base.den >= 1000 * frc->frame_rate.den / frc->frame_rate.num) {
        frc->previous_pts = ref->pts;
        return ff_filter_frame(inlink->dst->outputs[0], ref);
    } else {
        av_frame_free(&ref);
        return 0;
    }
}

static const AVFilterPad framerateexact_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad framerateexact_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output_props,
    },
    { NULL }
};

AVFilter ff_vf_framerateexact = {
    .name        = "fps_exact",
    .description = NULL_IF_CONFIG_SMALL("Simple frame rate filter with timestamp saving logic"),
    .priv_size   = sizeof(FrameRateContext),
    .priv_class  = &framerateexact_class,
    .init        = init,
    .inputs      = framerateexact_inputs,
    .outputs     = framerateexact_outputs,
    .flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
