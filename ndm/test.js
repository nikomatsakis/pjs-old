let i = 0;
while (i < 100) {
print("hi");
foo = fork(function(ctx) { print("in_fork 1"); setresult(22); });
bar = fork(function(ctx) { print("in_fork 2"); setresult(23); });
oncompletion(function() {
    print("in_completion");
    print("result of foo: ");
    print(foo.get());
    print("result of bar: ");
    print(bar.get());
});
i++;
}
