#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "avio_internal.h"
#include "internal.h"
#include "libswscale/swscale.h"
#include <unistd.h>

#if defined(__linux__)
    #include <fcntl.h>
    #include <sys/mman.h>
#endif

#define ALIGN (HAVE_AVX ? 32 : 16)

#pragma pack(push,1)
typedef struct CommandBufferData {
    int ready_flag;
    uint64_t timestamp;
    int width;
    int height;
    int bgr_stride;
    int gray_stride;
} CommandBufferData;
#pragma pack(pop)

#define COMMAND_BUFFER_LENGTH sizeof(CommandBufferData)

typedef struct Stream2ShmData {
    const AVClass *class;
    int cmd_file_handle;
    int image_file_handle;
    int gray_image_file_handle;
    char *cmd_buffer_ptr;
    char *image_buffer_ptr;
    int image_buffer_length;
    char *gray_image_buffer_ptr;
    int gray_image_buffer_length;
    int current_width;
    int current_height;
    enum AVPixelFormat current_format;
    struct SwsContext *sws_ctx;
} Stream2ShmData;

static int write_header(AVFormatContext *s)
{
 Stream2ShmData *h = (Stream2ShmData *)s->priv_data;

#if defined(__linux__)

 h->image_buffer_ptr = MAP_FAILED;
 h->gray_image_buffer_ptr = MAP_FAILED;
 h->image_file_handle = -1;
 h->gray_image_file_handle = -1;

 h->cmd_file_handle = shm_open(s->url, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

 if(h->cmd_file_handle == -1) {
   av_log(s, AV_LOG_ERROR, "Command file \"%s\" open failed\n", s->url);
   return -1;
 }

 h->cmd_buffer_ptr = mmap(NULL, COMMAND_BUFFER_LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, h->cmd_file_handle, 0);

 if(h->cmd_buffer_ptr == MAP_FAILED) {
   av_log(s, AV_LOG_ERROR, "Map Command file \"%s\" failed\n", s->url);
   close(h->cmd_file_handle);
   return -1;
 }

#endif

 h->current_width = 0;
 h->current_height = 0;
 h->current_format = AV_PIX_FMT_NONE;

 return 0;
}

static int write_packet(AVFormatContext *s, AVPacket *pkt)
{   
 Stream2ShmData *h = (Stream2ShmData *)s->priv_data;
 AVStream *st = s->streams[pkt->stream_index];
 int width;
 int height;
 int stride;
 AVFrame *frame;
 AVRational *time_base;
 CommandBufferData *cbd = (CommandBufferData *)h->cmd_buffer_ptr;
 char filename[512];

 if(st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
  return 0;

 switch (st->codecpar->format) {
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_YUV411P:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
        break;
    default:
        av_log(s, AV_LOG_ERROR, "The pixel format '%s' is not supported.\n",
               av_get_pix_fmt_name(st->codecpar->format));
        return AVERROR(EINVAL);
 }

 while(cbd->ready_flag) {
  
  if (ff_check_interrupt(&s->interrupt_callback))
   return AVERROR_EXIT;
            
  usleep(16 * 1000);
 }

 frame = (AVFrame *)pkt->data;
  
 width  = frame->width;
 height = frame->height;
 stride = frame->width * 3;

 if(h->current_width != width || h->current_height != height || h->current_format != st->codecpar->format) {
#if defined(__linux__)
  if(h->image_buffer_ptr != MAP_FAILED)
   munmap(h->image_buffer_ptr, h->image_buffer_length);

  if(h->gray_image_buffer_ptr != MAP_FAILED)
   munmap(h->gray_image_buffer_ptr, h->gray_image_buffer_length);

  snprintf(filename, sizeof(filename), "%s_img", s->url);

  if(h->image_file_handle == -1 ) {
     h->image_file_handle = shm_open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

   if(h->image_file_handle == -1) {
     av_log(s, AV_LOG_ERROR, "Shared image file \"%s\" create failed\n", filename);
     return -1;
   }
  }

  h->image_buffer_length = stride * height;
  
  int capacity = h->image_buffer_length + ALIGN;

  if(ftruncate(h->image_file_handle, capacity) != 0) {
    av_log(s, AV_LOG_ERROR, "Shared image file \"%s\" truncate failed\n", filename);
    close(h->image_file_handle);
    h->image_file_handle = -1;
    return -1;
  }

  h->image_buffer_ptr = mmap(NULL, capacity, PROT_WRITE, MAP_SHARED, h->image_file_handle, 0);

  if(h->image_buffer_ptr == MAP_FAILED) {
    av_log(s, AV_LOG_ERROR, "Map image file \"%s\" failed\n", filename);
    close(h->image_file_handle);
    h->image_file_handle = -1;
    return -1;
  }

  snprintf(filename, sizeof(filename), "%s_gray_img", s->url);

  if(h->gray_image_file_handle == -1 ) {
    h->gray_image_file_handle = shm_open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

   if(h->gray_image_file_handle == -1) {
     av_log(s, AV_LOG_ERROR, "Shared gray image file \"%s\" create failed\n", filename);
     return -1;
   }
  }

  h->gray_image_buffer_length = frame->linesize[0] * height;

  if(ftruncate(h->gray_image_file_handle, h->gray_image_buffer_length) != 0) {
    av_log(s, AV_LOG_ERROR, "Shared gray image file \"%s\" truncate failed\n", filename);
    close(h->gray_image_file_handle);
    h->gray_image_file_handle = -1;
    return -1;
  }

  h->gray_image_buffer_ptr = mmap(NULL, h->gray_image_buffer_length, PROT_WRITE, MAP_SHARED, h->gray_image_file_handle, 0);

  if(h->gray_image_buffer_ptr == MAP_FAILED) {
    av_log(s, AV_LOG_ERROR, "Map gray image file \"%s\" failed\n", filename);
    close(h->gray_image_file_handle);
    h->gray_image_file_handle = -1;
    return -1;
  }

#endif

  if(h->sws_ctx != NULL)
   sws_freeContext(h->sws_ctx);

  h->sws_ctx = sws_getContext(width, height, st->codecpar->format, width, height, AV_PIX_FMT_BGR24,
             SWS_BILINEAR, NULL, NULL, NULL);

  if(h->sws_ctx == NULL) {
    av_log(s, AV_LOG_ERROR, "Could not initialize the conversion context\n");
    return -1;
  }

  h->current_width = width;
  h->current_height = height;
  h->current_format = st->codecpar->format;
 }

 memcpy(h->gray_image_buffer_ptr, frame->data[0], h->gray_image_buffer_length);

 if(sws_scale(h->sws_ctx, (const uint8_t * const*)frame->data, frame->linesize, 0, height, (uint8_t **)&h->image_buffer_ptr, &stride) != height)
  return -1;

 time_base = &s->streams[pkt->stream_index]->time_base;

 cbd->timestamp = av_rescale_q(pkt->pts, time_base, (AVRational) { 1, 1000 });
 cbd->width = width;
 cbd->height = height;
 cbd->bgr_stride = stride;
 cbd->gray_stride = frame->linesize[0];
 cbd->ready_flag = 1;

 return 0;
}

static int write_trailer(struct AVFormatContext *s)
{
 Stream2ShmData *h = (Stream2ShmData *)s->priv_data;

#if defined(__linux__)

 if(h->image_buffer_ptr != MAP_FAILED)
   munmap(h->image_buffer_ptr, h->image_buffer_length + ALIGN);

 if(h->image_file_handle != -1 )
  close(h->image_file_handle);

 if(h->gray_image_buffer_ptr != MAP_FAILED)
   munmap(h->gray_image_buffer_ptr, h->gray_image_buffer_length);

 if(h->gray_image_file_handle != -1 )
  close(h->gray_image_file_handle);
 
 if(h->cmd_buffer_ptr != MAP_FAILED)
  munmap(h->cmd_buffer_ptr, COMMAND_BUFFER_LENGTH);

 if(h->cmd_file_handle != -1 )
  close(h->cmd_file_handle);
 
#endif

 if(h->sws_ctx != NULL)
  sws_freeContext(h->sws_ctx);
  
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
    .priv_data_size = sizeof(Stream2ShmData),
    .video_codec    = AV_CODEC_ID_WRAPPED_AVFRAME,
    .write_header   = write_header,
    .write_packet   = write_packet,
    .write_trailer  = write_trailer,
    .flags          = AVFMT_TS_NONSTRICT | AVFMT_NOFILE,
    .priv_class     = &stream2shm_muxer_class
};
#endif