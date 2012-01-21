let i = 0;
while (i < 100) {
    print("hi");
    foo = fork(function(x) { print("in_fork 1"); return x; }, "22");
    bar = fork(function() { print("in_fork 2"); return 23; });
    oncompletion(function() {
        print("in_completion");
        print("result of foo: ");
        print(foo.get());
        print("result of bar: ");
        print(bar.get());
    });
    i++;
}
