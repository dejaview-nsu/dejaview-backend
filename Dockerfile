# инструменты: компилятор, CMake, Ninja, Conan
FROM ubuntu:24.04 AS toolchain
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++-14 cmake ninja-build make perl pipx ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s gcc-14 /usr/bin/gcc && ln -s g++-14 /usr/bin/g++
ENV PIPX_BIN_DIR=/usr/local/bin
RUN pipx install conan==2.33.0 && conan profile detect
WORKDIR /src

# все библиотеки: пересобираются только при изменении conanfile.txt или conan.lock
FROM toolchain AS deps
COPY conanfile.txt conan.lock ./
RUN conan install . --lockfile=conan.lock --output-folder=build --build=missing -s compiler.cppstd=23

# среда разработки для VS Code Dev Containers: всё то же + clangd и git
FROM deps AS dev
RUN apt-get update \
 && apt-get install -y --no-install-recommends clangd git openssh-client \
 && rm -rf /var/lib/apt/lists/*

# скомпилированный код и тесты
FROM deps AS build
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
 && cmake --build build --parallel

# итоговый образ
FROM ubuntu:24.04 AS runtime
COPY --from=build /src/build/dejaview-backend /usr/local/bin/dejaview-backend
USER ubuntu
EXPOSE 8081
CMD ["dejaview-backend"]
