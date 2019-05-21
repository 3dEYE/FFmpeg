#include "avio_internal.h"
#include "internal.h"

static int write_packet(AVFormatContext *s, AVPacket *pkt)
{   
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

static const AVClass stream2shm_muxer_class = {
    .class_name = "stream2shm muxer",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

#if CONFIG_STREAM2SHM_MUXER

AVOutputFormat ff_stream2shm_muxer = {
    .name           = "stream2shm",
    .long_name      = NULL_IF_CONFIG_SMALL("shared memory stream sequence"),
    .video_codec    = AV_CODEC_ID_MJPEG,
    .write_packet   = write_packet,
    .flags          = AVFMT_TS_NONSTRICT,
    .priv_class     = &stream2shm_muxer_class
};
#endif
