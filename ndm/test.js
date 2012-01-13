print("hi");
foo = fork(function() {
    print("in_fork");
});
oncompletion(function() {
    print("in_completion");
});
