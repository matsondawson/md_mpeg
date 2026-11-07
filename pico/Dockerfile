# Pico SDK toolchain, so nothing needs installing on the host.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates cmake coreutils g++ gcc gcc-arm-none-eabi git \
	libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib libusb-1.0-0-dev \
	dosfstools mtools ninja-build pkg-config python3 \
	&& rm -rf /var/lib/apt/lists/*

ENV PICO_SDK_PATH=/opt/pico-sdk

RUN git clone --depth 1 --branch 2.3.1 https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk \
	&& git -C /opt/pico-sdk submodule update --init --depth 1 lib/tinyusb lib/mbedtls

# Installed here so the SDK doesn't download and build it during every project configure
RUN git clone --depth 1 --branch 2.3.1 https://github.com/raspberrypi/picotool.git /opt/picotool \
	&& cmake -S /opt/picotool -B /opt/picotool/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build /opt/picotool/build \
	&& cmake --install /opt/picotool/build
