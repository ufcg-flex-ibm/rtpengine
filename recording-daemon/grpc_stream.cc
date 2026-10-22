#include "grpc_stream.h"
#include "audio_recording.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

// Cada pacote de audio tem um limite, e a fila tambem tem um limite.
// Assim, uma rede lenta nao faz a memoria crescer sem parar.
constexpr size_t max_chunk_bytes = 1024 * 1024;
constexpr size_t max_queued_bytes = 8 * 1024 * 1024;

using rtpengine::recording::v1::AudioRecorder;
using rtpengine::recording::v1::RecordingEvent;
using rtpengine::recording::v1::RecordingResult;

} // namespace

struct grpc_audio_stream {
	// Dados que identificam esta chamada e este pedaco da conversa.
	std::string target;
	std::string call_id;
	std::string recording_id;
	std::string track_id;
	uint32_t sample_rate;
	uint32_t channels;

	// A fila guarda audio esperando para ser enviado.
	// O mutex protege a fila, e ready acorda a thread quando chega audio.
	std::mutex mutex;
	std::condition_variable ready;
	std::deque<RecordingEvent> queue;
	size_t queued_bytes = 0;
	uint64_t next_sequence = 0;

	// Estas marcas dizem se estamos fechando, descartando ou se algo falhou.
	bool closing = false;
	bool discarded = false;
	bool failed = false;
	bool accepted = false;
	std::thread worker;

	// Guarda os dados da chamada; ainda nao comeca a enviar audio.
	grpc_audio_stream(const char *target_value, bool insecure,
			const char *call_id_value,
			const char *recording_id_value, const char *track_id_value,
			unsigned rate, unsigned channel_count)
		: target(target_value), call_id(call_id_value),
		  recording_id(recording_id_value), track_id(track_id_value),
		  sample_rate(rate), channels(channel_count),
		  insecure_transport(insecure) {}

	// Cria a trabalhadora que conversa com o servidor sem parar o RTP.
	void start() {
		worker = std::thread(&grpc_audio_stream::run, this);
	}

	// Se a comunicacao tiver uma excecao, marca o fluxo como falho.
	void run() {
		try {
			run_rpc();
		}
		catch (const std::exception &error) {
			std::fprintf(stderr, "[gRPC ERROR] Stream failed for call '%s', track '%s': %s\n",
					call_id.c_str(), track_id.c_str(), error.what());
			std::lock_guard<std::mutex> lock(mutex);
			failed = true;
			accepted = false;
		}
	}

	// Abre o fluxo gRPC, manda o inicio, os audios da fila e o evento final.
	void run_rpc() {
		// Usa uma conexao segura, a menos que o modo sem TLS tenha sido pedido.
		std::shared_ptr<grpc::ChannelCredentials> credentials =
			grpc::SslCredentials(grpc::SslCredentialsOptions{});
		if (insecure_transport)
			credentials = grpc::InsecureChannelCredentials();
		auto channel = grpc::CreateChannel(target, credentials);
		auto stub = AudioRecorder::NewStub(channel);
		grpc::ClientContext context;
		RecordingResult result;
		std::unique_ptr<grpc::ClientWriter<RecordingEvent>> writer(
				stub->StreamRecording(&context, &result));

		std::fprintf(stderr,
				"[gRPC INFO] Opening stream to %s for call '%s', recording '%s', track '%s'\n",
				target.c_str(), call_id.c_str(), recording_id.c_str(), track_id.c_str());

		// A primeira mensagem avisa quem esta ligando e como o audio esta formatado.
		RecordingEvent event;
		auto *start_event = event.mutable_start();
		start_event->set_call_id(call_id);
		start_event->set_recording_id(recording_id);
		start_event->set_track_id(track_id);
		start_event->set_sample_rate(sample_rate);
		start_event->set_channels(channels);
		start_event->set_encoding("pcm_s16le");
		bool ok = writer->Write(event);
		size_t sent_chunks = 0;
		size_t sent_bytes = 0;

		// Espera audio novo e manda os pacotes em ordem.
		while (ok) {
			{
				std::unique_lock<std::mutex> lock(mutex);
				ready.wait(lock, [this] { return closing || !queue.empty(); });
				if (queue.empty() && closing)
					break;

				event = std::move(queue.front());
				queued_bytes -= event.chunk().pcm().size();
				queue.pop_front();
			}

			size_t chunk_bytes = event.chunk().pcm().size();
			ok = writer->Write(event);
			if (ok) {
				++sent_chunks;
				sent_bytes += chunk_bytes;
				if (sent_chunks == 1 || sent_chunks % 100 == 0)
					std::fprintf(stderr,
							"[gRPC INFO] Audio sent for call '%s', track '%s': %zu chunks, %zu bytes\n",
							call_id.c_str(), track_id.c_str(), sent_chunks, sent_bytes);
			}
		}

		// A ultima mensagem avisa que acabou e se a gravacao foi descartada.
		if (ok) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				event.Clear();
				event.mutable_end()->set_discarded(discarded || failed);
			}
			ok = writer->Write(event);
		}

		writer->WritesDone();
		grpc::Status status = writer->Finish();
		std::lock_guard<std::mutex> lock(mutex);
		accepted = ok && status.ok() && result.accepted() && !failed;
		failed = failed || !accepted;
		if (accepted)
			std::fprintf(stderr,
					"[gRPC INFO] Recording accepted for call '%s', track '%s': %zu chunks, %zu bytes\n",
					call_id.c_str(), track_id.c_str(), sent_chunks, sent_bytes);
		else
			std::fprintf(stderr,
					"[gRPC ERROR] Recording failed for call '%s', track '%s': status=%s, server_message='%s'\n",
					call_id.c_str(), track_id.c_str(),
					status.error_message().c_str(), result.message().c_str());
	}

	// Copia um pedaco de audio para a fila. Esta funcao nao espera a rede enviar.
	bool push(const uint8_t *data, size_t len, int64_t pts_samples) {
		std::lock_guard<std::mutex> lock(mutex);
		if (closing || failed)
			return false;
		if (!data || len == 0 || len > max_chunk_bytes
				|| len % sizeof(int16_t) != 0
				|| len > max_queued_bytes - queued_bytes) {
			failed = true;
			closing = true;
			ready.notify_one();
			return false;
		}

		// Um numero crescente ajuda o servidor a conferir a ordem dos pedacos.
		RecordingEvent event;
		auto *chunk = event.mutable_chunk();
		chunk->set_sequence(next_sequence);
		chunk->set_pts_samples(pts_samples);
		chunk->set_pcm(data, len);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		// O formato combinado e little-endian; troca os bytes se a maquina
		// guardar os numeros na ordem contraria.
		std::string *pcm = chunk->mutable_pcm();
		for (size_t i = 0; i < len; i += sizeof(int16_t))
			std::swap((*pcm)[i], (*pcm)[i + 1]);
#endif
		++next_sequence;
		queued_bytes += len;
		queue.push_back(std::move(event));
		ready.notify_one();
		return true;
	}

	// Pede para terminar e espera a trabalhadora confirmar o resultado final.
	bool finish(bool was_discarded) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			closing = true;
			discarded = discarded || was_discarded;
		}
		ready.notify_one();
		if (worker.joinable())
			worker.join();
		return accepted;
	}

	// Avisa a trabalhadora que algo deu errado e que ela deve parar.
	void abort() {
		std::lock_guard<std::mutex> lock(mutex);
		failed = true;
		closing = true;
		ready.notify_one();
	}

	// Ultima protecao: nao deixa a thread ficar rodando depois que o objeto some.
	~grpc_audio_stream() {
		if (worker.joinable()) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				closing = true;
			}
			ready.notify_one();
			worker.join();
		}
	}

private:
	// Guarda se devemos falar com TLS ou sem TLS.
	bool insecure_transport = false;
};

// Funcao que o codigo C usa para preparar um fluxo de audio novo.
extern "C" struct grpc_audio_stream *grpc_audio_stream_open(
		const char *target, bool insecure, const char *call_id,
		const char *recording_id, const char *track_id,
		unsigned sample_rate, unsigned channels) {
	if (!target || !*target || !call_id || !*call_id || !recording_id
			|| !*recording_id || !track_id || !*track_id
			|| sample_rate == 0 || channels == 0)
		return nullptr;

	// Monta o fluxo e liga sua trabalhadora; em erro, devolve "nao consegui".
	try {
		std::unique_ptr<grpc_audio_stream> stream(new grpc_audio_stream(target,
					insecure, call_id, recording_id, track_id, sample_rate, channels));
		stream->start();
		return stream.release();
	}
	catch (const std::exception &) {
		return nullptr;
	}
}

// Funcao que o codigo C usa para colocar um pedaco de PCM na fila.
extern "C" bool grpc_audio_stream_push_pcm(struct grpc_audio_stream *stream,
		const uint8_t *data, size_t len, int64_t pts_samples) {
	if (!stream)
		return false;
	try {
		return stream->push(data, len, pts_samples);
	}
	catch (const std::exception &) {
		stream->abort();
		return false;
	}
}

// Funcao que o codigo C usa para fechar o fluxo e liberar a memoria.
extern "C" bool grpc_audio_stream_close(struct grpc_audio_stream *stream,
		bool discarded) {
	if (!stream)
		return true;
	bool accepted = stream->finish(discarded);
	delete stream;
	return accepted;
}
