print("hi");
foo = fork(function() {
    print("in_fork");
});
print(foo);
oncompletion(function() {
    print("in_completion");
});
