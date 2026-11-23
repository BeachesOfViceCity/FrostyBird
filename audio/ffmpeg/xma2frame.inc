/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Adapter to FFmpeg 8.x's XMA decoder. SHAPE consumes ONE elementary
 * transform frame, at a caller-selected bit offset, and exposes its 512
 * untrimmed samples in 128-sample units. The ordinary file decoder instead
 * demuxes multiple streams, buffers lookahead, and applies file-level trim.
 * Both front ends use the same tile, spectral and overlap-add implementation.
 */
static av_cold int xma2frame_init(AVCodecContext *ctx)
{
    if ((ctx->ch_layout.nb_channels != 1 && ctx->ch_layout.nb_channels != 2) ||
        ctx->extradata_size != 34 || AV_RL16(ctx->extradata) != 1)
        return AVERROR(EINVAL);
    ctx->block_align = 2048;
    return decode_init(ctx->priv_data, ctx, 0);
}

static int xma2frame_decode(AVCodecContext *ctx, AVFrame *frame, int *got, AVPacket *packet)
{
    WMAProDecodeCtx *s = ctx->priv_data;
    int bits, ret;
    if (packet->size < 2) return AVERROR_INVALIDDATA;
    bits = AV_RB16(packet->data) >> 1;
    if (bits <= 16 || bits == 0x7fff || (bits + 7) / 8 != packet->size)
        return AVERROR_INVALIDDATA;
    if ((ret = init_get_bits(&s->gb, packet->data, bits)) < 0) return ret;
    s->frame_offset = 0;
    s->num_saved_bits = bits;
    s->packet_loss = s->skip_frame = 0;
    frame->nb_samples = 512;
    if ((ret = ff_get_buffer(ctx, frame, 0)) < 0) return ret;
    decode_frame(s, frame, got);
    if (!*got || s->packet_loss || get_bits_left(&s->gb) != 0)
        return AVERROR_INVALIDDATA;
    return packet->size;
}

const FFCodec ff_xma2frame_decoder = {
    .p.name = "xma2frame",
    CODEC_LONG_NAME("XMA2 elementary transform frame"),
    .p.type = AVMEDIA_TYPE_AUDIO,
    .p.id = AV_CODEC_ID_XMA2,
    .priv_data_size = sizeof(WMAProDecodeCtx),
    .init = xma2frame_init,
    .close = wmapro_decode_end,
    FF_CODEC_DECODE_CB(xma2frame_decode),
    .p.capabilities = AV_CODEC_CAP_DR1,
    .flush = wmapro_flush,
    .caps_internal = FF_CODEC_CAP_INIT_CLEANUP,
};
