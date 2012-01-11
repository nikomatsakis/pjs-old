
function out() {
    var node = document.createElement('div');
    for (var i = 0; i < arguments.length; i++) {
        node.appendChild(
            document.createTextNode(
                arguments[i].toString() + " "));
    }
    document.body.appendChild(node);
}

function Result() {
}

Result.prototype = {
    get: function get() {
        if (this.result === undefined) {
            throw new Error("get before execute");
        }
        return this.result;
    }
}

function Parallel() {
    this.children = [];
}

Parallel.prototype = {
    fork: function fork(fun) {
        var args = [];
        for (var i = 1, len = arguments.length; i < len; i++) {
            args.push(arguments[i]);
        }
        var r = new Result();
        this.children.push([fun, args, r]);
        return r;
    },
    execute: function execute() {
        var c = this.children;
        for (var i = 0, l = c.length; i < l; i++) {
            var child = c[i],
                func = child[0],
                args = child[1],
                res = child[2];

            res.result = func.apply(null, args);
        }
    }
}


function square(x) {
    return x * x;
}


var ctx = new Parallel(),
    r1 = ctx.fork(square, 2),
    r2 = ctx.fork(square, 4);

ctx.execute();

out("Squares:", r1.get(), r2.get());

