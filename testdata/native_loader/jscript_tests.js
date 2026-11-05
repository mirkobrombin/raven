// The JScript engine run by cscript: the language, the built-in objects and the host's objects. Each check compares the text of a
// value with the text it should have; the script prints how many checks passed when all of them held.

var failures = 0, checks = 0;

function check(actual, expected, label) {
	checks++;
	if (String(actual) !== String(expected)) {
		failures++;
		WScript.StdErr.WriteLine("FAIL " + label + ": got [" + actual + "], want [" + expected + "]");
	}
}

function thrown(f) {
	try {
		f();
	} catch (e) {
		return e.name + " " + (e.number & 0xffff);
	}
	return "nothing";
}

// Values and operators.
check(1 + 2 * 3, 7, "precedence");
check("1" + 2, "12", "string concatenation");
check("3" * "4", 12, "numeric strings");
check(7 % 3, 1, "remainder");
check(-7 >> 1, -4, "signed shift");
check(-1 >>> 28, 15, "unsigned shift");
check(5 & 3 | 8 ^ 1, 9, "bitwise operators");
check(typeof null + " " + typeof undefined + " " + typeof 1 + " " + typeof "" + " " + typeof {} + " " + typeof check, "object undefined number string object function", "typeof");
check(null == undefined, true, "null equals undefined");
check(null === undefined, false, "null is not undefined");
check("1" == 1, true, "loose equality converts");
check(NaN == NaN, false, "NaN is not itself");
check(0.1 + 0.2, "0.30000000000000004", "shortest digits");
check(1 / 3, "0.3333333333333333", "a third");
check(1e21, "1e+21", "exponent form");
check(123e-20, "1.23e-18", "small exponent");
check(-0.000001, "-0.000001", "small fixed");
check(255 .toString(16), "ff", "radix 16");
check((0.5).toString(2), "0.1", "radix 2 fraction");
check((1.005).toFixed(2), "1.00", "toFixed rounds the exact value");
check((-1.5).toFixed(0), "-2", "toFixed half away");
check((123.456).toExponential(2), "1.23e+2", "toExponential");
check((123.456).toPrecision(4), "123.5", "toPrecision");
check((0.00001234).toPrecision(2), "0.000012", "toPrecision small");
check(parseInt("0x1F"), 31, "parseInt hex");
check(parseInt("017"), 15, "parseInt octal");
check(parseInt("12px", 10), 12, "parseInt prefix");
check(parseFloat("3.25e2abc"), 325, "parseFloat prefix");
check(Number("  12  "), 12, "Number trims");
check(isNaN(Number("x")), true, "Number of text");
check(Number.MAX_VALUE, "1.7976931348623157e+308", "MAX_VALUE");
check(Number.MIN_VALUE, "5e-324", "MIN_VALUE");
check(parseFloat("0.1") === 0.1, true, "reading back");

// Statements.
var total = 0;
for (var i = 0; i < 10; i++) {
	if (i == 7)
		break;
	if (i % 2)
		continue;
	total += i;
}
check(total, 12, "for with break and continue");
var n = 0;
outer: while (true) {
	do {
		n++;
		if (n > 3)
			break outer;
	} while (true);
}
check(n, 4, "labelled break");
function kind(x) {
	switch (x) {
	case 1:
		return "one";
	case "1":
		return "text";
	default:
		return "other";
	}
}
check(kind(1) + kind("1") + kind(2), "onetextother", "switch is strict");
var keys = [];
var record = { a: 1, b: 2, c: 3 };
for (var k in record)
	keys.push(k);
check(keys.join(), "a,b,c", "for in order");
with (record)
	check(a + b + c, 6, "with");
var order = [];
try {
	try {
		throw new Error(1234, "inner");
	} finally {
		order.push("finally");
	}
} catch (e) {
	order.push(e.description, e.number, e.message);
}
check(order.join(), "finally,inner,1234,inner", "try finally catch");
check(thrown(function () { undefined.x; }), "TypeError 5007", "object expected");
check(thrown(function () { missing(); }), "ReferenceError 5009", "undefined name");
check(thrown(function () { (1)(); }), "TypeError 5002", "function expected");
check(thrown(function () { new Array(-1); }), "RangeError 5029", "array length");
check(thrown(function () { eval("var"); }), "SyntaxError 1010", "eval syntax error");
check(thrown(function () { throw 5; }), "undefined 0", "thrown number");

// Functions.
function counter() {
	var count = 0;
	return function () { return ++count; };
}
var next = counter();
next();
check(next(), 2, "closure");
function variadic() { return arguments.length + ":" + Array.prototype.join.call(arguments, "-"); }
check(variadic(1, 2, 3), "3:1-2-3", "arguments");
check(Math.max.apply(null, [4, 9, 2]), 9, "apply");
check(variadic.call(null, "x"), "1:x", "call");
check(new Function("a", "b", "return a * b")(6, 7), 42, "Function constructor");
check(eval("1 + 1"), 2, "eval");
var local = "outer";
function shadow() { var local = "inner"; return eval("local"); }
check(shadow(), "inner", "direct eval sees the caller");
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.sum = function () { return this.x + this.y; };
var p = new Point(2, 5);
check(p.sum(), 7, "prototype method");
check(p instanceof Point, true, "instanceof");
check("x" in p && !("z" in p), true, "in");
check(p.hasOwnProperty("x") && !p.hasOwnProperty("sum"), true, "hasOwnProperty");
check(delete p.x && p.x === undefined, true, "delete");
function recurse(n) { return n ? recurse(n - 1) : 0; }
check(thrown(function () { recurse(100000); }), "Error 28", "out of stack");

// Arrays.
var a = [5, 1, 4];
a.push(3, 2);
check(a.length + " " + a.join(""), "5 51432", "push");
check(a.sort().join(""), "12345", "sort");
check(a.sort(function (x, y) { return y - x; }).join(""), "54321", "sort with a comparison");
check(a.reverse().join(""), "12345", "reverse");
check(a.slice(1, -1).join(""), "234", "slice");
check(a.splice(1, 2, "x").join("") + a.join(""), "231x45", "splice");
check(a.concat([6, [7]], 8).length, 7, "concat");
check("" + a.pop() + a.shift() + a.length, "512", "pop and shift");
var sparse = [];
sparse[5] = 1;
check(sparse.length + "," + sparse, "6,,,,,,1", "holes");
sparse.length = 2;
check(sparse.length + "," + (5 in sparse), "2,false", "length cuts");
check([1, [2, 3]].toString(), "1,2,3", "nested toString");
check(new Array(3).length, 3, "Array of a length");
var looped = [1];
looped[1] = looped;
check(looped.join(), "1,", "join of itself");

// Strings.
var s = "Hello, World";
check(s.length + s.charAt(4) + s.charCodeAt(0), "12o72", "charAt and charCodeAt");
check(s.indexOf("o") + "," + s.lastIndexOf("o") + "," + s.indexOf("z"), "4,8,-1", "indexOf");
check(s.substring(7) + s.substr(-5, 3) + s.slice(-5, -2), "WorldWorWor", "substrings");
check(s.toUpperCase() + s.toLowerCase(), "HELLO, WORLDhello, world", "case");
check(s.split(", ").join("|") + "abc".split("").join("."), "Hello|Worlda.b.c", "split");
check("a1b22c333".split(/\d+/).join(), "a,b,c,", "split with an expression");
check("a,b".split(/(,)/).join("|"), "a|,|b", "split keeps groups");
check(String.fromCharCode(74, 83), "JS", "fromCharCode");
check("x".bold() + "y".link("u"), "<B>x</B><A HREF=\"u\">y</A>", "HTML methods");
check("aaa".replace("a", "b"), "baa", "replace a string");
check("aaa".replace(/a/g, "b"), "bbb", "replace all");
check("John Smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "Smith, John", "replace groups");
check("abc".replace(/b/, function (m, offset) { return "[" + m + offset + "]"; }), "a[b1]c", "replace with a function");
check("x-y-z".replace(/-/g, "$$"), "x$y$z", "replace dollar");
check(escape("a b+\u00fc") + unescape("%u0041%42"), "a%20b+%FCAB", "escape");
check(encodeURIComponent("a b&\u00fc") + decodeURI("%41%3B"), "a%20b%26%C3%BCA%3B", "URI functions");
check("b".localeCompare("a") > 0, true, "localeCompare");

// Regular expressions.
var re = /(\d+)-(\d+)/g;
var text = "1-2 33-44";
var m = re.exec(text);
check(m[0] + "|" + m[1] + "|" + m.index + "|" + re.lastIndex, "1-2|1|0|3", "exec");
m = re.exec(text);
check(m[2] + "|" + RegExp.$1 + "|" + RegExp.lastMatch, "44|33|33-44", "exec again and RegExp.$1");
check(re.exec(text) === null && re.lastIndex == 0, true, "exec at the end");
check(/^abc$/im.test("x\nABC\ny"), true, "flags");
check("a1b2".match(/\d/g).join(), "1,2", "match all");
check("abc".search(/c/), 2, "search");
check(new RegExp("a+", "g").source + /x/gi, "a+/x/gi", "RegExp source and toString");
check(thrown(function () { new RegExp("("); }), "SyntaxError 5020", "bad pattern");

// Objects of other kinds.
check(Math.floor(-1.5) + Math.ceil(1.2) + Math.abs(-3) + Math.round(2.5) + Math.round(-2.5), 4, "Math rounding");
check(Math.pow(2, 10) + Math.sqrt(16), 1028, "pow and sqrt");
check(Math.random() < 1, true, "random");
check(Boolean("") + "," + new Boolean(false).valueOf() + "," + !!new Boolean(false), "false,false,true", "Boolean");
check(Object.prototype.toString.call([]), "[object Array]", "class names");
check(new Error("boom").message + "," + new TypeError("t").name, "boom,TypeError", "error kinds");
check(String(new Error(5, "five")), "Error: five", "Error toString");
var d = new Date(Date.UTC(2026, 10, 5, 19, 43, 28, 250));
check(d.getUTCFullYear() + "-" + d.getUTCMonth() + "-" + d.getUTCDate() + " " + d.getUTCDay() + " " + d.getUTCHours() + ":" + d.getUTCMinutes() +
	":" + d.getUTCSeconds() + "." + d.getUTCMilliseconds(), "2026-10-5 4 19:43:28.250", "Date parts");
check(d.toUTCString(), "Thu, 5 Nov 2026 19:43:28 UTC", "toUTCString");
check(d.getTime(), 1793907808250, "getTime");
check(Date.parse("Thu, 5 Nov 2026 19:43:28 UTC"), 1793907808000, "parse");
check(Date.parse("11/05/2026 19:43:28 GMT"), 1793907808000, "parse m/d/y");
d.setUTCMonth(0, 31);
check(d.getUTCMonth() + "/" + d.getUTCDate(), "0/31", "setUTCMonth");
var local = new Date(2026, 10, 5, 20, 43, 28);
check(local.getHours() + ":" + local.getMinutes() + " " + local.getDate(), "20:43 5", "local time");
check(Date.parse(local.toString()) == local.getTime(), true, "toString reads back");
check(typeof local.getVarDate(), "date", "getVarDate");
check(ScriptEngine() + " " + ScriptEngineMajorVersion(), "JScript 5", "ScriptEngine");

// The host's objects.
var dictionary = new ActiveXObject("Scripting.Dictionary");
dictionary.Add("one", 1);
dictionary.Add("two", 2);
dictionary.Item("three") = 3;
check(dictionary.Count + "," + dictionary.Item("two") + "," + dictionary.Exists("one"), "3,2,true", "Dictionary");
var names = [];
for (var e = new Enumerator(dictionary); !e.atEnd(); e.moveNext())
	names.push(e.item());
check(names.join(), "one,two,three", "Enumerator");
var array = new VBArray(dictionary.Items());
check(array.dimensions() + " " + array.lbound() + " " + array.ubound() + " " + array.toArray().join() + " " + array.getItem(1), "1 0 2 1,2,3 2", "VBArray");
var fso = new ActiveXObject("Scripting.FileSystemObject");
var path = fso.BuildPath(fso.GetSpecialFolder(2), "raven-jscript.txt");
var file = fso.CreateTextFile(path, true);
file.WriteLine("first");
file.Write("second");
file.Close();
var stream = fso.OpenTextFile(path, 1);
check(stream.ReadLine() + "|" + stream.ReadAll(), "first|second", "files through the host");
stream.Close();
fso.DeleteFile(path);
check(fso.FileExists(path), false, "file deleted");
check(thrown(function () { new ActiveXObject("Raven.NoSuchClass"); }), "Error 429", "unknown class");
var shell = new ActiveXObject("WScript.Shell");
check(shell.ExpandEnvironmentStrings("%NO_SUCH_RAVEN_VARIABLE%"), "%NO_SUCH_RAVEN_VARIABLE%", "WScript.Shell");
check(WScript.Arguments.length, 0, "no arguments");

if (failures)
	WScript.Echo(failures + " of " + checks + " checks failed");
else
	WScript.Echo(checks + " checks passed");
