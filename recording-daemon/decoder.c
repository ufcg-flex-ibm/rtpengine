#include "decoder.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <glib.h>
#include <stdint.h>
#include <libavutil/opt.h>
#include "types.h"
#include "log_r.h"
#include "str.h"
#include "output.h"
#include "mix.h"
#include "resample.h"
#include "codeclib.h"
#include "streambuf.h"
#include "main.h"
#include "packet.h"
#include "tag.h"
#include "tls_send.h"
#include "fix_frame_channel_layout.compat"
#ifdef WITH_GRPC
#include "grpc_stream.h"
#endif


// does not initialise the contained `sink`
decode_t *decoder_new(const char *payload_str, const char *format, int ptime) {
	const char *slash = strchr(payload_str, '/');
	if (!slash) {
		ilog(LOG_WARN, "Invalid payload format: %s", payload_str);
		return NULL;
	}

	str name = STR_LEN(payload_str, slash - payload_str);
	int clockrate = atoi(slash + 1);
	if (clockrate <= 0) {
		ilog(LOG_ERR, "Invalid clock rate %i (parsed from '%.20s'/'%.20s')",
				clockrate, slash + 1, payload_str);
		return NULL;
	}

	int channels = 1;
	slash = strchr(slash + 1, '/');
	if (slash) {
		channels = atoi(slash + 1);
		if (!channels)
			channels = 1;
	}

	codec_def_t *def = codec_find(&name, MT_AUDIO);
	if (!def) {
		ilog(LOG_WARN, "No decoder for payload %s", payload_str);
		return NULL;
	}
	if (def->supplemental || !def->support_decoding || def->media_type != MT_AUDIO) {
		// not a real audio codec
		ilog(LOG_DEBUG, "Not decoding codec %s", payload_str);
		return NULL;
	}

	// decoder_new_fmt already handles the clockrate_mult scaling
	int rtp_clockrate = clockrate;
	clockrate = fraction_mult(clockrate, &def->default_clockrate_fact);

	format_t out_format = {
		.clockrate = clockrate,
		.channels = channels,
		.format = AV_SAMPLE_FMT_S16,
	};
	format_t dec_format = (format_t) {
		.clockrate = rtp_clockrate,
		.channels = channels,
		.format = -1,
	};

	str fmtp = STR(format);

	decoder_t *dec = decoder_new_fmtp(def, &dec_format, ptime, &out_format, NULL, &fmtp, NULL);
	if (!dec)
		return NULL;
	decode_t *deco = g_new0(decode_t, 1);
	deco->dec = dec;
	return deco;
}


static int decoder_got_frame(decoder_t *dec, AVFrame *frame, void *sp, void *dp) {
	ssrc_t *ssrc = sp;
	metafile_t *metafile = ssrc->metafile;
	output_t *output = ssrc->output;
	stream_t *stream = ssrc->stream;
	decode_t *deco = dp;

	dbg("got frame pts %llu samples %u contents %02x%02x%02x%02x...", (unsigned long long) frame->pts, frame->nb_samples,
			(unsigned int) frame->extended_data[0][0],
			(unsigned int) frame->extended_data[0][1],
			(unsigned int) frame->extended_data[0][2],
			(unsigned int) frame->extended_data[0][3]);

	if (metafile->recording_on) {
		sink_add(&deco->mix_sink, frame);

#ifdef WITH_GRPC
		// So envia se o gRPC foi ligado e esta faixa ainda nao teve erro.
		if (grpc_target && !ssrc->grpc_failed) {
			unsigned channels = GET_CHANNELS(frame);
			// O servidor espera audio PCM de 16 bits com formato valido.
			if (frame->format != AV_SAMPLE_FMT_S16 || frame->nb_samples <= 0
					|| frame->sample_rate <= 0 || channels == 0) {
				ilog(LOG_ERR, "Invalid PCM frame format for gRPC recording SSRC %08lx",
						ssrc->ssrc);
				ssrc->grpc_failed = 1;
			}
			if (!ssrc->grpc_stream) {
				// Abre a conexao na primeira vez que chega audio desta faixa.
				if (!ssrc->grpc_failed
						&& (!ssrc->grpc_call_id || !ssrc->grpc_recording_id)) {
					ilog(LOG_ERR, "Cannot start gRPC recording for SSRC %08lx: call ID or recording ID is missing",
							ssrc->ssrc);
					ssrc->grpc_failed = 1;
				}
				else if (!ssrc->grpc_failed) {
					ssrc->grpc_stream = grpc_audio_stream_open(grpc_target,
							grpc_insecure, ssrc->grpc_call_id,
							ssrc->grpc_recording_id, ssrc->grpc_track_id,
							frame->sample_rate, channels);
					if (!ssrc->grpc_stream) {
						ilog(LOG_ERR, "Failed to open gRPC recording stream for SSRC %08lx",
								ssrc->ssrc);
						ssrc->grpc_failed = 1;
					}
					else {
						ssrc->grpc_sample_rate = frame->sample_rate;
						ssrc->grpc_channels = channels;
					}
				}
			}

			// A mesma faixa deve continuar usando a mesma taxa e quantidade de canais.
			if (ssrc->grpc_stream && !ssrc->grpc_failed
					&& (ssrc->grpc_sample_rate != (unsigned) frame->sample_rate
						|| ssrc->grpc_channels != channels)) {
				ilog(LOG_ERR, "gRPC audio format changed within SSRC %08lx",
						ssrc->ssrc);
				ssrc->grpc_failed = 1;
			}

			if (ssrc->grpc_stream && !ssrc->grpc_failed) {
				// Cada quadro decodificado vira um pedaco enviado ao servidor.
				size_t pcm_len = (size_t) frame->nb_samples
					* channels * sizeof(int16_t);
				if (!grpc_audio_stream_push_pcm(ssrc->grpc_stream,
							frame->extended_data[0], pcm_len, frame->pts)) {
					ilog(LOG_ERR, "Failed to queue PCM chunk for gRPC recording SSRC %08lx",
							ssrc->ssrc);
					ssrc->grpc_failed = 1;
				}
			}
		}
#endif

		if (output) {
			dbg("SSRC %lx of stream #%lu has single output", ssrc->ssrc, stream->id);
			if (!sink_add(&output->sink, frame))
				ilog(LOG_ERR, "Failed to add decoded packet to individual output");
		}
	}

	if (metafile->forwarding_on)
		sink_add(&deco->tls_mix_sink, frame);

	if (ssrc->tls_fwd) {
		dbg("SSRC %lx of stream #%lu has TLS forwarding stream", ssrc->ssrc, stream->id);
		if (!sink_add(&ssrc->tls_fwd->sink, frame))
			ilog(LOG_ERR, "Failed to add decoded packet to TLS/TCP forward output");

	}

	av_frame_free(&frame);
	return 0;
}


int decoder_input(decode_t *deco, const str *data, unsigned long ts, ssrc_t *ssrc) {
	return decoder_input_data(deco->dec, data, ts, false, decoder_got_frame, ssrc, deco); // XXX support mark
}

void decoder_free(decode_t *deco) {
	if (!deco)
		return;
	decoder_close(deco->dec);
	sink_close(&deco->mix_sink);
	sink_close(&deco->tls_mix_sink);
	g_free(deco);
}
