TARGET=		rtpengine-recording

export top_srcdir = ..

with_grpc ?= no

include ../lib/deps.Makefile

include ../lib/flags.Makefile

CFLAGS += $(CFLAGS_LIBPCRE)
CFLAGS += $(CFLAGS_GLIB)
CFLAGS += $(CFLAGS_JSON_GLIB)
CFLAGS += $(CFLAGS_GTHREAD)
CFLAGS += $(CFLAGS_LIBAVCODEC)
CFLAGS += $(CFLAGS_LIBAVFORMAT)
CFLAGS += $(CFLAGS_LIBAVUTIL)
CFLAGS += $(CFLAGS_LIBSWRESAMPLE)
CFLAGS += $(CFLAGS_LIBSWSCALE)
CFLAGS += $(CFLAGS_LIBAVFILTER)
CFLAGS += $(CFLAGS_OPUS)
CFLAGS += $(CFLAGS_MYSQL)
CFLAGS += $(CFLAGS_OPENSSL)
CFLAGS += $(CFLAGS_LIBCURL)
CFLAGS += $(CFLAGS_LIBJWT)

LDLIBS += $(LDLIBS_GLIB)
LDLIBS += $(LDLIBS_JSON_GLIB)
LDLIBS += $(LDLIBS_GTHREAD)
LDLIBS += $(LDLIBS_LIBAVCODEC)
LDLIBS += $(LDLIBS_LIBAVFORMAT)
LDLIBS += $(LDLIBS_LIBAVUTIL)
LDLIBS += $(LDLIBS_LIBSWRESAMPLE)
LDLIBS += $(LDLIBS_LIBSWSCALE)
LDLIBS += $(LDLIBS_LIBAVFILTER)
LDLIBS += $(LDLIBS_OPUS)
LDLIBS += $(LDLIBS_MYSQL)
LDLIBS += $(LDLIBS_OPENSSL)
LDLIBS += $(LDLIBS_LIBCURL)
LDLIBS += $(LDLIBS_LIBJWT)

CFLAGS += $(CFLAGS_BCG729)
LDLIBS += $(LDLIBS_BCG729)

SRCS := epoll.c garbage.c inotify.c main.c metafile.c stream.c recaux.c packet.c \
		decoder.c output.c mix.c db.c log.c forward.c tag.c custom_poller.c notify.c tls_send.c s3.c \
		gcs.c
LIBSRCS := loglib.c auxlib.c rtplib.c codeclib.c resample.c str.c socket.c streambuf.c ssllib.c \
		dtmflib.c bufferpool.c bencode.c http.c s3utils.c oauth.c cchain.c
LIBSRCS += rescale.c
LIBSRCS += $(CODEC_SRCS)
LIBSRCS += arena.c
LIBASM := mvr2s_x64_avx2.S mvr2s_x64_avx512.S mix_in_x64_avx2.S mix_in_x64_avx512bw.S mix_in_x64_sse2.S

ifeq ($(with_grpc),yes)
# O restante do projeto continua sem gRPC, a menos que essa opcao seja ligada.
ifeq ($(shell pkg-config --exists grpc++ protobuf && echo yes),)
$(error with_grpc=yes requires grpc++, protobuf, protoc, and grpc_cpp_plugin)
endif

CFLAGS += -DWITH_GRPC
CXXFLAGS += -Wall -Wextra -std=c++17 -fPIE -pthread -I. -Iproto \
		$(shell pkg-config --cflags grpc++ protobuf)
LDLIBS += $(shell pkg-config --libs grpc++ protobuf) -pthread -lstdc++
CXXSRCS := grpc_stream.cc proto/audio_recording.pb.cc proto/audio_recording.grpc.pb.cc
# Estas ferramentas transformam o arquivo .proto em codigo C++ que o cliente usa.
PROTOC ?= protoc
GRPC_CPP_PLUGIN ?= /usr/bin/grpc_cpp_plugin
ifeq ($(shell command -v $(PROTOC) >/dev/null 2>&1 && echo yes),)
$(error with_grpc=yes requires protoc)
endif
ifeq ($(shell command -v $(GRPC_CPP_PLUGIN) >/dev/null 2>&1 && echo yes),)
$(error with_grpc=yes requires grpc_cpp_plugin)
endif

proto/audio_recording.pb.cc: proto/audio_recording.proto
	$(PROTOC) -I proto --cpp_out=proto $<

proto/audio_recording.pb.h: proto/audio_recording.pb.cc
	@test -f $@ || $(PROTOC) -I proto --cpp_out=proto proto/audio_recording.proto

proto/audio_recording.grpc.pb.cc: proto/audio_recording.proto proto/audio_recording.pb.h
	$(PROTOC) -I proto --grpc_out=proto \
		--plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN) $<

proto/audio_recording.grpc.pb.h: proto/audio_recording.grpc.pb.cc
	@test -f $@ || $(PROTOC) -I proto --grpc_out=proto \
		--plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN) proto/audio_recording.proto

proto/audio_recording.pb.o: proto/audio_recording.pb.h

proto/audio_recording.grpc.pb.o: proto/audio_recording.grpc.pb.h proto/audio_recording.pb.h

grpc_stream.o: proto/audio_recording.grpc.pb.h
endif

MDS := rtpengine-recording.ronn
MANS := $(MDS:.ronn=.8)

include ../lib/common.Makefile

ifeq ($(with_grpc),yes)
ADD_CLEAN += grpc_stream.o proto/audio_recording.pb.o proto/audio_recording.grpc.pb.o \
		proto/audio_recording.pb.cc proto/audio_recording.pb.h \
		proto/audio_recording.grpc.pb.cc proto/audio_recording.grpc.pb.h
endif

install: $(TARGET) $(MANS)
	install -m 0755 -D $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -m 0644 -D $(TARGET).8 $(DESTDIR)/usr/share/man/man8/$(TARGET).8
