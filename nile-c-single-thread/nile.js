var p = new Parallel();
p.fork(function() {print("hi")});
p.fork(function() {print("hello")});
p.execute();
