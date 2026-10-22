# Пересоздаёт образцы для tests/search/rules_test.cpp и tests/integration/search_test.cpp
# (через tests/search/samples.hpp); формат определяется по содержимому.
# Запуск из корня репозитория, нужен Docker:
#   docker run --rm -v "$PWD/tests/data/search:/out" -v "$PWD/tests/data/search/gen.sh:/gen.sh:ro" \
#       debian:bookworm sh /gen.sh
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends ffmpeg >/dev/null
cd /out
S="-f lavfi -i testsrc=size=16x16:rate=1 -frames:v 1"
V="-f lavfi -i testsrc=size=16x16:rate=2 -frames:v 2"
ff() { ffmpeg -loglevel error -y "$@"; }
ff $S image.jpg
ff $S image.png
ff $S -c:v libwebp image.webp
ff $V -c:v libx264 video.mp4
ff $V -c:v mpeg4 video.mov
ff $V -c:v libvpx-vp9 video.webm
ff $S reject.gif
ff $V -c:v mpeg4 reject.mkv
ff $V -c:v h263 -s 128x96 reject.3gp
# Аудио; A - 0,2 с синуса
A="-f lavfi -i sine=frequency=440:duration=0.2"
ff $A -c:a libmp3lame audio.mp3
ff $A -c:a pcm_s16le audio.wav
ff $A -c:a libvorbis audio.ogg
ff $A -c:a aac audio.m4a
ff $A -c:a aac -f mp4 audio-isom.m4a
ff $A -c:a flac reject.flac
ff $A -c:a aac -f adts reject.aac
ff -f lavfi -i testsrc=size=16x16:rate=2 $A -frames:v 2 -c:v libx264 -c:a aac reject-av.mp4
ff $A -c:a libopus reject-audio.webm
