all: spork.o main.o
	g++ -m64 -lmozjs -lnspr4 \
		-o spork main.o spork.o

#mozilla-central.loc:
#	./checkout-and-build-mozilla-central

%.o: %.cpp #mozilla-central.loc
	g++ -DDEBUG -g -w -m64 \
		-I/usr/local/include/nspr \
		-I $(MOZCENTRAL)/js/src/build-debug/dist/include \
		-o $@ -c $<
