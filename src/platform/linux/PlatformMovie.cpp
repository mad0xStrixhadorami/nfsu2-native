#include "platform/linux/PlatformMovie.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {
constexpr int kAudioRate = 48000;

// Reads the movie from memory through a custom AVIOContext.
struct MemoryReader {
    std::vector<uint8_t> data;
    size_t pos = 0;

    static int Read(void *opaque, uint8_t *buf, int size)
    {
        auto *r = static_cast<MemoryReader *>(opaque);
        size_t n = std::min<size_t>(size, r->data.size() - r->pos);
        if (n == 0)
            return AVERROR_EOF;
        memcpy(buf, r->data.data() + r->pos, n);
        r->pos += n;
        return static_cast<int>(n);
    }

    static int64_t Seek(void *opaque, int64_t offset, int whence)
    {
        auto *r = static_cast<MemoryReader *>(opaque);
        if (whence == AVSEEK_SIZE)
            return static_cast<int64_t>(r->data.size());
        int64_t base = whence == SEEK_CUR ? int64_t(r->pos) : whence == SEEK_END ? int64_t(r->data.size()) : 0;
        int64_t p = base + offset;
        if (p < 0 || p > int64_t(r->data.size()))
            return -1;
        r->pos = static_cast<size_t>(p);
        return p;
    }
};

SDL_AudioDeviceID g_audio = 0;

SDL_AudioDeviceID AudioDevice()
{
    if (g_audio)
        return g_audio;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return 0;
    SDL_AudioSpec want = {}, have;
    want.freq = kAudioRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        SDL_PauseAudioDevice(g_audio, 0);
        printf("Movie audio: %s, %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
    } else {
        fprintf(stderr, "Movie audio unavailable: %s\n", SDL_GetError());
    }
    return g_audio;
}
} // namespace

struct MoviePlayer::Impl {
    MemoryReader reader;
    AVIOContext *io = nullptr;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    int videoStream = -1, audioStream = -1;
    SwrContext *swr = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *pending = nullptr; // decoded video frame waiting for its time
    double pendingTime = 0;
    Uint32 startTicks = 0;
    bool demuxDone = false;

    ~Impl()
    {
        av_frame_free(&pending);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (fmt)
            avformat_close_input(&fmt);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

MoviePlayer::MoviePlayer() = default;

MoviePlayer::~MoviePlayer()
{
    Stop();
}

static AVCodecContext *OpenDecoder(AVStream *st)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        return nullptr;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

bool MoviePlayer::Open(std::vector<uint8_t> data, SDL_Renderer *renderer)
{
    Stop();
    m = std::make_unique<Impl>();
    m->reader.data = std::move(data);
    const int bufSize = 64 << 10;
    auto *buf = static_cast<uint8_t *>(av_malloc(bufSize));
    m->io = avio_alloc_context(buf, bufSize, 0, &m->reader, &MemoryReader::Read, nullptr, &MemoryReader::Seek);
    m->fmt = avformat_alloc_context();
    m->fmt->pb = m->io;
    const AVInputFormat *ea = av_find_input_format("ea");
    if (avformat_open_input(&m->fmt, nullptr, ea, nullptr) < 0) {
        fprintf(stderr, "Movie: not an EA movie container\n");
        m.reset();
        return false;
    }
    avformat_find_stream_info(m->fmt, nullptr);
    m->videoStream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m->audioStream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m->videoStream < 0 || !(m->video = OpenDecoder(m->fmt->streams[m->videoStream]))) {
        fprintf(stderr, "Movie: no decodable video stream\n");
        m.reset();
        return false;
    }
    if (m->audioStream >= 0 && (m->audio = OpenDecoder(m->fmt->streams[m->audioStream])) && AudioDevice()) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&m->swr, &stereo, AV_SAMPLE_FMT_S16, kAudioRate, &m->audio->ch_layout,
                            m->audio->sample_fmt, m->audio->sample_rate, 0, nullptr);
        if (!m->swr || swr_init(m->swr) < 0)
            swr_free(&m->swr);
    }
    m->pkt = av_packet_alloc();
    m->frame = av_frame_alloc();
    m_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, m->video->width,
                                  m->video->height);
    if (g_audio)
        SDL_ClearQueuedAudio(g_audio);
    m->startTicks = SDL_GetTicks();
    m_finished = false;
    return true;
}

static void QueueAudio(SwrContext *swr, AVFrame *f)
{
    int outSamples = swr_get_out_samples(swr, f->nb_samples);
    std::vector<int16_t> out(size_t(outSamples) * 2);
    uint8_t *dst = reinterpret_cast<uint8_t *>(out.data());
    int got = swr_convert(swr, &dst, outSamples, const_cast<const uint8_t **>(f->extended_data), f->nb_samples);
    if (got > 0)
        SDL_QueueAudio(g_audio, out.data(), Uint32(got) * 2 * sizeof(int16_t));
}

bool MoviePlayer::Update()
{
    if (!m || m_finished)
        return false;
    const double now = (SDL_GetTicks() - m->startTicks) / 1000.0;
    const AVRational vtb = m->fmt->streams[m->videoStream]->time_base;
    bool shown = false;

    while (true) {
        // Show the pending frame once its time has come.
        if (m->pending) {
            if (m->pendingTime > now)
                break;
            AVFrame *f = m->pending;
            SDL_UpdateYUVTexture(m_texture, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                                 f->linesize[2]);
            av_frame_free(&m->pending);
            shown = true;
            continue;
        }
        // Try to get a decoded video frame.
        int r = avcodec_receive_frame(m->video, m->frame);
        if (r == 0) {
            m->pending = av_frame_clone(m->frame);
            int64_t pts = m->frame->best_effort_timestamp;
            m->pendingTime = pts == AV_NOPTS_VALUE ? now : pts * av_q2d(vtb);
            av_frame_unref(m->frame);
            continue;
        }
        if (r == AVERROR_EOF) {
            m_finished = true;
            break;
        }
        // Need more input: demux the next packet.
        if (m->demuxDone) {
            avcodec_send_packet(m->video, nullptr);
            continue;
        }
        if (av_read_frame(m->fmt, m->pkt) < 0) {
            m->demuxDone = true;
            continue;
        }
        if (m->pkt->stream_index == m->videoStream) {
            avcodec_send_packet(m->video, m->pkt);
        } else if (m->pkt->stream_index == m->audioStream && m->audio && m->swr) {
            if (avcodec_send_packet(m->audio, m->pkt) == 0)
                while (avcodec_receive_frame(m->audio, m->frame) == 0) {
                    QueueAudio(m->swr, m->frame);
                    av_frame_unref(m->frame);
                }
        }
        av_packet_unref(m->pkt);
    }
    (void)shown;
    return !m_finished;
}

double MoviePlayer::CurrentTime() const
{
    if (!m)
        return 0.0;
    return (SDL_GetTicks() - m->startTicks) / 1000.0;
}

void MoviePlayer::Stop()
{
    if (g_audio)
        SDL_ClearQueuedAudio(g_audio);
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    m.reset();
}
