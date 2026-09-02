EXTRAS:= @vbiosdump

.include "${MBDIR}/extras.mk"

${OBJDIR}/vbiosdump: ${SUBDIR}/vbiosdump.c ${NYETPCI_LIB}
	${CC} ${CFLAGS} -o $@ $>
