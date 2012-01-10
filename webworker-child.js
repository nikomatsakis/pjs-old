
onmessage = function(msg) {
    var func = msg.data[0],
        args = msg.data[1],
        token = msg.data[2];

    func = eval("(" + func + ")");
    var r = func.apply(null, JSON.parse(args));
    postMessage([token, r]);
}

