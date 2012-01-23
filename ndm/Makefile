SRC=./src
BUILD=./build
INSTALL=./build/install
MOZCENTRAL=./mozilla-central
OBJS =											\
	${BUILD}/pjs.o								\
	${BUILD}/main.o								\
	${BUILD}/membrane.o

.PHONY : all TAGS clean cleaner

all: ${BUILD}/spork

.MC.loc:
	./checkout-mozilla-central

${BUILD}/MC: .MC.loc
	mkdir -p ${BUILD}
	./build-mozilla-central

${BUILD}/%.o: ${SRC}/%.cpp ${BUILD}/MC
	g++ -DDEBUG -O0 -g -w -m64						\
		$$(${BUILD}/install/bin/js-config --cflags)	\
		-o $@ -c $<

${BUILD}/spork: ${OBJS} ${BUILD}/MC
	g++ -m64										\
		$$(${BUILD}/install/bin/js-config --libs)	\
		-o $@ ${OBJS}

TAGS: .MC.loc
	ctags -e -f TAGS.emacs -R ${SRC}			\
		$$(cat .MC.loc)/js						\
		$$(cat .MC.loc)/nsprpub					\
		$$(cat .MC.loc)/mfbt

clean:
	rm -rf ${BUILD}
	rm -f TAGS.emacs

cleaner: clean
	rm -rf mozilla-central .MC.loc
