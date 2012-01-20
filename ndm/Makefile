SRC=./src
BUILD=./build
INSTALL=./build/install
MOZCENTRAL=./mozilla-central
.PHONY : all TAGS clean cleaner

all: ${BUILD}/spork

ifdef BUILD_MEMBRANE
MEMBRANE_O = ${BUILD}/membrane.o
endif

.MC.loc:
	./checkout-mozilla-central

${BUILD}/MC: .MC.loc
	mkdir -p ${BUILD}
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
		${MEMBRANE_O} \
		${BUILD}/MC
	g++ -m64 \
		-o $@ ${BUILD}/main.o ${BUILD}/spork.o \
			${INSTALL}/lib/libjs_static.a \
			${INSTALL}/lib/libnspr4.a

TAGS: .MC.loc
	ctags -e -f TAGS.emacs -R ${SRC} \
		$(cat .MC.loc)/js \
		$(cat .MC.loc)/nsprpub \
		$(cat .MC.loc)/mfbt

clean:
	rm -rf ${BUILD}
	rm TAGS.emacs

cleaner: clean
	rm -rf mozilla-central