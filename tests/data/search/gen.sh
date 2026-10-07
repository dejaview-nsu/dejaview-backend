# Пересоздаёт образцы для tests/search/rules_test.cpp (формат определяется по содержимому).
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
