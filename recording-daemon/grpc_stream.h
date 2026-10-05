#ifndef RTPENGINE_GRPC_STREAM_H
#define RTPENGINE_GRPC_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct grpc_audio_stream;

#ifdef __cplusplus
// Este bloco deixa os nomes das funcoes iguais quando C e C++ conversam.
extern "C" {
#endif

// Cria um fluxo para uma faixa de audio, como uma das vozes da chamada.
struct grpc_audio_stream *grpc_audio_stream_open(
        const char *target, bool insecure, const char *call_id,
        const char *recording_id, const char *track_id,
        unsigned sample_rate, unsigned channels);

// Coloca um pacote de audio PCM na fila para enviar.
bool grpc_audio_stream_push_pcm(struct grpc_audio_stream *,
        const uint8_t *data, size_t len, int64_t pts_samples);

// Fecha o envio; discarded diz se a gravacao foi descartada.
bool grpc_audio_stream_close(struct grpc_audio_stream *, bool discarded);

#ifdef __cplusplus
}
#endif

#endif //RTPENGINE_GRPC_STREAM_H
