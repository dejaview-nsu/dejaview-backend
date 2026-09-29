# компилятор, все библиотеки, скомпилированный код и тесты
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++-14 cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 \
 && cmake --build build --parallel

# итоговый образ
FROM ubuntu:24.04 AS runtime
COPY --from=build /src/build/dejaview-backend /usr/local/bin/dejaview-backend
USER ubuntu
CMD ["dejaview-backend"]
