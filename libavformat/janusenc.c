/*
 * Janus muxer
 * Copyright (c) 2018 3DEYE
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

#include "avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/opt.h"
#include "libavcodec/xiph.h"
#include "libavcodec/mpeg4audio.h"
#include "network.h"
#include "os_support.h"
#include "rtsp.h"
#include "rtpenc_chain.h"
#include "internal.h"
#include "avio_internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "libavutil/time.h"
#include "avc.h"
#include "hevc.h"
#include "url.h"
#include <pthread.h>

#define MAX_EXTRADATA_SIZE ((INT_MAX - 10) / 2)

typedef struct JanusState {
  const AVClass *class; /**< Class for private options. */
  AVFormatContext *video_rtpctx;
  AVFormatContext *audio_rtpctx;
  int video_stream_index;
  int audio_stream_index;
  char *mountpoint_id;
  char *mountpoint_pin;
  char *admin_key;
  int mountpoint_is_private;
  uint8_t *extradata;
  uint8_t *extradata_copy;
  int extradata_size;
  pthread_t janus_thread;
  volatile int janus_thread_terminated;
  AVIOInterruptCB thread_terminate_cb;
  volatile int video_port;
  volatile int audio_port;
  volatile int reconnect;
  int wait_i_frame;
} JanusState;

#define OFFSET(x) offsetof(JanusState, x)
#define E AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "mountpoint_id", "Janus mount point id", OFFSET(mountpoint_id), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { "mountpoint_pin", "Janus mount point pin code", OFFSET(mountpoint_pin), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { "mountpoint_private", "Janus mount point should be private", OFFSET(mountpoint_is_private), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, E },
    { "admin_key", "Janus API key", OFFSET(admin_key), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { NULL }
};

static const AVClass janus_muxer_class = {
    .class_name = "Janus muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int interrupt_cb(void *ctx)
{
  JanusState *js = (JanusState *)ctx;
  return js->janus_thread_terminated;
}

static char *read_json_value(const char *str, const char *field, char *value_buffer, int value_buffer_size)
{
  char buf[1024];
  char *field_start_pos;
  char *field_end_pos;
  int length;
  
  snprintf(buf, sizeof(buf), "\"%s\"", field);

  field_start_pos = strstr(str, buf);
  
  if(field_start_pos == NULL)
    return NULL;

  field_start_pos += strlen(buf);
  while(*field_start_pos != ':')
  {
    if(*field_start_pos == '\0')
       return NULL;

     ++field_start_pos;
  }

  ++field_start_pos;

  while(*field_start_pos == ' ' || *field_start_pos == '\"')
  {
    if(*field_start_pos == '\0')
       return NULL;

    ++field_start_pos;
  }

  field_end_pos = field_start_pos;

  while(*field_end_pos != ',' &&
        *field_end_pos != '\"' &&
        *field_end_pos != '\r' &&
        *field_end_pos != '\n' &&
        *field_end_pos != '}')
  {
    if(*field_start_pos == '\0')
       return NULL;

    ++field_end_pos;
  }

  length = field_end_pos - field_start_pos;
  memcpy_s(value_buffer, value_buffer_size, field_start_pos, length);

  if(length < value_buffer_size)
    value_buffer[length] = '\0';
  else
    value_buffer[value_buffer_size - 1] = '\0';

  return value_buffer;
}

static int open_http_context(AVFormatContext *s, URLContext **h, AVIOInterruptCB *interrupt_callback)
{
    char proto[32];
    char hostname[512];
    int port;
    int ret;
    URLContext *urlctx;
    char buf[2048];
    char headers[512];
    void *priv_data;
    
    av_url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname), &port, NULL, 0, s->filename);

    if (port < 0)
    {
       if(strcmpi(proto, "https") == 0)
         port = 443;
       else
         port = 80;
    }
    
    ff_url_join(buf, sizeof(buf), proto, NULL, hostname, port, NULL);
 
    if ((ret = ffurl_alloc(h, buf, AVIO_FLAG_READ_WRITE, interrupt_callback)) < 0)
        return ret;
 
    urlctx = *h;
    if (!urlctx->protocol_whitelist && s->protocol_whitelist) {
        urlctx->protocol_whitelist = av_strdup(s->protocol_whitelist);
        if (!urlctx->protocol_whitelist) {
            ret = AVERROR(ENOMEM);
            return ret;
        }
    }

    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\n");

    priv_data = urlctx->priv_data;

    av_opt_set(priv_data, "headers", headers, 0);
 
    return 0;
}

static int send_http_json_request(AVFormatContext *s, const char *path, const char *json_request, char *json_response, int json_response_max_size)
{
   char proto[32];
   char hostname[512];
   char buf[2048];
   int port;
   int ret;
   char *status;
   int request_length;
   URLContext *h;
   JanusState *js = s->priv_data;

   if((ret = open_http_context(s, &h, &js->thread_terminate_cb)) < 0)
     return ret;

   av_url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname), &port, NULL, 0, s->filename);

   ff_url_join(buf, sizeof(buf), proto, NULL, hostname, port, "%s", path);

   request_length = strlen(json_request);
   av_opt_set_bin(h->priv_data, "post_data", json_request, request_length, 0);

   if ((ret = ff_http_do_new_request(h, buf)) < 0)
       goto fail;
   
   if ((ret = ffurl_read(h, json_response, json_response_max_size - 1)) < 0)
      goto fail;
   
   json_response[ret] = '\0';

   status = read_json_value(json_response, "janus", buf, sizeof(buf));

   if(status == NULL)
   {
      av_log(s, AV_LOG_ERROR, "Status is not found in response\n");
      ret = -1;
      goto fail;
   }

   if(strcmpi(status, "success") != 0)
   {
      av_log(s, AV_LOG_ERROR, "Server error response: %s\n", status);
      ret = -2;
      goto fail;
   }
   else
      ret = 0;
fail:
   ff_http_close(h);
   ffurl_close(h);
   return ret;
}

static void fill_rtp_map_info(char *buff, int size, int payload_type, AVStream *st, AVFormatContext *fmt)
{
    AVCodecParameters *p = st->codecpar;

    switch (p->codec_id) {
        case AV_CODEC_ID_DIRAC:
            strncpy(buff, "VC2/90000", size);
            break;
        case AV_CODEC_ID_H264: {
            strncpy(buff, "H264/90000", size);
            break;
        }
        case AV_CODEC_ID_H261:
        {
            strncpy(buff, "H261/90000", size);
            break;
        }
        case AV_CODEC_ID_H263:
        case AV_CODEC_ID_H263P:
            strncpy(buff, "H263-2000/90000", size);
            break;
        case AV_CODEC_ID_HEVC:
            strncpy(buff, "H265/90000", size);
            break;
        case AV_CODEC_ID_MPEG4:
            strncpy(buff, "MP4V-ES/90000", size);
            break;
        case AV_CODEC_ID_AAC:
            if (fmt && fmt->oformat && fmt->oformat->priv_class && av_opt_flag_is_set(fmt->priv_data, "rtpflags", "latm")) {
                snprintf(buff, size, "MP4A-LATM/%d/%d",
                                         p->sample_rate, p->channels);
            } else {
                snprintf(buff, size, "MPEG4-GENERIC/%d/%d",
                                         p->sample_rate, p->channels);
            }
            break;
        case AV_CODEC_ID_PCM_S16BE:
            snprintf(buff, size, "L16/%d/%d",
                                    p->sample_rate, p->channels);
            break;
        case AV_CODEC_ID_PCM_MULAW:
             snprintf(buff, size, "PCMU/%d/%d",
                                    p->sample_rate, p->channels);
            break;
        case AV_CODEC_ID_PCM_ALAW:
             snprintf(buff, size, "PCMA/%d/%d",
                                    p->sample_rate, p->channels);
            break;
        case AV_CODEC_ID_AMR_NB:
            snprintf(buff, size, "AMR/%d/%d",
                                     p->sample_rate, p->channels);
            break;
        case AV_CODEC_ID_AMR_WB:
            snprintf(buff, size, "AMR-WB/%d/%d",
                                     p->sample_rate, p->channels);
            break;
        case AV_CODEC_ID_VORBIS:
            snprintf(buff, size, "vorbis/%d/%d",
                                    p->sample_rate, p->channels);
            break;
        case AV_CODEC_ID_THEORA: {
            snprintf(buff, size, "theora/90000");
            break;
        }

        case AV_CODEC_ID_MJPEG:
            snprintf(buff, size, "JPEG/90000");
            break;

        case AV_CODEC_ID_ADPCM_G722:
            snprintf(buff, size, "G722/%d/%d", 8000, p->channels);
            break;
        case AV_CODEC_ID_ADPCM_G726: {
            snprintf(buff, size, "G726-%d/%d",
                                   p->bits_per_coded_sample*8,
                                   p->sample_rate);
            break;
        }
        case AV_CODEC_ID_ILBC:
            snprintf(buff, size, "iLBC/%d",
                                     p->sample_rate);
            break;
        case AV_CODEC_ID_SPEEX:
            snprintf(buff, size, "speex/%d", p->sample_rate);
            break;
        case AV_CODEC_ID_OPUS:
            snprintf(buff, size, "opus/48000/2");
            break;
        case AV_CODEC_ID_VP8:
            strncpy(buff, "VP8/90000", size);
            break;
        case AV_CODEC_ID_VP9:
            strncpy(buff, "VP9/90000", size);
            break;
        default:
            *buff = '\0';
            break;
    }
}

#define MAX_PSET_SIZE 1024
static char *extradata2psets(AVFormatContext *s, AVCodecParameters *par)
{
    char *psets, *p;
    const uint8_t *r;
    static const char pset_string[] = "; sprop-parameter-sets=";
    static const char profile_string[] = "; profile-level-id=";
    uint8_t *extradata = par->extradata;
    int extradata_size = par->extradata_size;
    uint8_t *tmpbuf = NULL;
    const uint8_t *sps = NULL, *sps_end;

    if (par->extradata_size > MAX_EXTRADATA_SIZE) {
        av_log(s, AV_LOG_ERROR, "Too much extradata!\n");

        return NULL;
    }
    if (par->extradata[0] == 1) {
        if (ff_avc_write_annexb_extradata(par->extradata, &extradata,
                                          &extradata_size))
            return NULL;
        tmpbuf = extradata;
    }

    psets = av_mallocz(MAX_PSET_SIZE);
    if (!psets) {
        av_log(s, AV_LOG_ERROR, "Cannot allocate memory for the parameter sets.\n");
        av_free(tmpbuf);
        return NULL;
    }
    memcpy(psets, pset_string, strlen(pset_string));
    p = psets + strlen(pset_string);
    r = ff_avc_find_startcode(extradata, extradata + extradata_size);
    while (r < extradata + extradata_size) {
        const uint8_t *r1;
        uint8_t nal_type;

        while (!*(r++));
        nal_type = *r & 0x1f;
        r1 = ff_avc_find_startcode(r, extradata + extradata_size);
        if (nal_type != 7 && nal_type != 8) { /* Only output SPS and PPS */
            r = r1;
            continue;
        }
        if (p != (psets + strlen(pset_string))) {
            *p = ',';
            p++;
        }
        if (!sps) {
            sps = r;
            sps_end = r1;
        }
        if (!av_base64_encode(p, MAX_PSET_SIZE - (p - psets), r, r1 - r)) {
            av_log(s, AV_LOG_ERROR, "Cannot Base64-encode %"PTRDIFF_SPECIFIER" %"PTRDIFF_SPECIFIER"!\n", MAX_PSET_SIZE - (p - psets), r1 - r);
            av_free(psets);
            av_free(tmpbuf);

            return NULL;
        }
        p += strlen(p);
        r = r1;
    }
    if (sps && sps_end - sps >= 4) {
        memcpy(p, profile_string, strlen(profile_string));
        p += strlen(p);
        char sps_fixed[3];
        //hack to avoid problems with firefox
        sps_fixed[0]=0x42;
        sps_fixed[1]=0xe0;
        sps_fixed[2]=sps[3];
        ff_data_to_hex(p, sps_fixed, 3, 0);
        p[6] = '\0';
    }
    av_free(tmpbuf);

    return psets;
}

static char *extradata2psets_hevc(AVCodecParameters *par)
{
    char *psets;
    uint8_t *extradata = par->extradata;
    int extradata_size = par->extradata_size;
    uint8_t *tmpbuf = NULL;
    int ps_pos[3] = { 0 };
    static const char * const ps_names[3] = { "vps", "sps", "pps" };
    int num_arrays, num_nalus;
    int pos, i, j;

    // Convert to hvcc format. Since we need to group multiple NALUs of
    // the same type, and we might need to convert from one format to the
    // other anyway, we get away with a little less work by using the hvcc
    // format.
    if (par->extradata[0] != 1) {
        AVIOContext *pb;
        if (avio_open_dyn_buf(&pb) < 0)
            return NULL;
        if (ff_isom_write_hvcc(pb, par->extradata, par->extradata_size, 0) < 0) {
            avio_close_dyn_buf(pb, &tmpbuf);
            goto err;
        }
        extradata_size = avio_close_dyn_buf(pb, &extradata);
        tmpbuf = extradata;
    }

    if (extradata_size < 23)
        goto err;

    num_arrays = extradata[22];
    pos = 23;
    for (i = 0; i < num_arrays; i++) {
        int num_nalus, nalu_type;
        if (pos + 3 > extradata_size)
            goto err;
        nalu_type = extradata[pos] & 0x3f;
        // Not including libavcodec/hevc.h to avoid confusion between
        // NAL_* with the same name for both H.264 and HEVC.
        if (nalu_type == 32) // VPS
            ps_pos[0] = pos;
        else if (nalu_type == 33) // SPS
            ps_pos[1] = pos;
        else if (nalu_type == 34) // PPS
            ps_pos[2] = pos;
        num_nalus = AV_RB16(&extradata[pos + 1]);
        pos += 3;
        for (j = 0; j < num_nalus; j++) {
            int len;
            if (pos + 2 > extradata_size)
                goto err;
            len = AV_RB16(&extradata[pos]);
            pos += 2;
            if (pos + len > extradata_size)
                goto err;
            pos += len;
        }
    }
    if (!ps_pos[0] || !ps_pos[1] || !ps_pos[2])
        goto err;

    psets = av_mallocz(MAX_PSET_SIZE);
    if (!psets)
        goto err;
    psets[0] = '\0';

    for (i = 0; i < 3; i++) {
        pos = ps_pos[i];

        if (i > 0)
            av_strlcat(psets, "; ", MAX_PSET_SIZE);
        av_strlcatf(psets, MAX_PSET_SIZE, "sprop-%s=", ps_names[i]);

        // Skipping boundary checks in the input here; we've already traversed
        // the whole hvcc structure above without issues
        num_nalus = AV_RB16(&extradata[pos + 1]);
        pos += 3;
        for (j = 0; j < num_nalus; j++) {
            int len = AV_RB16(&extradata[pos]);
            int strpos;
            pos += 2;
            if (j > 0)
                av_strlcat(psets, ",", MAX_PSET_SIZE);
            strpos = strlen(psets);
            if (!av_base64_encode(psets + strpos, MAX_PSET_SIZE - strpos,
                                  &extradata[pos], len)) {
                av_free(psets);
                goto err;
            }
            pos += len;
        }
    }
    av_free(tmpbuf);
    return psets;
err:
    av_free(tmpbuf);
    return NULL;
}

static char *extradata2config(AVFormatContext *s, AVCodecParameters *par)
{
    char *config;

    if (par->extradata_size > MAX_EXTRADATA_SIZE) {
        av_log(s, AV_LOG_ERROR, "Too much extradata!\n");
        return NULL;
    }
    config = av_malloc(10 + par->extradata_size * 2);
    if (!config) {
        av_log(s, AV_LOG_ERROR, "Cannot allocate memory for the config info.\n");
        return NULL;
    }
    memcpy(config, "; config=", 9);
    ff_data_to_hex(config + 9, par->extradata, par->extradata_size, 0);
    config[9 + par->extradata_size * 2] = 0;

    return config;
}

static char *xiph_extradata2config(AVFormatContext *s, AVCodecParameters *par)
{
    char *config, *encoded_config;
    const uint8_t *header_start[3];
    int headers_len, header_len[3], config_len;
    int first_header_size;

    switch (par->codec_id) {
    case AV_CODEC_ID_THEORA:
        first_header_size = 42;
        break;
    case AV_CODEC_ID_VORBIS:
        first_header_size = 30;
        break;
    default:
        av_log(s, AV_LOG_ERROR, "Unsupported Xiph codec ID\n");
        return NULL;
    }

    if (avpriv_split_xiph_headers(par->extradata, par->extradata_size,
                              first_header_size, header_start,
                              header_len) < 0) {
        av_log(s, AV_LOG_ERROR, "Extradata corrupt.\n");
        return NULL;
    }

    headers_len = header_len[0] + header_len[2];
    config_len = 4 +          // count
                 3 +          // ident
                 2 +          // packet size
                 1 +          // header count
                 2 +          // header size
                 headers_len; // and the rest

    config = av_malloc(config_len);
    if (!config)
        goto xiph_fail;

    encoded_config = av_malloc(AV_BASE64_SIZE(config_len));
    if (!encoded_config) {
        av_free(config);
        goto xiph_fail;
    }

    config[0] = config[1] = config[2] = 0;
    config[3] = 1;
    config[4] = (RTP_XIPH_IDENT >> 16) & 0xff;
    config[5] = (RTP_XIPH_IDENT >>  8) & 0xff;
    config[6] = (RTP_XIPH_IDENT      ) & 0xff;
    config[7] = (headers_len >> 8) & 0xff;
    config[8] = headers_len & 0xff;
    config[9] = 2;
    config[10] = header_len[0];
    config[11] = 0; // size of comment header; nonexistent
    memcpy(config + 12, header_start[0], header_len[0]);
    memcpy(config + 12 + header_len[0], header_start[2], header_len[2]);

    av_base64_encode(encoded_config, AV_BASE64_SIZE(config_len),
                     config, config_len);
    av_free(config);

    return encoded_config;

xiph_fail:
    av_log(s, AV_LOG_ERROR, "Not enough memory for configuration string\n");
    return NULL;
}

static int latm_context2profilelevel(AVCodecParameters *par)
{
    /* MP4A-LATM
     * The RTP payload format specification is described in RFC 3016
     * The encoding specifications are provided in ISO/IEC 14496-3 */

    int profile_level = 0x2B;

    /* TODO: AAC Profile only supports AAC LC Object Type.
     * Different Object Types should implement different Profile Levels */

    if (par->sample_rate <= 24000) {
        if (par->channels <= 2)
            profile_level = 0x28; // AAC Profile, Level 1
    } else if (par->sample_rate <= 48000) {
        if (par->channels <= 2) {
            profile_level = 0x29; // AAC Profile, Level 2
        } else if (par->channels <= 5) {
            profile_level = 0x2A; // AAC Profile, Level 4
        }
    } else if (par->sample_rate <= 96000) {
        if (par->channels <= 5) {
            profile_level = 0x2B; // AAC Profile, Level 5
        }
    }

    return profile_level;
}

static char *latm_context2config(AVFormatContext *s, AVCodecParameters *par)
{
    /* MP4A-LATM
     * The RTP payload format specification is described in RFC 3016
     * The encoding specifications are provided in ISO/IEC 14496-3 */

    uint8_t config_byte[6];
    int rate_index;
    char *config;

    for (rate_index = 0; rate_index < 16; rate_index++)
        if (avpriv_mpeg4audio_sample_rates[rate_index] == par->sample_rate)
            break;
    if (rate_index == 16) {
        av_log(s, AV_LOG_ERROR, "Unsupported sample rate\n");
        return NULL;
    }

    config_byte[0] = 0x40;
    config_byte[1] = 0;
    config_byte[2] = 0x20 | rate_index;
    config_byte[3] = par->channels << 4;
    config_byte[4] = 0x3f;
    config_byte[5] = 0xc0;

    config = av_malloc(6*2+1);
    if (!config) {
        av_log(s, AV_LOG_ERROR, "Cannot allocate memory for the config info.\n");
        return NULL;
    }
    ff_data_to_hex(config, config_byte, 6, 1);
    config[12] = 0;

    return config;
}

static void fill_rtp_format_info(char *buff, int size, int payload_type, AVStream *st, AVFormatContext *fmt)
{
    char *config = NULL;
    AVCodecParameters *p = st->codecpar;

    switch (p->codec_id) {
        case AV_CODEC_ID_H264: {
            int mode = 1;
            if (fmt && fmt->oformat && fmt->oformat->priv_class &&
                av_opt_flag_is_set(fmt->priv_data, "rtpflags", "h264_mode0"))
                mode = 0;
            if (p->extradata_size) {
                config = extradata2psets(fmt, p);
            }
            snprintf(buff, size, "packetization-mode=%d%s",
                                     mode, config ? config : "");

            break;
        }
        case AV_CODEC_ID_H261:
        {
            const char *pic_fmt = NULL;
  
            if (p->width == 176 && p->height == 144)
                pic_fmt = "QCIF=1";
            else if (p->width == 352 && p->height == 288)
                pic_fmt = "CIF=1";

            if (pic_fmt)
                strncpy(buff, pic_fmt, size);
            else
                *buff = '\0';
  
            break;
        }
        case AV_CODEC_ID_HEVC:
            if (p->extradata_size)
                config = extradata2psets_hevc(p);

            if (config)
                strncpy(buff, config, size);
            else
                *buff = '\0';

            break;
        case AV_CODEC_ID_MPEG4:
            if (p->extradata_size) {
                config = extradata2config(fmt, p);
            }
            snprintf(buff, size, "profile-level-id=1%s",
                                     config ? config : "");
            break;
        case AV_CODEC_ID_AAC:
            if (fmt && fmt->oformat && fmt->oformat->priv_class &&
                av_opt_flag_is_set(fmt->priv_data, "rtpflags", "latm")) {
                config = latm_context2config(fmt, p);

                if (config)
                   snprintf(buff, size, "profile-level-id=%d;cpresent=0;config=%s",
                                         latm_context2profilelevel(p), config);
                else
                  *buff = '\0';

            } else {
                if (p->extradata_size) {
                    config = extradata2config(fmt, p);
                } else {

                    av_log(fmt, AV_LOG_ERROR, "AAC with no global headers is currently not supported.\n");
                    *buff = '\0';
                    return;
                }

                if (config) 
                 snprintf(buff, size, "profile-level-id=1;"
                                        "mode=AAC-hbr;sizelength=13;indexlength=3;"
                                        "indexdeltalength=3%s",config);
                else
                 *buff = '\0';
            }
            break;

        case AV_CODEC_ID_AMR_NB:
        case AV_CODEC_ID_AMR_WB:
            strncpy(buff, "octet-align=1", size);
            break;
        case AV_CODEC_ID_VORBIS:
            if (p->extradata_size)
                config = xiph_extradata2config(fmt, p);
            else
                av_log(fmt, AV_LOG_ERROR, "Vorbis configuration info missing\n");

            if (config)
               snprintf(buff, size, "configuration=%s", config);
            else
               *buff = '\0';

            break;
        case AV_CODEC_ID_THEORA: {
            const char *pix_fmt;
            switch (p->format) {
            case AV_PIX_FMT_YUV420P:
                pix_fmt = "YCbCr-4:2:0";
                break;
            case AV_PIX_FMT_YUV422P:
                pix_fmt = "YCbCr-4:2:2";
                break;
            case AV_PIX_FMT_YUV444P:
                pix_fmt = "YCbCr-4:4:4";
                break;
            default:
                av_log(fmt, AV_LOG_ERROR, "Unsupported pixel format.\n");
                *buff = '\0';
                return;
            }

            if (p->extradata_size)
                config = xiph_extradata2config(fmt, p);
            else
                av_log(fmt, AV_LOG_ERROR, "Theora configuration info missing\n");

            if (config)
               snprintf(buff, size, "delivery-method=inline; "
                                    "width=%d; height=%d; sampling=%s; "
                                    "configuration=%s",
                                    p->width, p->height, pix_fmt, config);
            else
               *buff = '\0';

            break;
        }

        case AV_CODEC_ID_ILBC:
            snprintf(buff, size, "mode=%d",
                                     p->block_align == 38 ? 20 : 30);
            break;
        case AV_CODEC_ID_SPEEX:
            if (st->codec) {
                const char *mode;
                uint64_t vad_option;

                if (st->codec->flags & AV_CODEC_FLAG_QSCALE)
                      mode = "on";
                else if (!av_opt_get_int(st->codec, "vad", AV_OPT_FLAG_ENCODING_PARAM, &vad_option) && vad_option)
                      mode = "vad";
                else
                      mode = "off";

                snprintf(buff, size, "vbr=%s", mode);
            }
            else
               *buff = '\0';
            break;
        case AV_CODEC_ID_OPUS:
            if (p->channels == 2) 
                strncpy(buff, "sprop-stereo=1", size);
            else
               *buff = '\0';
            break;
        default:
            *buff = '\0';
            break;
    }

    av_free(config);
}

static int create_janus_mountpoint(AVFormatContext *s, const char *admin_key, const char *mountpoint_id, const char *mountpoint_pin, int mountpoint_is_private, int destroy_previous_mountpoint, int *video_port, int *audio_port)
{     
   const char *create_session_request = "{\"janus\":\"create\",\"transaction\":\"sBJNyUhH6Vc6\"}";
   const char *destroy_session_request = "{\"janus\":\"destroy\",\"transaction\":\"1YUgyfhH0Vp3\"}";
   const char *attach_plugin_request = "{\"janus\":\"attach\",\"transaction\":\"xWJquAhH6dc2\",\"plugin\":\"janus.plugin.streaming\"}";
   const char *detach_plugin_request = "{\"janus\":\"detach\",\"transaction\":\"vZdFuGtJy213\"}";
   const char *create_mountpoint_request_template = "{\"janus\":\"message\",\"transaction\":\"hRJNyehH2jc4\",\"body\":{\"request\":\"create\",\"secret\":\"3DEYE_KEY_!\",%s%s\"type\":\"rtp\",\"is_private\":%s,\"id\":%s,\"name\":\"%s\",\"video\":true,\"videortpmap\":\"%s\",\"videopt\":%d,\"videofmtp\":\"%s\",\"videoport\":0,\"audio\":%s,\"audiortpmap\":\"%s\",\"audiopt\":%d,\"audiofmtp\":\"%s\",\"audioport\":0}}";
   const char *info_mountpoint_template = "{\"janus\":\"message\",\"transaction\":\"tRJRyeaV7fc0\",\"body\":{\"request\":\"info\",\"id\":%s,\"secret\":\"3DEYE_KEY_!\"}}";
   const char *destroy_mountpoint_request_template = "{\"janus\":\"message\",\"transaction\":\"cxJRyhtB1lo0\",\"body\":{\"request\":\"destroy\",\"id\":%s,\"secret\":\"3DEYE_KEY_!\"}}";
   const char *trueString = "true";
   const char *falseString = "false";
   char janus_response[2048];
   char session_buffer[512];
   char plugin_id_buffer[512];
   char path_buffer[512];
   char video_rtp_map_buffer[512];
   char audio_rtp_map_buffer[512];
   char video_rtp_format_buffer[512];
   char audio_rtp_format_buffer[512];
   char admin_key_buffer[512];
   char pin_buffer[512];
   char buffer[4096];
   char *session_id;
   char *plugin_id;
   char *error_code;
   char *port_value;
   int video_payload_type;
   int audio_payload_type;
   int ret;
   JanusState *js = s->priv_data;

   if((ret = send_http_json_request(s, "/janus", create_session_request, janus_response, sizeof(janus_response))) < 0)
     goto fail;

   session_id = read_json_value(janus_response, "id", session_buffer, sizeof(session_buffer));

   if(session_id == NULL)
   {
     av_log(s, AV_LOG_ERROR, "Session id is not found in json.\n");
     ret = -10000;
     goto fail;
   }

   snprintf(path_buffer, sizeof(path_buffer), "/janus/%s", session_id);

   if((ret = send_http_json_request(s, path_buffer, attach_plugin_request, janus_response, sizeof(janus_response))) < 0)
     goto fail;

   plugin_id = read_json_value(janus_response, "id", plugin_id_buffer, sizeof(plugin_id_buffer));

   if(plugin_id == NULL)
   {
     av_log(s, AV_LOG_ERROR, "Plugin id is not found in json.\n");
     ret = -10001;
     goto destroy_session;
   }

   snprintf(path_buffer, sizeof(path_buffer), "/janus/%s/%s", session_id, plugin_id);

   if(destroy_previous_mountpoint)
   {
      snprintf(buffer, sizeof(buffer), destroy_mountpoint_request_template, mountpoint_id);

      if((ret = send_http_json_request(s, path_buffer, buffer, janus_response, sizeof(janus_response))) < 0)
         goto fail;
   }

   video_payload_type = ff_rtp_get_payload_type(s, s->streams[js->video_stream_index]->codecpar, js->video_stream_index);
   fill_rtp_map_info(video_rtp_map_buffer, sizeof(video_rtp_map_buffer), video_payload_type, s->streams[js->video_stream_index], s);
   fill_rtp_format_info(video_rtp_format_buffer, sizeof(video_rtp_format_buffer), video_payload_type, s->streams[js->video_stream_index], s);

   av_log(s, AV_LOG_DEBUG, "Video rtp map info: %s\n", video_rtp_map_buffer);
   av_log(s, AV_LOG_DEBUG, "Video rtp format info: %s\n", video_rtp_format_buffer);

   if(js->audio_stream_index != -1)
   {
      audio_payload_type = ff_rtp_get_payload_type(s, s->streams[js->audio_stream_index]->codecpar, js->audio_stream_index);
      fill_rtp_map_info(audio_rtp_map_buffer, sizeof(audio_rtp_map_buffer), audio_payload_type, s->streams[js->audio_stream_index], s);
      fill_rtp_format_info(audio_rtp_format_buffer, sizeof(audio_rtp_format_buffer), audio_payload_type, s->streams[js->audio_stream_index], s);

      av_log(s, AV_LOG_DEBUG, "Audio rtp map info: %s\n", audio_rtp_map_buffer);
      av_log(s, AV_LOG_DEBUG, "Audio rtp format info: %s\n", audio_rtp_format_buffer);
   }
   else
   {
      *audio_rtp_map_buffer='\0';
      *audio_rtp_format_buffer = '\0';
      audio_payload_type = 0;
   }

   if(admin_key != NULL)
     snprintf(admin_key_buffer, sizeof(admin_key_buffer), "\"admin_key\":\"%s\",", admin_key);
   else
     *admin_key_buffer = '\0';

   if(mountpoint_pin != NULL)
      snprintf(pin_buffer, sizeof(pin_buffer), "\"pin\":\"%s\",", mountpoint_pin);
   else
      *pin_buffer = '\0';

   snprintf(buffer, sizeof(buffer), create_mountpoint_request_template, admin_key_buffer, pin_buffer, mountpoint_is_private == 1 ? trueString : falseString, mountpoint_id, mountpoint_id, 
     video_rtp_map_buffer, video_payload_type, video_rtp_format_buffer, 
    js->audio_stream_index != -1 ? trueString : falseString, audio_rtp_map_buffer, 
    audio_payload_type, audio_rtp_format_buffer);

   if((ret = send_http_json_request(s, path_buffer, buffer, janus_response, sizeof(janus_response))) < 0)
     goto fail;

  error_code = read_json_value(janus_response, "error_code", buffer, sizeof(buffer));

  if(error_code != NULL)
  {
    if(strcmpi(error_code, "456") != 0)
    {
       av_log(s, AV_LOG_ERROR, "Unknown error code in json response: %s\n", error_code);
       ret = -10002;
       goto detach_plugin;
    }

    snprintf(buffer, sizeof(buffer), info_mountpoint_template, mountpoint_id);

    if((ret = send_http_json_request(s, path_buffer, buffer, janus_response, sizeof(janus_response))) < 0)
      goto fail;

    port_value = read_json_value(janus_response, "videoport", buffer, sizeof(buffer));

    if(port_value == NULL)
    {
      av_log(s, AV_LOG_ERROR, "Video port is not found in response\n");
      ret = -10003;
      goto detach_plugin;
    }

    if(!(*video_port = strtoull(port_value, NULL, 10)))
    {
      av_log(s, AV_LOG_ERROR, "Can't parse video port: %s\n", port_value);
      ret = -10004;
      goto detach_plugin;
    }

    if(js->audio_stream_index != -1)
    {
       port_value = read_json_value(janus_response, "audioport", buffer, sizeof(buffer));

       if(port_value == NULL)
       {
         av_log(s, AV_LOG_ERROR, "Audio port is not found in response\n");
         ret = -10005;
         goto detach_plugin;
       }

       if(!(*audio_port = strtoull(port_value, NULL, 10)))
       {
          av_log(s, AV_LOG_ERROR, "Can't parse audio port: %s\n", port_value);
          ret = -10006;
          goto detach_plugin;
       }
    }
  }
  else
  {
    port_value = read_json_value(janus_response, "video_port", buffer, sizeof(buffer));

    if(port_value == NULL)
    {
      av_log(s, AV_LOG_ERROR, "Video port is not found in response\n");
      ret = -10007;
      goto detach_plugin;
    }

    if(!(*video_port = strtoull(port_value, NULL, 10)))
    {
      av_log(s, AV_LOG_ERROR, "Can't parse video port: %s\n", port_value);
      ret = -10008;
      goto detach_plugin;
    }

    if(js->audio_stream_index != -1)
    {
       port_value = read_json_value(janus_response, "audio_port", buffer, sizeof(buffer));

       if(port_value == NULL)
       {
         av_log(s, AV_LOG_ERROR, "Audio port is not found in response\n");
         ret = -10009;
         goto detach_plugin;
       }

       if(!(*audio_port = strtoull(port_value, NULL, 10)))
       {
          av_log(s, AV_LOG_ERROR, "Can't parse audio port: %s\n", port_value);
          ret = -10010;
          goto detach_plugin;
       }
    }
  }

detach_plugin:
   snprintf(path_buffer, sizeof(path_buffer), "/janus/%s/%s", session_id, plugin_id);

   if((ret = send_http_json_request(s, path_buffer, detach_plugin_request, janus_response, sizeof(janus_response))) < 0)
     goto fail;

destroy_session:
   snprintf(path_buffer, sizeof(path_buffer), "/janus/%s", session_id);

   if((ret = send_http_json_request(s, path_buffer, destroy_session_request, janus_response, sizeof(janus_response))) < 0)
     goto fail;

fail:
  return ret;
}

static int janus_set_rtp_remote_url(AVFormatContext *s, URLContext **rtp_handle, int port)
{
    char hostname[512];
    int ret;
    AVDictionary *opts = NULL;
    char buf[2048];

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), NULL, NULL, 0, s->filename);

    ff_url_join(buf, sizeof(buf), "rtp", NULL, hostname, port, "%s", "?localrtcpport=0");

    ret = ffurl_open_whitelist(rtp_handle, buf, AVIO_FLAG_WRITE,
                                 &s->interrupt_callback, &opts, s->protocol_whitelist, s->protocol_blacklist, NULL);

    return ret;
}

static void* ensure_janus_mountpoint_exists_thread(void *arg)
{
   int i, sleep_count, video_port, audio_port, reconnect;
   int destroy_previous_mountpoint;
   AVFormatContext *s = (AVFormatContext *)arg;
   JanusState *js = s->priv_data;

   destroy_previous_mountpoint = 1;
   while(!js->janus_thread_terminated)
   {
      if(!create_janus_mountpoint(s, js->admin_key, js->mountpoint_id, js->mountpoint_pin, js->mountpoint_is_private, destroy_previous_mountpoint, &video_port, &audio_port))
      {
         destroy_previous_mountpoint = 0;
         sleep_count = 5 * 60;
         reconnect = 0;

         if(js->video_port != video_port)
         {
            av_log(s, AV_LOG_DEBUG, "New mountpoint video port: %d\n", video_port);
            js->video_port = video_port;
            reconnect = 1;
         }

         if(js->audio_stream_index != -1 && js->audio_port != audio_port)
         {
            av_log(s, AV_LOG_DEBUG, "New mountpoint audio port: %d\n", audio_port);
            js->audio_port = audio_port;
            reconnect = 1;
         }
 
          if(reconnect)
           js->reconnect = reconnect;
       }
       else
           sleep_count = 10;

       for (i = 0; i < sleep_count; i++) 
       {
          if (js->janus_thread_terminated)
            return NULL;

          sleep(1);
       }
   }

   return NULL;
}

static int janus_write_header(AVFormatContext *s)
{
   JanusState *js = s->priv_data;
   AVStream *st = NULL;
   AVCodecParameters *par;
   int i;
   int ret;

   if(js->mountpoint_id == NULL)
   {
     av_log(s, AV_LOG_ERROR, "Parameter \"mountpoint_id\" is not set\n");
     return AVERROR_OPTION_NOT_FOUND;
   }
   
   js->video_rtpctx = NULL;
   js->audio_rtpctx = NULL;
   js->video_stream_index = -1;
   js->audio_stream_index = -1;
   js->reconnect = 0;
   js->video_port = -1;
   js->audio_port = -1;
   js->thread_terminate_cb.callback = interrupt_cb;
   js->thread_terminate_cb.opaque = js;
   js->wait_i_frame = 0;
   js->extradata = NULL;
   js->extradata_copy = NULL;

   for(i = 0; i < s->nb_streams; i++)
   {
      st = s->streams[i];

      if(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && js->video_stream_index == -1)
           js->video_stream_index = i;
      else if(st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && js->audio_stream_index == -1)
           js->audio_stream_index = i;
       else
           continue;
   }

   if(js->video_stream_index == -1)
   {
      av_log(s, AV_LOG_ERROR, "Video stream is not found\n");
      return AVERROR_INVALIDDATA;
   }

   par = s->streams[js->video_stream_index]->codecpar;

   if(par->codec_id == AV_CODEC_ID_H264)
   {
     js->extradata_size = par->extradata_size;

     if (par->extradata[0] == 1) {

       if ((ret = ff_avc_write_annexb_extradata(par->extradata, &js->extradata,
                                          &js->extradata_size)))
          return ret;

        js->extradata_copy = js->extradata;
     }
     else 
        js->extradata = par->extradata;
  
   }

   js->janus_thread_terminated = 0;

   if(pthread_create(&js->janus_thread, NULL, &ensure_janus_mountpoint_exists_thread, s) != 0)
      return AVERROR(ENOMEM);

   return 0;
}

static void close_rtp_context(AVFormatContext *rtpctx)
{
   av_write_trailer(rtpctx);
   avio_closep(&rtpctx->pb);
   avformat_free_context(rtpctx);
}

static int janus_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    JanusState *js = s->priv_data;
    AVFormatContext *rtpctx;
    URLContext *urlctx;
    int ret;

    if (pkt->stream_index < 0)
        return AVERROR_INVALIDDATA;

    if(js->reconnect)
    {
       if(js->video_rtpctx != NULL)
       {
         close_rtp_context(js->video_rtpctx);
         js->video_rtpctx = NULL;
       }

       if(js->audio_rtpctx != NULL)
       {
          close_rtp_context(js->audio_rtpctx);
          js->audio_rtpctx = NULL;
       }

       urlctx = NULL;

       if((ret = janus_set_rtp_remote_url(s, &urlctx, js->video_port)) < 0)
          return ret;

       if ((ret = ff_rtp_chain_mux_open(&rtpctx,
                                       s, s->streams[js->video_stream_index], urlctx,
                                       RTSP_TCP_MAX_PACKET_SIZE,
                                       js->video_stream_index)) < 0)
         return ret;

       js->video_rtpctx = rtpctx;

       if(js->audio_stream_index != -1)
       {
          urlctx = NULL;

          if((ret = janus_set_rtp_remote_url(s, &urlctx, js->audio_port)) < 0)
            return ret;

          if ((ret = ff_rtp_chain_mux_open(&rtpctx,
                                       s, s->streams[js->audio_stream_index], urlctx,
                                       RTSP_TCP_MAX_PACKET_SIZE,
                                       js->audio_stream_index)) < 0)
            return ret;

           js->audio_rtpctx = rtpctx;
        }

        js->reconnect = 0;
        js->wait_i_frame = 1;
    }

    if(pkt->stream_index == js->video_stream_index)
    {
       if(js->video_rtpctx == NULL)
          return 0;

       rtpctx = js->video_rtpctx;

       if((pkt->flags & AV_PKT_FLAG_KEY) != 0)
       {
         js->wait_i_frame = 0;

         if(js->extradata_size > 0 && pkt->size > 4 && (pkt->data[4] & 0x1F) != 7)
         {
           if(av_grow_packet(pkt, js->extradata_size))
             return ret;

           memmove(pkt->data + js->extradata_size, pkt->data, pkt->size - js->extradata_size);
           memcpy(pkt->data, js->extradata, js->extradata_size);
         }
       }
       
       if(js->wait_i_frame)
           return 0;
    }
    else if(pkt->stream_index == js->audio_stream_index && !js->wait_i_frame)
    {
       if(js->audio_rtpctx == NULL)
          return 0;

       rtpctx = js->audio_rtpctx;
    }
    else
       return 0;

    ret = ff_write_chained(rtpctx, 0, pkt, s, 0);    
    return ret;
}

static int janus_write_close(AVFormatContext *s)
{
    JanusState *js = s->priv_data;

    js->janus_thread_terminated = 1;
    pthread_join(js->janus_thread, NULL);
    av_free(js->extradata_copy);

    if(js->video_rtpctx != NULL)
      close_rtp_context(js->video_rtpctx);

    if(js->audio_rtpctx != NULL)
      close_rtp_context(js->audio_rtpctx);

    return 0;
}

AVOutputFormat ff_janus_muxer = {
    .name              = "janus",
    .long_name         = NULL_IF_CONFIG_SMALL("Janus output"),
    .priv_data_size    = sizeof(JanusState),
    .audio_codec       = AV_CODEC_ID_PCM_MULAW,
    .video_codec       = AV_CODEC_ID_MPEG4,
    .write_header      = janus_write_header,
    .write_packet      = janus_write_packet,
    .write_trailer     = janus_write_close,
    .flags             = AVFMT_NOFILE | AVFMT_TS_NONSTRICT,
    .priv_class        = &janus_muxer_class,
};
