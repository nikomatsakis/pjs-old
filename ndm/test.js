print("hi");
foo = fork(function() { print("in_fork 1"); });
bar = fork(function() { print("in_fork 2"); });
oncompletion(function() {
    print("in_completion");
});
