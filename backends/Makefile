.PHONY: all clean full
# Backends that can only be built on Linux
LINUX_BACKENDS = midi.so evdev.so
# Backends that can only be built on Windows (mostly due to the .DLL extension)
WINDOWS_BACKENDS = artnet.dll osc.dll loopback.dll sacn.dll maweb.dll winmidi.dll openpixelcontrol.dll rtpmidi.dll wininput.dll visca.dll mqtt.dll
# Backends that can be built on any platform that can load .SO libraries
BACKENDS = artnet.so osc.so loopback.so sacn.so lua.so maweb.so jack.so openpixelcontrol.so python.so rtpmidi.so visca.so mqtt.so
# Backends that require huge dependencies to be installed
OPTIONAL_BACKENDS = ola.so
# Backends that need to be built manually (but still should be included in the clean target)
MANUAL_BACKENDS = lua.dll

# The backend library, providing platform-independent abstractions for common things
BACKEND_LIB = libmmbackend.o

# Evaluate which system we are on
SYSTEM := $(shell uname -s)

# Generate debug symbols unless overridden
CFLAGS ?= -g
CXXFLAGS ?= -g

# All backends are shared libraries
CFLAGS += -fPIC -I../ -Wall -Wpedantic
CXXFLAGS += -fPIC -I../
LDFLAGS += -shared

# MB: fix for OS X
MD5_INCLUDE = /opt/homebrew/Cellar/openssl@3/3.5.0/include
JACK_INCLUDE = /opt/homebrew/opt/jack/include
JACK_LIB = /opt/homebrew/opt/jack/lib  

CFLAGS += -I$(MD5_INCLUDE) -I$(JACK_INCLUDE)
LDFLAGS += -L$(JACK_LIB)

# Build Linux backends if possible
ifeq ($(SYSTEM),Linux)
BACKENDS += $(LINUX_BACKENDS)
endif
# Convince OSX that missing functions are present at runtime
ifeq ($(SYSTEM),Darwin)
LDFLAGS += -undefined dynamic_lookup
endif

# Most of these next few backends just pull in the backend lib, some set additional flags
artnet.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
artnet.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
artnet.dll: LDLIBS += -lws2_32

osc.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
osc.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
osc.dll: LDLIBS += -lws2_32

sacn.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
sacn.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
sacn.dll: LDLIBS += -lws2_32

visca.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
visca.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
visca.dll: LDLIBS += -lws2_32

mqtt.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
mqtt.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
mqtt.dll: LDLIBS += -lws2_32

openpixelcontrol.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
openpixelcontrol.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
openpixelcontrol.dll: LDLIBS += -lws2_32

maweb.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
# On OSX, the system provides libressl but no pkg-config data.
# Brew provides OpenSSL v1.1, but with a mangled path and no clear way to find it
# Give up and just default to -lcrypto for the time being...
maweb.so: LDLIBS = $(shell pkg-config --libs openssl || echo "-lcrypto")
maweb.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
maweb.dll: LDLIBS += -lws2_32
maweb.dll: CFLAGS += -DMAWEB_NO_LIBSSL

rtpmidi.so: ADDITIONAL_OBJS += $(BACKEND_LIB)
rtpmidi.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
rtpmidi.dll: LDLIBS += -lws2_32 -liphlpapi

winmidi.dll: ADDITIONAL_OBJS += $(BACKEND_LIB)
winmidi.dll: LDLIBS += -lwinmm -lws2_32

wininput.dll: LDLIBS += -lwinmm

jack.so: LDLIBS = -ljack -lpthread
midi.so: LDLIBS = -lasound
evdev.so: CFLAGS += $(shell pkg-config --cflags libevdev || echo "-DBUILD_ERROR=\"Missing pkg-config data for libevdev\"")
evdev.so: LDLIBS = $(shell pkg-config --libs libevdev || echo "-DBUILD_ERROR=\"Missing pkg-config data for libevdev\"")
ola.so: LDLIBS = -lola
ola.so: CXXFLAGS += -std=c++11 -Wno-write-strings

# The pkg-config name for liblua5.3 is subject to discussion. I prefer 'lua5.3' (which works on Debian and OSX),
# but Arch requires 'lua53' which works on Debian, too, but breaks on OSX.
lua.so: CFLAGS += $(shell pkg-config --cflags lua53 || pkg-config --cflags lua5.3 || pkg-config --cflags lua || echo "-DBUILD_ERROR=\"Missing pkg-config data for lua53\"")
lua.so: LDLIBS += $(shell pkg-config --libs lua53 || pkg-config --libs lua5.3 || pkg-config --libs lua || echo "-DBUILD_ERROR=\"Missing pkg-config data for lua53\"")
lua.dll: CFLAGS += $(shell pkg-config --cflags lua53 || pkg-config --cflags lua5.3 || pkg-config --cflags lua || echo "-DBUILD_ERROR=\"Missing pkg-config data for lua53\"")
lua.dll: LDLIBS += -L../ -llua53

# Python seems to ship their own little python3-config tool instead of properly maintaining their pkg-config files.
# This one also spams a good deal of unwanted flags into CFLAGS, so we use only --includes. On the other hand, the --libs
# info from this one seems to include the actual interpreter library only on some systems, which makes it worse than useless.
python.so: CFLAGS += $(shell python3-config --includes || pkg-config --cflags python3 || pkg-config --cflags python || echo "-DBUILD_ERROR=\"Missing pkg-config data for python3\"")
python.so: LDLIBS += $(shell pkg-config --libs python3-embed || python3-config --libs || pkg-config --libs python3 || echo "-DBUILD_ERROR=\"Missing pkg-config data for python3\"")
python.so: LDFLAGS += $(shell python3-config --ldflags || echo "-DBUILD_ERROR=\"Missing pkg-config data for python3\"")
python.dll: CFLAGS += $(shell python3-config --includes || echo "-DBUILD_ERROR=\"Missing pkg-config data for python3\"")
python.dll: LDLIBS += -L../ -lpython3

# Generic rules on how to build .SO/.DLL's from C and CPP sources
%.so :: %.c %.h $(BACKEND_LIB)
	$(CC) $(CFLAGS) $< $(ADDITIONAL_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

%.dll :: %.c %.h $(BACKEND_LIB)
	$(CC) $(CFLAGS) $< $(ADDITIONAL_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

%.so :: %.cpp %.h
	$(CXX) $(CXXFLAGS) $< $(ADDITIONAL_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

# This is the actual first named target, and thus the default
all: $(BACKEND_LIB) $(BACKENDS)

# Build an import lib for the windows build if it's not already there
../libmmapi.a:
	$(MAKE) -C ../ midimonster.exe

# Override a bunch of stuff for the windows target and it's DLL dependencies
%.dll: export CC = x86_64-w64-mingw32-gcc
%.dll: LDLIBS += -lmmapi
%.dll: LDFLAGS += -L../
%.dll: CFLAGS += -Wno-format -Wno-pointer-sign
windows: CFLAGS += -Wno-format -Wno-pointer-sign
windows: export CC = x86_64-w64-mingw32-gcc
windows: ../libmmapi.a $(BACKEND_LIB) $(WINDOWS_BACKENDS)

# Optional target including the backends that require large dependencies
full: $(BACKEND_LIB) $(BACKENDS) $(OPTIONAL_BACKENDS)

# Clean up all generated files
clean:
	$(RM) $(BACKEND_LIB) $(BACKENDS) $(OPTIONAL_BACKENDS) $(WINDOWS_BACKENDS) $(MANUAL_BACKENDS)
