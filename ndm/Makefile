all: spork.o main.o
	g++ -m64 -lmozjs -lnspr4 \
		-o spork main.o spork.o

%.o: %.cpp
	g++ -g -w -m64 \
		-I/usr/local/include/nspr \
		-I $(MOZCENTRAL)/js/src/build-debug/dist/include \
		-o $@ -c $<
