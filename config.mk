VERSION=0.1
BUILDDATE=20091203

#MANCOUNTRIES=en_GB

CFLAGS=-I. -ggdb -Wall -O2
LDFLAGS=-nopie -lsqlite3

CC=gcc
INSTALL=install
XGETTEXT=xgettext
MSGMERGE=msgmerge
MSGFMT=msgfmt
DOCBOOK2MAN=docbook2man.pl

prefix=/usr/local
mandir=${prefix}/share/man
localedir=${prefix}/share/locale
