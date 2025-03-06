#Makefile for mpd inband standalone driver "segbuilder"

CC = g++
CFLAGS = -DSEGBUILD -Wall -W -g 
SRCS = ngx_inband_internal.cpp inband_main.cpp pugixml.cpp
HDRS = inband_main.h

%.o: %.cpp
	$(CC) $(CFLAGS) -o $@ -c $<

all: segbuilder

segbuilder: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ -I.

clean:
	rm -f *.o segbuilder core

