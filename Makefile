ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

VERSION := 1.0.0
TARGET  := SMPlusGui_$(VERSION).elf
CFLAGS  += -Wall -g -I. \
           -DPLATFORM_PS5 \
           -DSMPLUS_VERSION=\"$(VERSION)\" \
           -DMG_ARCH=MG_ARCH_UNIX \
           -DMG_TLS=MG_TLS_NONE \
           -DMG_ENABLE_TCPIP=0 \
           -DMG_ENABLE_IPV6=0
LDADD   := -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService -lSceNotification
SRCS    := main.c mongoose.c app_installer.c

PS5_HOST ?= ps5
PS5_PORT ?= 9021

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRCS) mongoose_config.h mongoose.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDADD)

test: $(TARGET)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(TARGET)

clean:
	rm -f SMPlusGui_*.elf
