EXTRAS:= @pcisave
.include "${MBDIR}/extras.mk"

${OBJDIR}/pcisave: ${SUBDIR}/pcisave.c ${NYETPCI_LIB}
	${CC} ${CFLAGS} -o $@ $>
