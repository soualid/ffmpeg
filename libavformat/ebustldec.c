// https://tech.ebu.ch/docs/tech/tech3264.pdf

#include <stdbool.h>
#include <libavformat/internal.h>
#include "libavcodec/ass.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/intreadwrite.h"

#define EBU_HEADER_SIZE 1024
#define TTI_BLOCK_SIZE 128

static int ebustl_probe(const AVProbeData *p)
{
    if (p->buf_size < 5)
        return 0;
    if (p->buf[3] == 'S' && p->buf[4] == 'T' && p->buf[5] == 'L')
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int ebustl_read_header(AVFormatContext *s)
{
    AVStream *st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = AV_CODEC_ID_EBUSTL;
    st->codecpar->width = 720;
    st->codecpar->height = 576;
    st->time_base = (AVRational){1, 1000};
    avpriv_set_pts_info(st, 64, 1, 1000);
    avio_seek(s->pb, EBU_HEADER_SIZE, SEEK_SET);
    return 0;
}

static int64_t ebustl_timestamp_to_pts(const uint8_t *timecode)
{
    int hours = timecode[0];
    int minutes = timecode[1];
    int seconds = timecode[2];
    int frames = timecode[3];
    return ((hours - 10) * 3600LL + minutes * 60LL + seconds) * 1000LL + frames * (1000 / 25);
}

static int ebustl_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int64_t pts;
    int64_t pts_end;
    int ret = av_new_packet(pkt, TTI_BLOCK_SIZE);
    if (ret < 0)
        return ret;
    ret = avio_read(s->pb, pkt->data, TTI_BLOCK_SIZE);
    if (ret < TTI_BLOCK_SIZE) {
        av_packet_unref(pkt);
        return AVERROR_EOF;
    }
    pts = ebustl_timestamp_to_pts(pkt->data + 5);
    pkt->pts = pts;
    pts_end = ebustl_timestamp_to_pts(pkt->data + 9);
    pkt->duration = pts_end - pts;
    pkt->stream_index = 0;
    return 0;
}

static int ebustl_read_close(AVFormatContext *s)
{
    return 0;
}

AVInputFormat ff_ebustl_demuxer = {
    .name           = "ebustl",
    .long_name      = "EBU STL Subtitle format",
    .extensions     = "stl",
    .read_probe     = ebustl_probe,
    .read_header    = ebustl_read_header,
    .read_packet    = ebustl_read_packet,
    .read_close     = ebustl_read_close,
};