CC = gcc
CXX = g++
LD = g++
CFLAGS = -g -Wall -DDEBUG
CPPFLAGS =
CXXFLAGS = -g -Wall -DDEBUG
LDFLAGS =
INCL =
LIBS = -lsndfile

YATE_CONFIG = yate-config
#YATE_MODULE_LDFLAGS = $(shell $(YATE_CONFIG) --ldflags) -L$(subst /yate,, $(shell $(YATE_CONFIG) --modules))
#YATE_MODULE_LIBS = $(shell $(YATE_CONFIG) --libs)
YATE_MODULE_LIBS = -lyate -L/home/art/opt/lib
YATE_MODULE_LDFLAGS = -rdynamic -shared -Wl,--unresolved-symbols=ignore-in-shared-libs
INCL += $(shell $(YATE_CONFIG) --includes)
CXXFLAGS += $(shell $(YATE_CONFIG) --cflags)

# Setup make behaviour
.SUFFIXES:
.PHONY: all clean install uninstall

all: sndfile.yate

clean:
	-rm *.yate *.o *.d

install:
	cp sndfile.yate $(shell $(YATE_CONFIG) --modules)/sndfile.yate

uninstall:
	rm $(shell $(YATE_CONFIG) --modules)/sndfile.yate

%.o: %.c
	$(CC) -c $(CFLAGS) $(INCL) -o $@ $<
	@$(CC) -MM $(CFLAGS) $(INCL) -MT $@ -MF $(@:.o=.d) $<

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $(INCL) -o $@ $<
	@$(CXX) -MM $(CXXFLAGS) $(INCL) -MT $@ -MF $(@:.o=.d) $<

%.yate: %.o
	$(LD) $(YATE_MODULE_LDFLAGS) $(YATE_MODULE_LIBS) $(LIBS) -o $@ $^

# Automatic dependency includes
-include *.d
