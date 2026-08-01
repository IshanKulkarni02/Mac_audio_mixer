BUNDLE = build/AudioMixer.driver
CC = clang
CFLAGS = -Wall -Wextra -std=c11 -O2
FRAMEWORKS = -framework CoreAudio -framework CoreFoundation

.PHONY: all clean

all: $(BUNDLE)/Contents/Info.plist $(BUNDLE)/Contents/MacOS/AudioMixer
	codesign --force --deep --sign - $(BUNDLE)

$(BUNDLE)/Contents/MacOS/AudioMixer: src/MixerDriver.c
	mkdir -p $(BUNDLE)/Contents/MacOS
	$(CC) $(CFLAGS) -bundle -o $@ src/MixerDriver.c $(FRAMEWORKS)

$(BUNDLE)/Contents/Info.plist: Info.plist
	mkdir -p $(BUNDLE)/Contents
	cp Info.plist $@

clean:
	rm -rf build
