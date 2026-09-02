.include "contrib/mb/init.mk"

.include "src/dirs.mk"

.if !make(clean)
.BEGIN:
	@mkdir -p ${OBJS_ALL:H:O:u} ${EXTRAS_ALL:H:O:u}
.endif

build: ${OBJS_ALL} ${EXTRAS_ALL}

${NYETPCI_LIB}: ${NYETPCI}
	${MAKE} -C ${NYETPCI} DEBUG=${DEBUG}

clean:
	@${MAKE} -C ${NYETPCI} clean
.if exists(${OBJDIR})
	rm -rf ${OBJDIR}
.endif

.PHONY: build clean
.MAIN: build
