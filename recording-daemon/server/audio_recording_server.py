#!/usr/bin/env python3
"""Recebe o audio do rtpengine e guarda cada faixa em um arquivo WAV."""

import argparse
import logging
import os
import re
import tempfile
import uuid
import wave
from concurrent import futures
from pathlib import Path

import grpc

import audio_recording_pb2 as pb
import audio_recording_pb2_grpc as pb_grpc


MAX_CHUNK_BYTES = 1024 * 1024
SAMPLE_WIDTH_BYTES = 2


def safe_name(value):
    """Troca caracteres estranhos para o texto nao virar um caminho de arquivo."""
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._-")
    return cleaned[:80] or "audio"


class AudioRecorder(pb_grpc.AudioRecorderServicer):
    def __init__(self, output_dir):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)

    def StreamRecording(self, request_iterator, context):
        # A primeira mensagem precisa dizer de quem e o audio e qual formato usa.
        first_event = next(request_iterator, None)
        if first_event is None or first_event.WhichOneof("event") != "start":
            return pb.RecordingResult(
                accepted=False,
                message="A primeira mensagem precisa ser RecordingStart",
            )

        start = first_event.start
        if (
            not start.call_id
            or not start.recording_id
            or not start.track_id
            or start.sample_rate == 0
            or start.channels == 0
            or start.encoding != "pcm_s16le"
        ):
            return pb.RecordingResult(
                accepted=False,
                message="Metadados invalidos ou formato de audio nao suportado",
            )

        logging.info(
            "Fluxo gRPC iniciado: call=%s recording=%s track=%s rate=%d channels=%d",
            start.call_id,
            start.recording_id,
            start.track_id,
            start.sample_rate,
            start.channels,
        )

        # Cada faixa ganha seu proprio WAV; um identificador aleatorio evita
        # sobrescrever uma gravacao anterior com os mesmos nomes.
        unique_id = uuid.uuid4().hex
        filename = "{}-{}-{}-{}.wav".format(
            safe_name(start.call_id),
            safe_name(start.recording_id),
            safe_name(start.track_id),
            unique_id,
        )
        final_path = self.output_dir / filename

        # Primeiro escrevemos num arquivo temporario para nao deixar um WAV
        # pela metade com cara de gravacao pronta.
        temp_fd, temp_name = tempfile.mkstemp(
            prefix=".rtpengine-", suffix=".wav", dir=self.output_dir
        )
        os.close(temp_fd)
        ended = False
        discarded = False
        expected_sequence = 0
        total_samples = 0
        total_bytes = 0
        bytes_per_sample_frame = start.channels * SAMPLE_WIDTH_BYTES

        try:
            with wave.open(temp_name, "wb") as wav_file:
                wav_file.setnchannels(start.channels)
                wav_file.setsampwidth(SAMPLE_WIDTH_BYTES)
                wav_file.setframerate(start.sample_rate)

                # Cada mensagem contem um quadro de audio ou diz que acabou.
                for event in request_iterator:
                    event_type = event.WhichOneof("event")

                    if event_type == "chunk":
                        chunk = event.chunk
                        if chunk.sequence != expected_sequence:
                            raise ValueError("Numero de ordem dos chunks inesperado")
                        if (
                            not chunk.pcm
                            or len(chunk.pcm) > MAX_CHUNK_BYTES
                            or len(chunk.pcm) % bytes_per_sample_frame != 0
                        ):
                            raise ValueError("Tamanho de chunk PCM invalido")

                        wav_file.writeframesraw(chunk.pcm)
                        total_samples += len(chunk.pcm) // bytes_per_sample_frame
                        total_bytes += len(chunk.pcm)
                        expected_sequence += 1
                        if expected_sequence == 1 or expected_sequence % 100 == 0:
                            logging.info(
                                "Audio gRPC recebido: call=%s track=%s chunks=%d bytes=%d",
                                start.call_id,
                                start.track_id,
                                expected_sequence,
                                total_bytes,
                            )
                    elif event_type == "end":
                        discarded = event.end.discarded
                        ended = True
                        if next(request_iterator, None) is not None:
                            raise ValueError("RecordingEnd precisa ser a ultima mensagem")
                        break
                    else:
                        raise ValueError("Evento inesperado depois do inicio")

                if not ended:
                    raise ValueError("O fluxo terminou sem RecordingEnd")

            # O final informa se a gravacao foi descartada, mas os audios
            # recebidos ainda ficam salvos para poderem ser inspecionados.
            if discarded:
                final_path = final_path.with_name(
                    final_path.stem + "-discarded" + final_path.suffix
                )
            os.replace(temp_name, final_path)
        except (grpc.RpcError, OSError, ValueError, wave.Error) as error:
            logging.warning(
                "Gravacao recusada para a faixa %s: %s", start.track_id, error
            )
            try:
                os.unlink(temp_name)
            except FileNotFoundError:
                pass
            except OSError:
                logging.exception("Nao foi possivel remover o WAV temporario %s", temp_name)
            return pb.RecordingResult(accepted=False, message=str(error))

        logging.info(
            "Audio recebido: call=%s recording=%s track=%s samples=%d file=%s",
            start.call_id,
            start.recording_id,
            start.track_id,
            total_samples,
            final_path,
        )
        message = "Audio salvo em {}".format(final_path)
        if discarded:
            message += " (gravacao marcada como descartada)"
        return pb.RecordingResult(accepted=True, message=message)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Servidor gRPC que recebe gravacoes do rtpengine."
    )
    parser.add_argument(
        "--listen",
        default="0.0.0.0:50051",
        help="Endereco e porta onde o servidor espera conexoes",
    )
    parser.add_argument(
        "--output-dir",
        default="./recordings",
        help="Pasta onde os arquivos WAV serao guardados",
    )
    parser.add_argument(
        "--max-streams",
        type=int,
        default=128,
        help="Quantidade maxima de faixas de audio recebidas ao mesmo tempo",
    )
    parser.add_argument(
        "--tls-cert",
        help="Arquivo de certificado TLS do servidor (opcional)",
    )
    parser.add_argument(
        "--tls-key",
        help="Chave privada TLS correspondente ao certificado",
    )
    args = parser.parse_args()
    if args.max_streams < 1:
        parser.error("--max-streams precisa ser maior que zero")
    if bool(args.tls_cert) != bool(args.tls_key):
        parser.error("--tls-cert e --tls-key precisam ser usados juntos")
    return args


def main():
    args = parse_args()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    server = grpc.server(
        futures.ThreadPoolExecutor(max_workers=args.max_streams)
    )
    pb_grpc.add_AudioRecorderServicer_to_server(
        AudioRecorder(args.output_dir), server
    )

    if args.tls_cert:
        # TLS protege o audio enquanto ele passa pela rede.
        with open(args.tls_key, "rb") as key_file:
            private_key = key_file.read()
        with open(args.tls_cert, "rb") as cert_file:
            certificate = cert_file.read()
        credentials = grpc.ssl_server_credentials(
            ((private_key, certificate),)
        )
        bound_port = server.add_secure_port(args.listen, credentials)
    else:
        logging.warning(
            "TLS desligado: use essa conexao apenas numa rede confiavel."
        )
        bound_port = server.add_insecure_port(args.listen)

    if bound_port == 0:
        raise RuntimeError("Nao foi possivel abrir a porta {}".format(args.listen))

    server.start()
    logging.info("Servidor pronto em %s; WAVs serao salvos em %s",
                 args.listen, args.output_dir)
    server.wait_for_termination()


if __name__ == "__main__":
    main()
