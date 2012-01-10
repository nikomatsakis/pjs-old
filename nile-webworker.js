
function out() {
    var node = document.createElement('div');
    for (var i = 0; i < arguments.length; i++) {
        node.appendChild(
            document.createTextNode(
                arguments[i].toString() + " "));
    }
    document.body.appendChild(node);
}

var current_worker = 0,
    global_workers = [],
    // xxx should be unguessable
    current_result = 0,
    // xxx should use weakmap
    global_results = {};


function add_worker() {
    var worker = new Worker('webworker-child.js');
    worker.onmessage = function(msg) {
        var id = msg.data[0],
            val = msg.data[1],
            result = global_results[id];

        delete global_results[id];

        result.onvalue(val);
    }
    global_workers.push(worker);
}

add_worker();
add_worker();
add_worker();
add_worker();

function Result() {
    this.onvalue = function() {};
    this.id = current_result++;
    global_results[this.id] = this;
}

Result.prototype = {
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
        this.children = [];
        for (var i = 0, l = c.length; i < l; i++) {
            if (current_worker >= global_workers.length) {
                current_worker = 0;
            }
            var work = global_workers[current_worker++];

            var child = c[i],
                func = child[0],
                args = child[1],
                res = child[2];

            work.postMessage([func.toString(), JSON.stringify(args), res.id]);
        }
    }
}


function square(x) {
    return x * x;
}

out("Squares:");

var ctx = new Parallel();

ctx.fork(square, 2).onvalue = out;
ctx.fork(square, 4).onvalue = out;

ctx.execute();

