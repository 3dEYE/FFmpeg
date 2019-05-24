#include "libavutil/pixdesc.h"
#include "avio_internal.h"
#include "internal.h"
#include <libswscale/swscale.h>

#if defined(__linux__)
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
#endif

typedef struct CommandBufferData {
    char ready_flag;
    uint64_t timestamp;
    int width;
    int height;
    int stride;
} CommandBufferData;


#define COMMAND_BUFFER_LENGTH sizeof(CommandBufferData)

typedef struct Stream2ShmData {
    const AVClass *class;
    int cmd_file_handle;
    int image_file_handle;
    char *cmd_buffer_ptr;
    char *image_buffer_ptr;
    int image_buffer_length;
    int current_width;
    int current_height;
    struct SwsContext *sws_ctx;
} Stream2ShmData;

static int write_header(AVFormatContext *s)
{
 Stream2ShmData *h = (Stream2ShmData *)s->priv_data;

#if defined(__linux__)

 h->image_buffer_ptr = MAP_FAILED;
 h->image_file_handle = -1;
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

 while(cbd->ready_flag) {
  
  if (ff_check_interrupt(&s->interrupt_callback))
   return AVERROR_EXIT;
            
  usleep(8 * 1000);
 }

 width  = st->codecpar->width;
 height = st->codecpar->height;
 stride = st->codecpar->width * 3;

 frame = (AVFrame *)pkt->data;

 if(h->current_width != width || h->current_height != height) {
  snprintf(filename, 512, "%s_img", s->url);

#if defined(__linux__)

  if(h->image_buffer_ptr != MAP_FAILED)
   munmap(h->image_buffer_ptr, h->image_buffer_length);

  if(h->image_file_handle != -1 ) {
   close(h->image_file_handle);
   shm_unlink(filename);
  }

  h->image_file_handle = shm_open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

  if(h->image_file_handle == -1) {
    av_log(s, AV_LOG_ERROR, "Shared image file \"%s\" create failed\n", filename);
    return -1;
  }

  h->image_buffer_length = stride * height;

  if(ftruncate(h->image_file_handle, h->image_buffer_length) != 0) {
    av_log(s, AV_LOG_ERROR, "Shared image file \"%s\" truncate failed\n", filename);
    return -1;
  }

  h->image_buffer_ptr = mmap(NULL, h->image_buffer_length, PROT_WRITE, MAP_SHARED, h->image_file_handle, 0);

  if(h->image_buffer_ptr == MAP_FAILED) {
    av_log(s, AV_LOG_ERROR, "Map image file \"%s\" failed\n", filename);
    close(h->image_file_handle);
    shm_unlink(filename);
    return -1;
  }

#endif

  if(h->sws_ctx != NULL)
   sws_freeContext(h->sws_ctx);

  h->sws_ctx = sws_getContext(width, height, st->codecpar->format, width, height, AV_PIX_FMT_BGR24,
             SWS_FAST_BILINEAR, NULL, NULL, NULL);

  h->current_width = width;
  h->current_height = height;
 }

 if(sws_scale(h->sws_ctx, (const uint8_t * const*)frame->data, frame->linesize, 0, height, (uint8_t **)&h->image_buffer_ptr, &stride) != height)
  return -1;

 time_base = &s->streams[pkt->stream_index]->time_base;
 cbd->timestamp = *s->timestamp_base + pkt->pts * 1000 * time_base->num / time_base->den;
 cbd->width = width;
 cbd->height = height;
 cbd->stride = stride;
 cbd->ready_flag = 1;

 return 0;
}

static int write_trailer(struct AVFormatContext *s)
{
 char filename[512];
 Stream2ShmData *h = (Stream2ShmData *)s->priv_data;

#if defined(__linux__)

 if(h->image_buffer_ptr != MAP_FAILED)
   munmap(h->image_buffer_ptr, h->image_buffer_length);

 if(h->image_file_handle != -1 ) {
  close(h->image_file_handle);
  snprintf(filename, 512, "%s_img", s->url);
  shm_unlink(filename);
 }

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
