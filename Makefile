# RK3588 V4L2 + DRM Zero-Copy Video Display Example

# Use your specific cross compiler path
CROSS_COMPILE ?= /opt/atk-dlrk3588-toolchain/bin/aarch64-buildroot-linux-gnu-

CXX = $(CROSS_COMPILE)g++

# Ensure your sysroot contains libdrm headers and libraries
SYSROOT = /opt/atk-dlrk3588-toolchain/aarch64-buildroot-linux-gnu/sysroot
SDK_ROOT = /home/zsx/rk3588_linux_sdk
CXXFLAGS = -Wall -O2 -std=c++11 -pthread \
	-I./app -I./config -I./display -I./audio -I./stream -I./autofocus -I./shared -I./assets -I./dw9763_v4l2subdev -I./peripherals/backlight_pwm -I./peripherals/eeprom_version -I./peripherals/key_noblockio \
	-I$(SYSROOT)/usr/include/libdrm -I$(SYSROOT)/usr/include/drm -I$(SDK_ROOT)/external/mpp/inc
LDFLAGS = -pthread -ldrm -lrockchip_mpp

TARGET = v4l2_drm_imx415_cloud_img
PROBE_TARGET = peripheral_probe
SRCS = app/main.cpp \
	config/app_config.cpp \
	display/text_overlay.cpp \
	audio/audio_cloud_overlay.cpp \
	autofocus/autofocus_worker.cpp \
	stream/nv12_overlay_blend.cpp \
	stream/stream_mode.cpp \
	display/drm_mode.cpp \
	peripherals/backlight_pwm/backlight_pwm.cpp \
	peripherals/eeprom_version/eeprom_version.cpp \
	peripherals/key_noblockio/key_select_listener.cpp \
	dw9763_v4l2subdev/dw9763_vcm_sim.cpp
OBJS = $(SRCS:.cpp=.o)
PROBE_SRCS = app/peripheral_probe.cpp peripherals/backlight_pwm/backlight_pwm.cpp peripherals/eeprom_version/eeprom_version.cpp
PROBE_OBJS = $(PROBE_SRCS:.cpp=.o)

all: $(TARGET) $(PROBE_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(PROBE_TARGET): $(PROBE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(PROBE_TARGET) $(OBJS) $(PROBE_OBJS)

.PHONY: all clean
