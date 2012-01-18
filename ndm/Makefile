SRC=./src
BUILD=./build
INSTALL=./build/install
MOZCENTRAL=./mozilla-central
.PHONY : all TAGS clean cleaner build-dir

all: TAGS ${BUILD}/spork

build-dir:
	mkdir -p ${BUILD}

.MC.loc:
	./checkout-mozilla-central

${BUILD}/MC: .MC.loc build-dir
	./build-mozilla-central

${BUILD}/%.o: ${SRC}/%.cpp ${BUILD}/MC
	g++ -DDEBUG -g -w -m64 \
		-I ${INSTALL}/include \
		-I ${INSTALL}/include/js \
		-I ${INSTALL}/include/nspr \
		-o $@ -c $<

${BUILD}/spork: \
		${BUILD}/spork.o \
		${BUILD}/main.o \
		${BUILD}/MC
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
	rm -rf ${BUILD}
	rm TAGS.emacs

cleaner: clean
	rm -rf mozilla-central