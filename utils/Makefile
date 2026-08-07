# Change the configuration here!!!
.include "common.mk"

#
# Targets:
#   make build - Build the utilities
#   make clean - Clean up
#

.if ${DEBUG} == 1
CFLAGS+= ${CFLAGS_DEBUG}
.else
CFLAGS+= ${CFLAGS_NODEBUG}
.endif

# Generate library object directives
LIBFILES:=
.for f in ${LIBS}
${OBJDIR}/${f:S/.c/.o/}: lib/${f}
	${CC} ${CFLAGS} -c -o $@ $^
LIBFILES+= ${OBJDIR}/${f:S/.c/.o/}
.endfor

# Generate executable directives
EXEFILES:=
.for f in ${SRCS}
${OBJDIR}/${f:S/.c/.elf/}: src/${f} ${LIBFILES}
	${CC} ${CFLAGS} -o $@ $^
EXEFILES+= ${OBJDIR}/${f:S/.c/.elf/}
.endfor

# Create objdir, compile libraries, compile executables
build: ${OBJDIR} ${LIBFILES} ${EXEFILES}

libs: ${OBJDIR} ${LIBFILES}

clean: ${OBJDIR}
	rm -rf ${OBJDIR}

${OBJDIR}:
	mkdir -p ${OBJDIR}

.PHONY: clean build
.MAIN: build
