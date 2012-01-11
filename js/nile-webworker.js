
function out() {
    var node = document.createElement('div');
    for (var i = 0; i < arguments.length; i++) {
        node.appendChild(
            document.createTextNode(
                arguments[i].toString() + " "));
    }
    document.body.appendChild(node);
}

var global_workers = [];


function add_worker() {
    var worker = new Worker('webworker-child.js');
    global_workers.push(worker);
}

add_worker();
add_worker();
add_worker();
add_worker();

function Result() {
    this.get = function() {
        throw new Error("get before execute");
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
    execute: function execute(cb) {
        this.cb = cb;
        this.pending = this.children.length;
        this._execute();
    },
    _execute: function _execute() {
        var self = this;
        if (!this.pending) {
            return this.cb();
        }
        if (this.children.length === 0 || global_workers.length === 0) {
            return;
        }

        var work = global_workers.shift();
        var child = this.children.shift();

        work.onmessage = function(msg) {
            var data = msg.data;
            self.pending--;
            child[2].get = function () {
                if (self.pending) {
                    throw new Error("get before execute");
                }
                return data;
            }
            global_workers.push(work);
            self._execute();
        }
        work.postMessage([child[0].toString(), JSON.stringify(child[1])]);
        this._execute();
        return;
    }
}


function square(x) {
    return x * x;
}

var ctx = new Parallel(),
    t1 = ctx.fork(square, 2),
    t2 = ctx.fork(square, 4);
    t3 = ctx.fork(square, 8);
    t4 = ctx.fork(square, 16);
    t5 = ctx.fork(square, 32);

ctx.execute(function() {
    out("Squares:", t1.get(), t2.get(), t3.get(), t4.get(), t5.get());
});

