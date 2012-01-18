SRC=.
INSTALL=./install
BUILD=./build
MOZCENTRAL=./mozilla-central

all: ${BUILD}/spork

${BUILD}/mozilla-central.loc:
	./checkout-and-build-mozilla-central
	touch ${BUILD}/mozilla-central.loc

${BUILD}/%.o: ${SRC}/%.cpp ${BUILD}/mozilla-central.loc
	g++ -DDEBUG -g -w -m64 \
		-I ${INSTALL}/include \
		-I ${INSTALL}/include/js \
		-I ${INSTALL}/include/nspr \
		-o $@ -c $<

${BUILD}/spork: ${BUILD}/spork.o ${BUILD}/main.o
	g++ -m64 -L ${INSTALL}/lib -lmozjs -lnspr4 \
		-o $@ ${BUILD}/main.o ${BUILD}/spork.o

TAGS.emacs: ${BUILD}/mozilla-central.loc
	ctags -e -f $@ -R ${SRC} ${MOZCENTRAL}

clean:
	rm -rf ${BUILD}/*
	rm -rf ${INSTALL}
	rm -rf mozilla-central
