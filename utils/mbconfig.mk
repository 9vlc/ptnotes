NYETPCI:= ${PROJDIR}/contrib/nyetpci
NYETPCI_LIB:= ${NYETPCI}/build/lib/nyetpci.a

CFLAGS:= -I${INCDIR} -I${NYETPCI}/include -Wall -Wextra -pedantic -std=c99 -Os
LDFLAGS:= -s
