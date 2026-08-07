OBJDIR:= out
LIBS:= pci.c
SRCS:= pcisave.c vbiosdump.c

CC?= cc
LD?= ld
AR?= ar

DEBUG?= 0

CFLAGS_NODEBUG:= -O2
CFLAGS_DEBUG:= -Og -g -D DEBUG=1

CFLAGS:= -Iinclude -Wall -Wextra -pedantic -std=c99
