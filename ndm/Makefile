SRC=./src
INSTALL=./install
BUILD=./build
MOZCENTRAL=./mozilla-central
.PHONY : all TAGS clean cleaner build-dir

all: ${BUILD}/spork TAGS

build-dir:
	mkdir -p ${BUILD}

${BUILD}/mozilla-central.loc:
	mkdir -p ${BUILD}
	./checkout-and-build-mozilla-central
	touch ${BUILD}/mozilla-central.loc

${BUILD}/%.o: ${SRC}/%.cpp ${BUILD}/mozilla-central.loc build-dir
	g++ -DDEBUG -g -w -m64 \
		-I ${INSTALL}/include \
		-I ${INSTALL}/include/js \
		-I ${INSTALL}/include/nspr \
		-o $@ -c $<

${BUILD}/spork: ${BUILD}/spork.o ${BUILD}/main.o build-dir
	g++ -m64 \
		-o $@ ${BUILD}/main.o ${BUILD}/spork.o \
			${INSTALL}/lib/libjs_static.a \
			${INSTALL}/lib/libnspr4.a

TAGS: ${BUILD}/mozilla-central.loc
	ctags -e -f TAGS.emacs -R ${SRC} \
		${MOZCENTRAL}/js \
		${MOZCENTRAL}/nsprpub \
		${MOZCENTRAL}/mfbt

clean:
	rm -rf ${BUILD}/*
	rm -rf ${INSTALL}
	rm TAGS.emacs
	find mozilla-central -name "build-spork" | xargs rm -rf

cleaner: clean
	rm -rf mozilla-central