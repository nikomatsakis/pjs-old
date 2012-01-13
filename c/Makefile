all: spork.o
	g++ -m64 -lmozjs -lnspr4 -I/usr/local/include/nspr -I ../../mozilla-central/js/src/build-new/dist/include -o spork main.cpp jsworkers.cpp

spork.o: spork.cpp
	g++ -w -m64 -lmozjs -lnspr4 -I/usr/local/include/nspr -I ../../drd-js/js/src/build-debug/dist/include -o spork.o -c spork.cpp
